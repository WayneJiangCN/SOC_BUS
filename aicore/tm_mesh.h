#ifndef _TM_MESH_H_
#define _TM_MESH_H_

#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "cfg.h"
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
 * 整张 mesh-lite 互连的顶层容器。
 *
 * 这层本身不承担某个具体 router 或 link 的微结构细节，
 * 它主要负责：
 * 1. 持有 master 侧 NIU、router、link、target port。
 * 2. 维护整张网络共享的事务上下文 txn_ctx_。
 * 3. 在每个 tick 中按固定顺序推进请求、响应和 target 发射。
 * 4. 调用 topology 和 flow-control，决定路由、credit、busy time。
 */
class TmMeshFabric : public tm_engine::TmModule
{
  public:
    TmMeshFabric();
    TmMeshFabric(tm_engine::p_tm_clk_t clk, p_tm_mesh_cfg_t cfg);
    virtual ~TmMeshFabric();

    /* 初始化所有子模块并完成拓扑建连。 */
    virtual void config();
    virtual void build();
    /* 清空所有队列、链路在途包和共享事务状态。 */
    virtual void reset();
    /* 判断整张 mesh 是否已经没有在途事务。 */
    virtual bool idle();

    /*
     * 每拍主流程：
     * 1. 先让 NIU 自己做本地 tick；
     * 2. target 响应注入本地 router；
     * 3. master 请求进入 NIU；
     * 4. NIU 本地 pending 请求注入 source router；
     * 5. router/link 内部前推；
     * 6. 到达 target 侧的请求再发给 target。
     */
    virtual void tick();

    /* 绑定 master/target 侧对象到 fabric。 */
    virtual void attach_master(uint32_t idx, p_tm_mesh_inf_t inf);
    virtual void attach_master(p_tm_mesh_inf_t inf);
    virtual void attach_master(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_mem_t mem);
    /* 绑定 port_id 对应的 master_id。 */
    virtual void bind_master_id(uint32_t port_id, uint32_t mst_id);

    /* API 风格的直接发请求接口。 */
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

    /* 每个 master 对应一个 master-side NIU。 */
    std::vector<p_tm_mesh_inf_t> master_nius_;
    /* 拓扑中的每个 router 节点。 */
    std::vector<p_tm_mesh_router_t> routers_;
    /*
     * 有向物理链路表。
     * key 由 (src_router, src_dir, dst_router, dst_dir) 编码而成。
     */
    std::unordered_map<uint64_t, p_tm_mesh_link_t> links_;
    /* 每个 target 对应一个 target-side endpoint。 */
    std::vector<p_tm_mesh_target_port_t> target_ports_;

  protected:
    /*
     * 全局事务上下文表。
     * 这里记录一笔事务从进入 NIU、进入 mesh、到达 target、
     * 等待响应到最终完成的完整生命周期状态。
     */
    std::unordered_map<uint64_t, TmMeshTxnCtx> txn_ctx_;

    /* target 侧每类请求口下一次允许发射的时间。 */
    std::vector<tm_engine::tm_time_t> next_rd_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_req_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_dat_issue_time_;

    /* mesh 基本拓扑参数，config() 后固定。 */
    uint32_t mesh_router_count_ = 0;
    uint32_t mesh_rows_ = 1;
    uint32_t mesh_cols_ = 1;
    uint32_t mesh_link_latency_ = 1;

    /* 负责地址解码、node 映射和下一跳计算。 */
    TmMeshTopology topology_;
    /* 负责 target 侧 credit、token、busy time 建模。 */
    TmBusFlowCtrl flow_ctrl_;

  protected:
    /* 从 master 侧 bus_inf_ 吸收请求到 NIU 本地 pending queue。 */
    void recv_master_reqs();
    /* 将 NIU 本地 pending 请求注入 source router。 */
    void inject_mesh_reqs();
    void inject_mesh_req(uint32_t master_port, aic_req_type_t req_type);
    /* 将已经到达目的端的 in-flight link 包灌回目标 router 输入口。 */
    void drain_ready_links();
    /* 推进 router -> link -> router / target / master 的主路径。 */
    void advance_mesh_routers();

    /* 将 target local queue 中的请求真正发往 target/TmMem。 */
    void send_target_reqs();
    void send_target_req(uint32_t target_id, aic_req_type_t req_type);

    /* 从 target 侧接口收响应，并注入目标 router 的 LOCAL 输入口。 */
    void recv_target_rsps();
    void recv_target_rd_rsp(uint32_t target_id, uint32_t lane);
    void recv_target_wr_req_rsp(uint32_t target_id);
    void recv_target_wr_dat_rsp(uint32_t target_id);

    /* 统一按 traffic class 访问 target/router 中的本地队列。 */
    p_tm_com_que_t get_target_fifo(uint32_t target_id,
                                   aic_req_type_t req_type) const;
    p_tm_com_que_t get_mesh_req_fifo(uint32_t router_id, TmMeshPortDir in_dir,
                                     aic_req_type_t req_type) const;
    p_tm_com_que_t get_mesh_rd_rsp_fifo(uint32_t router_id, TmMeshPortDir in_dir,
                                        uint32_t lane) const;
    p_tm_com_que_t get_mesh_wr_req_rsp_fifo(uint32_t router_id,
                                            TmMeshPortDir in_dir) const;
    p_tm_com_que_t get_mesh_wr_dat_rsp_fifo(uint32_t router_id,
                                            TmMeshPortDir in_dir) const;

    /* 查找某条明确的 port-to-port 有向链路。 */
    p_tm_mesh_link_t get_mesh_link(uint32_t src_router_id, TmMeshPortDir src_dir,
                                   uint32_t dst_router_id,
                                   TmMeshPortDir dst_dir) const;
    uint64_t make_link_key(uint32_t src_router_id, TmMeshPortDir src_dir,
                           uint32_t dst_router_id, TmMeshPortDir dst_dir) const;

    /* mesh 内统一事务键：高 32 位是 mst_id，低 32 位是 gid。 */
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
