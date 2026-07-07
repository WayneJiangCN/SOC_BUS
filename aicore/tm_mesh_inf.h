#ifndef _TM_MESH_INF_H_
#define _TM_MESH_INF_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "tm_bus_flow_ctrl.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mesh_topology.h"
#include "tm_mesh_types.h"
#include "tm_pld.h"
#include "tm_que.h"

struct TmMeshInfApiReq
{
    /* 记录通过本地 API 发出的请求类型和完成进度。 */
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
};

/*
 * Tm_mesh_inf:
 * master-side NIU（Network Interface Unit）。
 *
 * 它只负责单个 master 自己本地的事情：
 * 1. 从上游接口或本地 API 接收请求；
 * 2. 用本地 FIFO 暂存 RD_REQ / WR_REQ / WR_DAT；
 * 3. 维护写事务 grant；
 * 4. 接收从 mesh 回来的响应，并在本地排队后再送回上游。
 *
 * 它不负责共享 mesh 网络本体：
 * - 不做逐跳路由；
 * - 不做 target 流控；
 * - 不持有 mesh 内部 FIFO。
 * 这些都留在 TmMeshFabric 中。
 */
class Tm_mesh_inf : public tm_engine::TmModule
{
  public:
    Tm_mesh_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                uint32_t inf_id, p_tm_mesh_cfg_t cfg);
    virtual ~Tm_mesh_inf();

    void config();
    void reset();
    bool idle();

    void connect_upstream(p_tm_com_inf_t inf);
    void bind_master_id(uint32_t mst_id);

    uint32_t send_rd_req(uint64_t address, uint32_t size);
    uint32_t send_wr_req(uint64_t address, uint32_t size);

    bool completed(uint32_t req_id);
    bool canSendRdReq();
    bool canSendWrReq();

    void ingest_upstream_reqs(uint32_t master_port,
                              const TmMeshTopology& topology,
                              std::unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
                              tm_engine::tm_time_t now);

    bool has_req(aic_req_type_t req_type) const;
    p_tm_pld_t front_req(aic_req_type_t req_type) const;
    void pop_req(aic_req_type_t req_type);

    bool has_grant() const;
    const TmMeshGrant& front_grant() const;
    void pop_grant();

    bool can_accept_rd_rsp(uint32_t lane) const;
    bool can_accept_wr_req_rsp() const;
    bool can_accept_wr_dat_rsp() const;
    bool can_accept_grant() const;

    bool accept_rd_rsp(p_tm_pld_t rsp, uint32_t lane);
    bool accept_wr_req_rsp(p_tm_pld_t rsp, const TmMeshGrant& grant);
    bool accept_wr_dat_rsp(p_tm_pld_t rsp);

    void service_rsp_outputs(const TmBusFlowCtrl& flow_ctrl,
                             const std::unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
                             tm_engine::tm_time_t now);

  public:
    /* 对外可见的 master 侧接口。 */
    p_tm_com_inf_t bus_inf_ = nullptr;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    /*
     * inf_id_ 表示这个 NIU 对应的 master_id。
     * 它会写入 pld->mst_id，后续 fabric / target / response path
     * 都靠这个字段识别这笔事务属于哪个 master。
     */
    uint32_t inf_id_ = 0;

  private:
    p_tm_com_que_t req_fifo(aic_req_type_t req_type) const;
    uint64_t make_txn_key(uint32_t mst_id, uint32_t gid) const;
    uint64_t make_txn_key(p_tm_pld_t pld) const;
    bool retire_api_req(p_tm_pld_t rsp);

  private:
    uint32_t req_id_ = 0;

    /* 本地请求 FIFO：分别缓存读请求、写请求头和写数据。 */
    p_tm_com_que_t rd_req_fifo_ = nullptr;
    p_tm_com_que_t wr_req_fifo_ = nullptr;
    p_tm_com_que_t wr_dat_fifo_ = nullptr;

    /* 本地响应 FIFO：回到 source NIU 后，先排队再送上游。 */
    std::vector<p_tm_com_que_t> rd_rsp_fifo_;
    p_tm_com_que_t wr_req_rsp_fifo_ = nullptr;
    p_tm_com_que_t wr_dat_rsp_fifo_ = nullptr;

    /* 写事务 grant FIFO：WR_DAT 必须严格匹配队头 grant 才能继续发送。 */
    std::deque<TmMeshGrant> wr_grant_fifo_;

    /*
     * 仅用于本地 API 风格：
     * send_rd_req()/send_wr_req() 发出的请求不会通过上游 bus_inf_ 回调完成，
     * 因此需要一张小表记录 req_id 是否已经完成。
     */
    std::unordered_map<uint32_t, TmMeshInfApiReq> api_req_map_;

    /* 本地响应向上游发送时的节拍控制。 */
    std::vector<tm_engine::tm_time_t> next_rd_rsp_issue_time_;
    tm_engine::tm_time_t next_wr_req_rsp_issue_time_ = 0;
    tm_engine::tm_time_t next_wr_dat_rsp_issue_time_ = 0;
};

using tm_mesh_niu_t = Tm_mesh_inf;
using p_tm_mesh_inf_t = std::shared_ptr<tm_mesh_niu_t>;

inline p_tm_mesh_inf_t
tm_make_mesh_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                 uint32_t inf_id, p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<Tm_mesh_inf>(name, clk, inf_id, cfg);
}

#endif  // _TM_MESH_INF_H_
