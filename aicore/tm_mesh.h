#ifndef _TM_MESH_H_
#define _TM_MESH_H_

#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "cfg.h"
#include "pem_biu.h"
#include "tm_bus_flow_ctrl.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mesh_topology.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

class Tm_mesh_inf;
using tm_mesh_inf_t = Tm_mesh_inf;
using p_tm_mesh_inf_t = std::shared_ptr<tm_mesh_inf_t>;

class TmMeshRouter;
using tm_mesh_router_t = TmMeshRouter;
using p_tm_mesh_router_t = std::shared_ptr<tm_mesh_router_t>;

class TmMeshLink;
using tm_mesh_link_t = TmMeshLink;
using p_tm_mesh_link_t = std::shared_ptr<tm_mesh_link_t>;

class TmMeshTargetPort;
using tm_mesh_target_port_t = TmMeshTargetPort;
using p_tm_mesh_target_port_t = std::shared_ptr<tm_mesh_target_port_t>;

/*
 * TmMeshFabric
 *
 * 轻量级 AI Core mesh-lite 互连的顶层共享容器。
 *
 * 这里不直接实现每一层的细节行为，而是持有：
 * - 左侧 master-side NIU
 * - 中间 Router / Link
 * - 右侧 TargetPort
 *
 * Fabric 自己主要负责：
 * - 维护共享事务上下文 txn_ctx_
 * - 驱动每拍 tick 主流程
 * - 调用拓扑和 target-level 流控
 */
class TmMeshFabric : public tm_engine::TmModule
{
  public:
    TmMeshFabric();
    TmMeshFabric(tm_engine::p_tm_clk_t clk, p_tm_mesh_cfg_t cfg);
    virtual ~TmMeshFabric();

    // config() 创建 NIU / Router / Link / TargetPort 对象。
    // reset() 清空所有局部状态，供仿真起始或重新运行使用。
    virtual void config();
    virtual void build();
    virtual void reset();
    virtual bool idle();

    // 每拍主调度入口：
    // target rsp ingress -> master req ingress -> mesh advance -> target issue。
    virtual void tick();

    // master / target 接入辅助接口。
    // 这样 BIU、裸 com_inf、TmMem 都可以复用同一套 fabric。
    virtual void attach_master(uint32_t idx, p_tm_mesh_inf_t inf);
    virtual void attach_master(p_tm_mesh_inf_t inf);
    virtual void attach_master(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_master(uint32_t idx, p_pem_biu_t biu);
    virtual void attach_master(p_pem_biu_t biu);
    virtual void attach_target(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_mem_t mem);
    virtual void bind_master_id(uint32_t port_id, uint32_t mst_id);

    virtual uint32_t send_rd_req(uint32_t master_port, uint64_t address,
                                 uint32_t size);
    virtual uint32_t send_wr_req(uint32_t master_port, uint64_t address,
                                 uint32_t size);
    virtual bool completed(uint32_t master_port, uint32_t req_id);
    virtual bool canSendRdReq(uint32_t master_port);
    virtual bool canSendWrReq(uint32_t master_port);

  public:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    // 左侧源端点：每个 master 一个 NIU。
    std::vector<p_tm_mesh_inf_t> master_nius_;
    // mesh 中的 router，对应 topology node id。
    std::vector<p_tm_mesh_router_t> routers_;
    // 有向物理链路，key = (src_router, dst_router)。
    std::unordered_map<uint64_t, p_tm_mesh_link_t> links_;
    // 右侧目标端点：每个 target 一个 TargetPort。
    std::vector<p_tm_mesh_target_port_t> target_ports_;

  protected:
    // 全局事务上下文表。
    // request/data/response 在不同模块之间流动时，都靠它找到：
    // - src/dst node
    // - target id
    // - 当前事务状态
    // - 是否已完成/是否已释放 credit
    std::unordered_map<uint64_t, TmMeshTxnCtx> txn_ctx_;

    // target 发射节流时间。
    // 分别约束 RD_REQ / WR_REQ / WR_DAT 三类请求在 target 侧的发射间隔。
    std::vector<tm_engine::tm_time_t> next_rd_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_req_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_dat_issue_time_;

    // 缓存拓扑维度，避免热路径里重复计算。
    uint32_t mesh_router_count_ = 0;
    uint32_t mesh_rows_ = 1;
    uint32_t mesh_cols_ = 1;
    uint32_t mesh_link_latency_ = 1;

    // topology 决定“该往哪走”。
    // flow_ctrl 决定“target 侧能不能接”。
    TmMeshTopology topology_;
    TmBusFlowCtrl flow_ctrl_;

  protected:
    // 请求侧主路径：
    // 1) 从 NIU 吸收请求到本地 pending
    // 2) 注入 source router
    // 3) 在 router/link 上做一次粗粒度前推
    void recv_master_reqs();
    void inject_mesh_reqs();
    void inject_mesh_req(uint32_t master_port, aic_req_type_t req_type);
    void advance_mesh_routers();

    // 已经到达目标 router 的包，尝试送到 target-side endpoint。
    // 这里仍然受 target-level flow control 约束。
    void send_target_reqs();
    void send_target_req(uint32_t target_id, aic_req_type_t req_type);

    // target 响应先回注到目标节点 router，再沿同一套 router/link 路径回程。
    void recv_target_rsps();
    void recv_target_rd_rsp(uint32_t target_id, uint32_t lane);
    void recv_target_wr_req_rsp(uint32_t target_id);
    void recv_target_wr_dat_rsp(uint32_t target_id);

    // queue 辅助函数：屏蔽不同 traffic class 实际存放在哪个对象里。
    p_tm_com_que_t get_target_fifo(uint32_t target_id,
                                   aic_req_type_t req_type) const;
    p_tm_com_que_t get_mesh_req_fifo(uint32_t router_id,
                                     aic_req_type_t req_type) const;
    p_tm_com_que_t get_mesh_rd_rsp_fifo(uint32_t router_id,
                                        uint32_t lane) const;
    p_tm_com_que_t get_mesh_wr_req_rsp_fifo(uint32_t router_id) const;
    p_tm_com_que_t get_mesh_wr_dat_rsp_fifo(uint32_t router_id) const;

    // 链路查询辅助函数。
    // 调用侧只关心 src/dst router，不关心 links_ 的内部组织方式。
    p_tm_mesh_link_t get_mesh_link(uint32_t src_router_id,
                                   uint32_t dst_router_id) const;
    uint64_t make_link_key(uint32_t src_router_id, uint32_t dst_router_id) const;

    // 整张 mesh 里统一使用 (master id, gid) 作为事务 key，
    // 这样 NIU / Router / TargetPort / 回包路径都能查到同一笔事务。
    uint64_t make_txn_key(uint32_t mst_id, uint32_t gid) const;
    uint64_t make_txn_key(p_tm_pld_t pld) const;
};

using tm_mesh_fabric_t = TmMeshFabric;
using p_tm_mesh_fabric_t = std::shared_ptr<TmMeshFabric>;

inline p_tm_mesh_cfg_t
tm_make_mesh_cfg()
{
    return std::make_shared<tm_mesh_cfg_t>();
}

inline p_tm_mesh_fabric_t
tm_make_mesh(tm_engine::p_tm_clk_t clk, p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<TmMeshFabric>(clk, cfg);
}

#endif  // _TM_MESH_H_
