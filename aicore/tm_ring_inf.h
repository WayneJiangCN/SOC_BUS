#ifndef _TM_RING_INF_H_
#define _TM_RING_INF_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "pem_log.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

using tm_ring_topology_t = TmRingTopology;
using p_tm_ring_topology_t = std::shared_ptr<tm_ring_topology_t>;

class TmBusFlowCtrl;

struct TmRingInfApiReq
{
    // API 请求表仅服务 send_*_req()/completed()，不参与 Router 路由判断。
    PldCmd cmd = PldCmd::RD;
    p_tm_pld_t req = nullptr;
    // 多分片读响应通过 expected/seen 判断何时从 API 请求表退休。
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
};

/*
 * Master 侧网络接口单元（NIU）。
 *
 * NIU 是上游 BIU/com_inf 与本地源 Router 之间的边界，负责请求缓存、
 * 路由元数据、两阶段写匹配和 API 请求完成跟踪；逐跳方向仍由 Router 决定。
 */
class TmRingInf : public tm_engine::TmModule
{
  public:
    TmRingInf(const std::string& name, tm_engine::p_tm_clk_t clk,
                uint32_t inf_id, p_tm_ring_cfg_t cfg);
    virtual ~TmRingInf();

    void config();
    void reset();
    bool idle();

    // 连接 BIU 侧总线接口；请求从该接口进入，最终响应也从该接口返回。
    void attach(p_tm_com_inf_t inf);
    // 绑定 Master 所在端口、拓扑和共享流控对象，用于请求元数据及资源释放。
    void attach(uint32_t master_port, p_tm_ring_topology_t topology,
                std::shared_ptr<TmBusFlowCtrl> flow_ctrl);
    void set_master_id(uint32_t mst_id);

    // API 请求接口：成功时返回本地 req_id，队列满时返回 UINT32_MAX。
    uint32_t send_rd_req(uint64_t address, uint32_t size);
    uint32_t send_wr_req(uint64_t address, uint32_t size);

    bool is_request_completed(uint32_t req_id);
    // can_send 同时检查 Master OSD 和 NIU 本地命令队列容量。
    bool can_send_rd_req();
    bool can_send_wr_req();

    // recv_* 由 BIU 接口 vld 事件触发，将请求搬入 NIU 本地 FIFO。
    void recv_rd_cmd();
    void recv_wr_cmd();
    void recv_wr_dat();
    // send_* 由本地 FIFO vld 事件触发，尝试向源 Router 注入队头请求。
    void send_rd_cmd();
    void send_wr_cmd();
    void send_wr_dat();
    void send_wr_dat_rsp();
    // 接收 Router 从响应子网弹出的数据，并按响应类型完成后续处理。
    void recv_router_rsp();

    void release_read_osd();
    void release_write_osd();

  public:
    // bus_inf_ 面向 BIU，router_inf_ 面向本地 Router，二者职责不可互换。
    p_tm_com_inf_t bus_inf_ = nullptr;
    p_tm_com_inf_t router_inf() const;
    uint32_t inf_id_ = 0;

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_cfg_t cfg_ = nullptr;

    uint32_t master_port_ = 0;
    p_tm_ring_topology_t topology_ = nullptr;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_ = nullptr;
    p_tm_com_inf_t router_inf_ = nullptr;
    // 以下 tm_que 才是 NIU 的真实缓存；TmInf 主要表达模块端口和握手延迟。
    p_tm_com_que_t rd_cmds_ = nullptr;
    p_tm_com_que_t wr_cmds_ = nullptr;
    p_tm_com_que_t wr_data_ = nullptr;
    p_tm_com_que_t wr_dat_rsp_q_ = nullptr;

    // req_map_ 跟踪 API 完成状态；pending_writes_ 保存两阶段写所需的原始数据。
    std::unordered_map<uint32_t, TmRingInfApiReq> req_map_;
    std::unordered_map<uint64_t, p_tm_pld_t> pending_writes_;
    std::unordered_map<uint64_t, TmRingRdRspState> rd_rsp_states_;

    uint32_t req_id_ = 0;
    // Master OSD 在 RD/WR 请求成功注入 Ring 时增加，在最终响应完成时释放。
    uint32_t rd_outstanding_ = 0;
    uint32_t wr_outstanding_ = 0;
    p_logger_t log_ = nullptr;

  protected:
    // 将 BIU/Ring 通道号映射集中在此处，避免调用方自行拼接 lane。
    uint32_t response_channel(PldCmd cmd, uint32_t lane = 0) const;

    void recv_cmd(PldCmd cmd);
    void send_cmd(PldCmd cmd);
    // 返回 false 表示下游暂不可接收；调用方必须保留原队头，不能丢包。
    bool recv_rsp(p_tm_pld_t rsp);
    bool accept_rd_rsp(p_tm_pld_t rsp);
    // 写命令响应仅作为 grant，匹配原始写请求后生成独立 WR_DAT 包。
    bool accept_wr_req_rsp(p_tm_pld_t rsp);
    bool accept_wr_dat_rsp(p_tm_pld_t rsp);
    p_tm_com_que_t req_queue(PldCmd cmd) const;
    bool issue_cmd_to_ring(PldCmd cmd, p_tm_pld_t pld);
    // 填写 Target、源节点和目的节点；具体 EAST/WEST 方向由 Router 逐跳计算。
    void prepare_request_metadata(p_tm_pld_t pld, PldCmd cmd);
    bool can_reserve_master_osd(PldCmd cmd) const;
    // 完整事务完成后释放 Master OSD 和 Target credit；WR_RSP 不是最终写完成。
    void complete_rsp(p_tm_pld_t rsp);

    void track_request(uint32_t req_id, p_tm_pld_t req, PldCmd cmd);
    p_tm_pld_t make_write_data(p_tm_pld_t grant);
    bool retire_read_response(p_tm_pld_t rsp);
    bool retire_write_response(p_tm_pld_t rsp);
};

using tm_ring_inf_t = TmRingInf;
using p_tm_ring_inf_t = std::shared_ptr<tm_ring_inf_t>;

inline p_tm_ring_inf_t
tm_make_ring_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                 uint32_t inf_id, p_tm_ring_cfg_t cfg)
{
    return std::make_shared<TmRingInf>(name, clk, inf_id, cfg);
}

#endif  // _TM_RING_INF_H_
