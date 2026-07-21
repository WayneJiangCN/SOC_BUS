#ifndef _TM_RING_H_
#define _TM_RING_H_

#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "cfg.h"
#include "pem_log.h"
#include "tm_bus_flow_ctrl.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

class PemBiu;
using p_tm_ring_biu_t = std::shared_ptr<PemBiu>;

class TmRingInf;
using tm_ring_inf_t = TmRingInf;
using p_tm_ring_inf_t = std::shared_ptr<tm_ring_inf_t>;

class TmRingRouter;
using tm_ring_router_t = TmRingRouter;
using p_tm_ring_router_t = std::shared_ptr<tm_ring_router_t>;

class TmRingLink;
using tm_ring_link_t = TmRingLink;
using p_tm_ring_link_t = std::shared_ptr<tm_ring_link_t>;

class TmRingTargetPort;
using tm_ring_target_port_t = TmRingTargetPort;
using p_tm_ring_target_port_t = std::shared_ptr<tm_ring_target_port_t>;

struct TmRingLinkStallBreakdown
{
    uint64_t serialization_busy = 0;
    uint64_t inflight_limit = 0;
    uint64_t link_fifo_full = 0;
    uint64_t bubble_reserved = 0;
    uint64_t downstream_fifo_full = 0;

    uint64_t total() const
    {
        return serialization_busy + inflight_limit + link_fifo_full +
               bubble_reserved + downstream_fifo_full;
    }
};

/*
 * Ring 互连顶层模型。
 *
 * Fabric 负责持有 Master NIU、Router、Link 和 TargetPort 等拓扑对象。
 * 细粒度队列事件由各子模块自行处理，Fabric 只负责创建、连接、复位和查询状态。
 */
class TmRingFabric : public tm_engine::TmModule
{
  public:
    TmRingFabric();
    TmRingFabric(tm_engine::p_tm_clk_t clk, p_tm_ring_cfg_t cfg);
    virtual ~TmRingFabric();

    virtual void config();
    virtual void build();
    virtual void reset();
    // 仅当 NIU、Router、Link、TargetPort 和流控相关队列全部清空时返回 true。
    virtual bool idle();

    // 将外部 Master 接入指定端口；不同重载分别适配 NIU、BIU 和裸 TmInf。
    virtual void attach_master(uint32_t idx, p_tm_ring_inf_t inf);
    virtual void attach_master(p_tm_ring_inf_t inf);
    virtual void attach_master(uint32_t idx, p_tm_ring_biu_t biu);
    virtual void attach_master(uint32_t idx, p_tm_com_inf_t inf);
    // 将外部存储接口或 TmMem 后端接入指定 TargetPort。
    virtual void attach_target(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_mem_t mem);
    virtual void bind_master_id(uint32_t port_id, uint32_t mst_id);

    // API 路径直接向指定 Master NIU 投递事务，返回值是该 NIU 本地请求号。
    virtual uint32_t send_rd_req(uint32_t master_port, uint64_t address,
                                 uint32_t size);
    virtual uint32_t send_wr_req(uint32_t master_port, uint64_t address,
                                 uint32_t size);
    // completed 只查询请求状态，不会推进仿真或主动消费响应。
    virtual bool completed(uint32_t master_port, uint32_t req_id);
    virtual bool canSendRdReq(uint32_t master_port);
    virtual bool canSendWrReq(uint32_t master_port);
    uint64_t global_osd_stalls() const;
    uint64_t target_slot_stalls() const;
    uint64_t bandwidth_token_stalls() const;
    TmRingLinkStallBreakdown ring_link_stall_breakdown() const;
    uint64_t ring_link_stalls() const;

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_cfg_t cfg_ = nullptr;

    // Fabric 拥有所有子模块实例，负责生命周期和连接关系，不参与逐包仲裁。
    std::vector<p_tm_ring_inf_t> master_nius_;
    std::vector<p_tm_ring_router_t> routers_;
    std::unordered_map<uint64_t, p_tm_ring_link_t> links_;
    std::vector<p_tm_ring_target_port_t> target_ports_;

    uint32_t ring_router_count_ = 0;
    uint32_t ring_link_latency_ = 1;

    // Topology 负责地址/节点映射，FlowCtrl 负责 Target credit、token 和 OSD。
    std::shared_ptr<TmRingTopology> topology_;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_;
    p_logger_t log_ = nullptr;

  protected:
    // config() 的构建阶段：先生成拓扑与资源，再创建各类子模块。
    void init_topology_and_flow_ctrl();
    void clear_components();
    void create_master_nius();
    void create_target_ports();
    void create_routers();
    void create_links();
    void create_link(uint32_t router_id, TmRingPortDir out_dir);

    // config() 和 attach API 的连接阶段：建立 Master、Router、Link、Target 的端口关系。
    void bind_master_nius();
    void attach_routers();
    void attach_links();
    void bind_target_ports();
    void bind_master_niu(uint32_t idx, p_tm_ring_inf_t inf);

    // 根据 Router 和输入方向取得 Link 的下游端口，用于完成逐跳连接。
    p_tm_com_inf_t get_router_port_inf(uint32_t router_id,
                                        TmRingPortDir in_dir) const;

    p_tm_ring_link_t get_ring_link(uint32_t src_router_id,
                                   TmRingPortDir src_dir,
                                   uint32_t dst_router_id,
                                   TmRingPortDir dst_dir) const;
    // Link key 同时编码源/目的 Router 和方向，避免双向链路发生键冲突。
    uint64_t make_link_key(uint32_t src_router_id, TmRingPortDir src_dir,
                           uint32_t dst_router_id, TmRingPortDir dst_dir) const;

};

using tm_ring_fabric_t = TmRingFabric;
using p_tm_ring_fabric_t = std::shared_ptr<TmRingFabric>;

inline p_tm_ring_cfg_t
tm_make_ring_cfg()
{
    return std::make_shared<tm_ring_cfg_t>();
}

inline p_tm_ring_fabric_t
tm_make_ring(tm_engine::p_tm_clk_t clk, p_tm_ring_cfg_t cfg)
{
    return std::make_shared<TmRingFabric>(clk, cfg);
}

#endif  // _TM_RING_H_
