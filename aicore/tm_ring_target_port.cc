#include "tm_ring_target_port.h"

#include <algorithm>

#include "tm_bus_flow_ctrl.h"
#include "tm_pld.h"

using namespace tm_engine;

TmRingTargetPort::TmRingTargetPort()
{
}

TmRingTargetPort::TmRingTargetPort(const std::string& name, p_tm_clk_t clk,
                                   p_tm_ring_target_cfg_t cfg,
                                   uint32_t rd_rsp_port_num,
                                   uint32_t rsp_phys_lanes,
                                   TmRingRspLaneSelect rsp_lane_select)
    : TmModule(name)
{
    config(name, clk, cfg, rd_rsp_port_num, rsp_phys_lanes,
           rsp_lane_select);
}

TmRingTargetPort::~TmRingTargetPort()
{
}

void
TmRingTargetPort::config(const std::string& name, p_tm_clk_t clk,
                         p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t rsp_phys_lanes,
                         TmRingRspLaneSelect rsp_lane_select)
{
    name_ = name;
    this->name(name_);
    clk_ = clk;
    cfg_ = cfg;
    rd_rsp_port_num_ = rd_rsp_port_num;
    rsp_phys_lanes_ = tm_ring_rsp_phys_lane_count(rsp_phys_lanes);
    rsp_lane_select_ = rsp_lane_select;
    log_para_t log_para(name_ + ".log");
    log_ = pem_log::create_logger(log_para);
    PEM_LOG_INFO(log_, "[{0:d}] config rd_rsp_ports:{1:d}",
                 time(), rd_rsp_port_num_);

    // inf_ 面向 Memory，通道布局沿用 BIU/TmMem 的请求和响应约定。
    uint32_t chan_num = std::max<uint32_t>(
        tm_ring_cmd_bus_channel(PldCmd::WR_DAT) + 1,
        tm_ring_rd_rsp_bus_channel(0) + rd_rsp_port_num_);
    inf_ = tm_make_com_inf(clk_, name_ + "_inf", tm_ring_inf_depth());
    inf_->set_chan_num(chan_num);
    // 三个回调共享 inf_->vld，但分别只处理读响应、写 grant 和写完成响应。
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_rd_cmd_rsp), inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_wr_cmd_rsp), inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_wr_dat_rsp), inf_->vld);

    // ring_inf_ 面向本地 Router，同时承载 Ring 请求输入和响应输出。
    ring_inf_ = tm_make_com_inf(clk_, name_ + "_ring_inf",
                                tm_ring_inf_depth());
    ring_inf_->set_chan_num(tm_ring_packet_channel_count(rd_rsp_port_num_));
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_ring_req),
                 ring_inf_->vld);

    // Target 本地请求 FIFO 是 Router 与 Memory 处理速率之间的弹性缓存。
    rd_req_q_ = tm_make_com_que(clk_, name_ + "_rd_req_q",
                                cfg_->rd_req_fifo_depth);
    wr_req_q_ = tm_make_com_que(clk_, name_ + "_wr_req_q",
                                cfg_->wr_req_fifo_depth);
    wr_dat_q_ = tm_make_com_que(clk_, name_ + "_wr_dat_q",
                                cfg_->wr_dat_fifo_depth);

    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::send_rd_cmd), rd_req_q_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::send_wr_cmd), wr_req_q_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::send_wr_dat), wr_dat_q_->vld);

    reset();
}

void
TmRingTargetPort::reset()
{
    // 清空接口、请求 FIFO 和发射时间，保证 reset 后不会延续旧事务节奏。
    if (inf_ != nullptr) {
        inf_->reset();
    }
    if (ring_inf_ != nullptr) {
        ring_inf_->reset();
    }
    if (rd_req_q_ != nullptr) {
        rd_req_q_->clear();
    }
    if (wr_req_q_ != nullptr) {
        wr_req_q_->clear();
    }
    if (wr_dat_q_ != nullptr) {
        wr_dat_q_->clear();
    }
    next_req_issue_time_ = {0, 0, 0};
    next_rd_rsp_issue_time_.assign(rd_rsp_port_num_, 0);
    next_wr_req_rsp_issue_time_ = 0;
    next_wr_dat_rsp_issue_time_ = 0;
    next_rsp_phys_lane_ = 0;
}

void
TmRingTargetPort::attach(uint32_t target_id,
                         std::shared_ptr<TmBusFlowCtrl> flow_ctrl)
{
    // target_id 用于索引该 Target 独立的 credit、token 和 outstanding 状态。
    target_id_ = target_id;
    flow_ctrl_ = flow_ctrl;
    PEM_LOG_INFO(log_, "[{0:d}] attach_target target:{1:d}",
                 time(), target_id_);
}

bool
TmRingTargetPort::idle() const
{
    // Memory/Ring 接口和三个本地 FIFO 都空，才表示 Target 侧没有在途工作。
    return (inf_ == nullptr || inf_->idle()) &&
           (ring_inf_ == nullptr || ring_inf_->idle()) &&
           (rd_req_q_ == nullptr || rd_req_q_->empty()) &&
           (wr_req_q_ == nullptr || wr_req_q_->empty()) &&
           (wr_dat_q_ == nullptr || wr_dat_q_->empty());
}

void
TmRingTargetPort::attach(p_tm_com_inf_t inf)
{
    inf_->connect(inf);
    PEM_LOG_INFO(log_, "[{0:d}] attach_mem_inf target:{1:d}",
                 time(), target_id_);
}

void
TmRingTargetPort::attach(p_tm_mem_t mem)
{
    if (mem != nullptr) {
        attach(mem->rw_inf_);
    }
}

p_tm_com_inf_t
TmRingTargetPort::ring_inf() const
{
    return ring_inf_;
}

p_tm_com_que_t
TmRingTargetPort::req_q(PldCmd cmd) const
{
    if (cmd == PldCmd::RD) {
        return rd_req_q_;
    }
    if (cmd == PldCmd::WR) {
        return wr_req_q_;
    }
    return wr_dat_q_;
}

void
TmRingTargetPort::recv_ring_req()
{
    recv_ring_cmd(PldCmd::RD);
    recv_ring_cmd(PldCmd::WR);
    recv_ring_cmd(PldCmd::WR_DAT);
}

void
TmRingTargetPort::recv_ring_cmd(PldCmd cmd)
{
    auto q = req_q(cmd);
    uint32_t ring_chan = ring_channel(cmd);
    // FIFO 满时不 pop ring_inf_，压力会沿 Router/Link 逐跳向上游传播。
    if (ring_inf_->valid(ring_chan) && !q->full()) {
        auto pld = ring_inf_->get_pld(ring_chan);
        q->push_back(pld);
        ring_inf_->pop_pld(ring_chan);
        PEM_LOG_INFO(log_, "[{0:d}] recv_ring_cmd target:{1:d} cmd:{2:d} "
                           "gid:{3:d} addr:0x{4:x} size:{5:d}",
                     time(), target_id_, static_cast<uint32_t>(cmd), pld->gid,
                     pld->addr, pld->size);
    }
}

void
TmRingTargetPort::send_pending_requests()
{
    send_rd_cmd();
    send_wr_cmd();
    send_wr_dat();
}

void
TmRingTargetPort::send_rd_cmd()
{
    send_cmd(PldCmd::RD);
}

void
TmRingTargetPort::send_wr_cmd()
{
    send_cmd(PldCmd::WR);
}

void
TmRingTargetPort::send_wr_dat()
{
    send_cmd(PldCmd::WR_DAT);
}

void
TmRingTargetPort::send_cmd(PldCmd cmd)
{
    auto q = req_q(cmd);
    if (q->empty()) {
        return;
    }

    // 每次只处理队头，保持同一命令通道内的基本顺序。
    auto pld = q->front();
    auto legacy_req = tm_ring_cmd_to_req(cmd);
    // 先恢复周期性 token，再同时检查全局 OSD、Target slot 和带宽 token。
    flow_ctrl_->update_tokens(time());
    if (!flow_ctrl_->can_send_to_target(target_id_, legacy_req, pld)) {
        return;
    }

    // next_issue 表达 Target 前端吞吐限制；时间未到时保留 FIFO 队头。
    auto& next_issue = next_req_issue_time(cmd);
    if (time() < next_issue) {
        return;
    }

    // Memory 真正接收后才 pop 并占用 credit，避免发送失败造成资源泄漏。
    if (inf_->send(tm_ring_cmd_bus_channel(cmd), pld)) {
        q->pop_front();
        flow_ctrl_->consume_target_credit(target_id_, legacy_req, pld);
        next_issue =
            time() + flow_ctrl_->calc_issue_busy_cycles(target_id_, pld);
        PEM_LOG_INFO(log_, "[{0:d}] send_mem_cmd target:{1:d} cmd:{2:d} "
                           "gid:{3:d} addr:0x{4:x} next_issue:{5:d}",
                     time(), target_id_, static_cast<uint32_t>(cmd), pld->gid,
                     pld->addr, next_issue);
    }
}

void
TmRingTargetPort::recv_rd_cmd_rsp()
{
    // 每个读响应 lane 有独立发射时间，可并行表达多路返回能力。
    for (uint32_t lane = 0; lane < rd_rsp_port_num_; ++lane) {
        if (!has_response(PldCmd::RD, lane)) {
            continue;
        }

        auto& next_issue = next_rsp_issue_time(PldCmd::RD, lane);
        if (time() < next_issue) {
            continue;
        }

        // 把 Memory 侧 RD 响应转换成 Ring 的 RD_RSP 元数据。
        auto rsp = front_response(PldCmd::RD, lane);
        rsp->cmd = PldCmd::RD_RSP;
        rsp->ring_subnet = tm_ring_subnet_index(TmRingSubnet::RSP);
        rsp->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
        rsp->ring_rsp_lane = lane;
        rsp->ring_rsp_phys_lane = select_rsp_phys_lane(rsp);
        // Ring 暂不可接收时不 pop Memory 响应，响应保持在原接口中。
        if (!ring_inf_->send(ring_channel(PldCmd::RD_RSP, lane), rsp)) {
            continue;
        }
        commit_rsp_phys_lane();
        pop_response(PldCmd::RD, lane);
        PEM_LOG_INFO(log_, "[{0:d}] send_rd_rsp target:{1:d} gid:{2:d} "
                           "lane:{3:d} addr:0x{4:x}",
                     time(), target_id_, rsp->gid, lane, rsp->addr);

        // 成功注入后按 Target 响应带宽计算该 lane 的下一次可发时间。
        next_issue =
            time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                      tm_ring_cmd_to_req(PldCmd::RD));
    }
}

void
TmRingTargetPort::recv_wr_cmd_rsp()
{
    // WR_RSP 是 grant/DBID 阶段，只允许 Master 继续发送 WR_DAT，不结束写事务。
    if (!has_response(PldCmd::WR)) {
        return;
    }

    auto& next_issue = next_rsp_issue_time(PldCmd::WR);
    if (time() < next_issue) {
        return;
    }

    auto rsp = front_response(PldCmd::WR);
    rsp->cmd = PldCmd::WR_RSP;
    rsp->ring_subnet = tm_ring_subnet_index(TmRingSubnet::RSP);
    rsp->ring_traffic_class = static_cast<uint32_t>(PldCmd::WR_RSP);
    rsp->ring_rsp_phys_lane = select_rsp_phys_lane(rsp);
    if (!ring_inf_->send(ring_channel(PldCmd::WR_RSP), rsp)) {
        return;
    }
    commit_rsp_phys_lane();
    pop_response(PldCmd::WR);
    PEM_LOG_INFO(log_, "[{0:d}] send_wr_rsp target:{1:d} gid:{2:d} "
                       "addr:0x{3:x}",
                 time(), target_id_, rsp->gid, rsp->addr);

    next_issue =
        time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                  tm_ring_cmd_to_req(PldCmd::WR));
}

void
TmRingTargetPort::recv_wr_dat_rsp()
{
    // WR_DAT 的响应转换为 Ring RSP，回到 Master 后才释放整笔写事务资源。
    if (!has_response(PldCmd::WR_DAT)) {
        return;
    }

    auto& next_issue = next_rsp_issue_time(PldCmd::WR_DAT);
    if (time() < next_issue) {
        return;
    }

    auto rsp = front_response(PldCmd::WR_DAT);
    rsp->cmd = PldCmd::RSP;
    rsp->ring_subnet = tm_ring_subnet_index(TmRingSubnet::RSP);
    rsp->ring_traffic_class = static_cast<uint32_t>(PldCmd::RSP);
    rsp->ring_rsp_phys_lane = select_rsp_phys_lane(rsp);
    if (!ring_inf_->send(ring_channel(PldCmd::RSP), rsp)) {
        return;
    }
    commit_rsp_phys_lane();
    pop_response(PldCmd::WR_DAT);
    PEM_LOG_INFO(log_, "[{0:d}] send_wr_dat_rsp target:{1:d} gid:{2:d} "
                       "addr:0x{3:x}",
                 time(), target_id_, rsp->gid, rsp->addr);

    next_issue =
        time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                  tm_ring_cmd_to_req(PldCmd::WR_DAT));
}

bool
TmRingTargetPort::has_response(PldCmd cmd, uint32_t lane) const
{
    // valid 只观察响应，不会从 Memory 接口移除 payload。
    return inf_->valid(response_channel(cmd, lane));
}

p_tm_pld_t
TmRingTargetPort::front_response(PldCmd cmd, uint32_t lane) const
{
    return inf_->get_pld(response_channel(cmd, lane));
}

void
TmRingTargetPort::pop_response(PldCmd cmd, uint32_t lane)
{
    // 仅在响应已成功注入 Ring 后调用，确保下游阻塞时不丢响应。
    inf_->pop_pld(response_channel(cmd, lane));
}

uint32_t
TmRingTargetPort::ring_channel(PldCmd cmd, uint32_t lane) const
{
    return tm_ring_packet_channel(cmd, lane);
}

uint32_t
TmRingTargetPort::rsp_phys_lane_count() const
{
    return tm_ring_rsp_phys_lane_count(rsp_phys_lanes_);
}

uint32_t
TmRingTargetPort::select_rsp_phys_lane(p_tm_pld_t rsp)
{
    const uint32_t lanes = rsp_phys_lane_count();
    if (lanes <= 1 || rsp == nullptr) {
        return 0;
    }

    switch (rsp_lane_select_) {
        case TmRingRspLaneSelect::TARGET:
            return target_id_ % lanes;
        case TmRingRspLaneSelect::HASH:
            return static_cast<uint32_t>(
                ((rsp->addr >> 6) ^ (rsp->addr >> 12) ^ target_id_) % lanes);
        case TmRingRspLaneSelect::ROUND_ROBIN:
            return next_rsp_phys_lane_ % lanes;
        default:
            return target_id_ % lanes;
    }
}

void
TmRingTargetPort::commit_rsp_phys_lane()
{
    const uint32_t lanes = rsp_phys_lane_count();
    if (lanes <= 1 || rsp_lane_select_ != TmRingRspLaneSelect::ROUND_ROBIN) {
        return;
    }
    next_rsp_phys_lane_ = (next_rsp_phys_lane_ + 1) % lanes;
}

tm_time_t&
TmRingTargetPort::next_req_issue_time(PldCmd cmd)
{
    return next_req_issue_time_[tm_ring_cmd_bus_channel(cmd)];
}

tm_time_t&
TmRingTargetPort::next_rsp_issue_time(PldCmd cmd, uint32_t lane)
{
    // RD 每个 lane 独立限速；两类写响应各有自己的发射节奏。
    if (cmd == PldCmd::RD) {
        return next_rd_rsp_issue_time_[lane];
    }
    if (cmd == PldCmd::WR) {
        return next_wr_req_rsp_issue_time_;
    }
    return next_wr_dat_rsp_issue_time_;
}

uint32_t
TmRingTargetPort::response_channel(PldCmd cmd, uint32_t lane) const
{
    if (cmd == PldCmd::RD) {
        return tm_ring_rd_rsp_bus_channel(lane);
    }
    return tm_ring_cmd_bus_channel(cmd);
}
