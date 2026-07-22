#ifndef _TM_RING_ROUTER_H_
#define _TM_RING_ROUTER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "pem_log.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_pld.h"
#include "tm_ring_link.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

struct TmRingCandidate
{
    // Candidate 只保存本次仲裁的局部结果，不写入 payload 的逐跳方向状态。
    p_tm_pld_t pld = nullptr;
    TmRingPortDir in_dir = TmRingPortDir::LOCAL;
    TmRingPortDir out_dir = TmRingPortDir::LOCAL;
    uint32_t slot_id = 0;
};
using tm_ring_candidate_t = TmRingCandidate;
using p_tm_ring_candidate_t = std::shared_ptr<tm_ring_candidate_t>;

class TmRingRouter : public tm_engine::TmModule
{
  public:
    TmRingRouter();
    TmRingRouter(const std::string& name, tm_engine::p_tm_clk_t clk,
                 p_tm_ring_cfg_t cfg);
    ~TmRingRouter();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_ring_cfg_t cfg);
    void reset();
    bool idle() const;
    // 绑定当前 Router 的拓扑位置和两个方向的输出 Link。
    void attach(uint32_t router_id,
                std::shared_ptr<TmRingTopology> topology,
                p_tm_ring_link_t east_link, p_tm_ring_link_t west_link);
    // LOCAL 端口可挂多个 Master/Target，实际弹出方向由 payload ID 决定。
    void bind_local_master(uint32_t master_port, p_tm_com_inf_t inf);
    void bind_local_target(uint32_t target_id, p_tm_com_inf_t inf);

    p_tm_com_inf_t port_inf(TmRingPortDir in_dir) const;

  private:
    // 六个事件入口按输入方向和子网拆分，避免无关流量触发同一处理函数。
    void route_local_request();
    void route_east_request();
    void route_west_request();
    void route_local_response();
    void route_east_response();
    void route_west_response();
    void recv_east_input();
    void recv_west_input();
    void recv_port_input(TmRingPortDir in_dir);
    void recv_port_subnet(TmRingPortDir in_dir, TmRingSubnet subnet);
    void advance_local_input(TmRingSubnet subnet);
    void advance_east_input(TmRingSubnet subnet);
    void advance_west_input(TmRingSubnet subnet);
    void advance_input(TmRingPortDir in_dir, TmRingSubnet subnet);
    // 扫描指定输入端口，在该子网的固定 slot 空间内执行 Round-Robin。
    p_tm_ring_candidate_t select_input_candidate(TmRingPortDir in_dir,
                                                 TmRingSubnet subnet);
    p_tm_ring_candidate_t select_buffered_candidate(TmRingPortDir in_dir,
                                                    TmRingSubnet subnet);
    // 只有下游成功接收后才更新 RR 指针并 pop 输入端事务。
    void commit_packet(TmRingSubnet subnet, p_tm_ring_candidate_t candidate);
    // 请求路由到 Target 节点，响应路由回原 Master 节点。
    TmRingPortDir resolve_route(p_tm_pld_t pld);
    bool route_ready(p_tm_ring_candidate_t candidate);
    bool route_packet(p_tm_ring_candidate_t candidate);
    bool local_ready(p_tm_pld_t pld);
    bool route_local(p_tm_pld_t pld);
    p_tm_com_inf_t local_inf(p_tm_pld_t pld) const;
    uint32_t local_channel(p_tm_pld_t pld) const;
    p_tm_ring_link_t output_link(TmRingPortDir out_dir) const;

    uint32_t traffic_slot_count() const;
    // 将命令类型和额外 RD_RSP lane 展开成稳定仲裁 slot。
    void decode_slot(uint32_t slot_class, uint32_t& traffic_class,
                     uint32_t& rsp_lane);
    uint32_t packet_channel(uint32_t traffic_class, uint32_t lane = 0) const;
    p_tm_com_inf_t inf_for_class(TmRingPortDir in_dir,
                                 uint32_t traffic_class,
                                 uint32_t lane = 0) const;
    p_tm_com_que_t input_queue(TmRingPortDir in_dir,
                               TmRingSubnet subnet) const;
    uint32_t slot_class_for_packet(p_tm_pld_t pld) const;

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_cfg_t cfg_ = nullptr;

    // EAST/WEST 输入端口仅表达 Link 到站握手，不保存完整事务生命周期。
    std::vector<p_tm_com_inf_t> port_infs_;
    // EAST/WEST 输入缓存承接已到站 packet，让 Link 可以尽快释放 in-flight。
    std::vector<p_tm_com_que_t> req_input_qs_;
    std::vector<p_tm_com_que_t> rsp_input_qs_;

    // 每个输出/输入方向、每个子网独立保存上次 grant，保证长期公平性。
    std::vector<uint32_t> output_rr_ptr_;
    std::vector<uint32_t> input_rr_ptr_;
    uint32_t router_id_ = 0;
    std::shared_ptr<TmRingTopology> topology_ = nullptr;
    p_tm_ring_link_t east_link_ = nullptr;
    p_tm_ring_link_t west_link_ = nullptr;
    // LOCAL 端口按逻辑 ID 保存接口，响应可准确返回发起事务的 Master。
    std::vector<p_tm_com_inf_t> local_master_infs_;
    std::vector<p_tm_com_inf_t> local_target_infs_;
    p_logger_t log_ = nullptr;
};

using tm_ring_router_t = TmRingRouter;
using p_tm_ring_router_t = std::shared_ptr<tm_ring_router_t>;

inline p_tm_ring_router_t
tm_make_ring_router(const std::string& name, tm_engine::p_tm_clk_t clk,
                    p_tm_ring_cfg_t cfg)
{
    return std::make_shared<TmRingRouter>(name, clk, cfg);
}

#endif  // _TM_RING_ROUTER_H_
