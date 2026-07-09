#ifndef _TM_MESH_INF_H_
#define _TM_MESH_INF_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mesh_topology.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

using tm_mesh_grant_que_t = TmQue<TmMeshGrant>;
using p_tm_mesh_grant_que_t = std::shared_ptr<tm_mesh_grant_que_t>;

struct TmMeshInfApiReq
{
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
};

/*
 * Tm_mesh_inf
 *
 * master-side endpoint / NIU。
 *
 * 这层借鉴的是 gem5 SimpleNetwork endpoint 的 message-buffer 思路：
 * - endpoint 自己维护本地 pending queue / grant queue
 * - Fabric 主动从 endpoint 读取待发事务
 * - Fabric 把回包直接交回 endpoint
 *
 * 但协议语义仍然保持 AI Core 风格：
 * - RD_REQ / WR_REQ / WR_DAT 三类请求
 * - WR_REQ_RSP -> grant -> WR_DAT
 * - WR_DAT_RSP 才代表写事务真正完成
 *
 * bus_inf_ 是 NIU 对上游暴露的唯一接口：
 * - 上游可以通过 bus_inf_ 把 request 推给 NIU
 * - NIU 也通过 bus_inf_ 把 response 送回上游
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
    void tick();

    void attach_upstream(p_tm_com_inf_t inf);
    void set_master_id(uint32_t mst_id);

    uint32_t send_rd_req(uint64_t address, uint32_t size);
    uint32_t send_wr_req(uint64_t address, uint32_t size);

    bool is_request_completed(uint32_t req_id);
    bool can_send_rd_req();
    bool can_send_wr_req();

    /*
     * 从 bus_inf_ 吸收新的上游请求，并在需要时建立共享 txn_ctx_。
     * Fabric 每拍调用一次，把端口边界的新请求收进 NIU 本地 pending queue。
     */
    void ingest_upstream_requests(
        uint32_t master_port, const TmMeshTopology& topology,
        std::unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
        tm_engine::tm_time_t now);

    /*
     * Fabric 注入 request/data subnet 时使用的本地 pending 接口。
     * RD_REQ 和 WR_REQ 共用 req_pending_q_，WR_DAT 单独走 wr_dat_pending_q_。
     */
    bool has_pending_request(aic_req_type_t req_type) const;
    p_tm_pld_t peek_pending_request(aic_req_type_t req_type) const;
    void pop_pending_request(aic_req_type_t req_type);

    /* 写事务 grant 由 NIU 本地缓存，后续驱动 WR_DAT 注入 */
    bool has_pending_grant() const;
    TmMeshGrant peek_pending_grant() const;
    void pop_pending_grant();
    bool can_accept_write_grant() const;

    /*
     * Fabric 把 response 直接交回 NIU：
     * - 读响应可按 lane 乱序返回
     * - 写请求响应会顺带生成本地 grant
     * - 写完成响应会清理本地完成状态
     */
    bool accept_read_response(p_tm_pld_t rsp, uint32_t lane);
    bool accept_write_request_response(p_tm_pld_t rsp,
                                       const TmMeshGrant& grant);
    bool accept_write_data_response(p_tm_pld_t rsp);

  public:
    /* 对上游暴露的唯一接口 */
    p_tm_com_inf_t bus_inf_ = nullptr;
    /* 当前 NIU 绑定的 master_id */
    uint32_t inf_id_ = 0;

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    /* 本地待发请求：RD_REQ / WR_REQ 共用，保持提交顺序 */
    std::vector<p_tm_pld_t> req_pending_q_;
    /* 本地待发 WR_DAT，只有拿到 grant 后才允许注入 data subnet */
    std::vector<p_tm_pld_t> wr_dat_pending_q_;
    /* 写事务 grant 本地缓存 */
    p_tm_mesh_grant_que_t wr_grant_fifo_ = nullptr;

    /* 非 API 形式请求的在途跟踪，用于按 pld 指针乱序完成 */
    std::vector<std::pair<uint32_t, p_tm_pld_t>> bus_req_list_;
    /* API 形式请求的 req_id -> 完成统计 */
    std::unordered_map<uint32_t, TmMeshInfApiReq> api_req_map_;

    uint32_t req_id_ = 0;

  protected:
    size_t request_queue_capacity() const;
    size_t write_data_queue_capacity() const;
    bool front_request_matches(aic_req_type_t req_type) const;

    uint32_t request_channel(aic_req_type_t req_type) const;
    uint32_t response_channel(aic_req_type_t req_type,
                              uint32_t lane = 0) const;

    uint64_t make_txn_key(uint32_t mst_id, uint32_t gid) const;
    uint64_t make_txn_key(p_tm_pld_t pld) const;

    void track_api_request(uint32_t req_id, p_tm_pld_t req,
                           aic_req_type_t req_type);
    void retire_tracked_request(p_tm_pld_t rsp);
    bool retire_api_read_response(p_tm_pld_t rsp);
    bool retire_api_write_response(p_tm_pld_t rsp);
    bool is_api_write_request(p_tm_pld_t rsp) const;
};

using tm_mesh_inf_t = Tm_mesh_inf;
using p_tm_mesh_inf_t = std::shared_ptr<tm_mesh_inf_t>;

inline p_tm_mesh_inf_t
tm_make_mesh_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                 uint32_t inf_id, p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<Tm_mesh_inf>(name, clk, inf_id, cfg);
}

#endif  // _TM_MESH_INF_H_
