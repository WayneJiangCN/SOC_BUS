#include "tm_ring_target_port.h"

#include "tm_bus_flow_ctrl.h"
#include "tm_pld.h"

using namespace tm_engine;

TmRingTargetPort::TmRingTargetPort()
{
}

TmRingTargetPort::TmRingTargetPort(const std::string& name, p_tm_clk_t clk,
                                   p_tm_ring_target_cfg_t cfg,
                                   uint32_t rd_rsp_port_num,
                                   uint32_t inf_depth)
    : TmModule(name)
{
    config(name, clk, cfg, rd_rsp_port_num, inf_depth);
}

TmRingTargetPort::~TmRingTargetPort()
{
}

void
TmRingTargetPort::config(const std::string& name, p_tm_clk_t clk,
                         p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t inf_depth)
{
    name_ = name;
    this->name(name_);
    clk_ = clk;
    cfg_ = cfg;
    rd_rsp_port_num_ = rd_rsp_port_num;

    uint32_t chan_num =
        tm_ring_rd_rsp_bus_channel(0) + rd_rsp_port_num_;
    inf_ = tm_make_com_inf(clk_, name_ + "_inf", inf_depth);
    inf_->set_chan_num(chan_num);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_rd_cmd_rsp), inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_wr_cmd_rsp), inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_wr_dat_rsp), inf_->vld);

    ring_inf_ = tm_make_com_inf(clk_, name_ + "_ring_inf", inf_depth);
    ring_inf_->set_chan_num(tm_ring_packet_channel_count(rd_rsp_port_num_));
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_ring_req),
                 ring_inf_->vld);

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
}

void
TmRingTargetPort::attach(uint32_t target_id,
                         std::shared_ptr<TmBusFlowCtrl> flow_ctrl)
{
    target_id_ = target_id;
    flow_ctrl_ = flow_ctrl;
}

bool
TmRingTargetPort::idle() const
{
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
    if (ring_inf_->valid(ring_chan) && !q->full()) {
        auto pld = ring_inf_->get_pld(ring_chan);
        q->push_back(pld);
        ring_inf_->pop_pld(ring_chan);
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

    auto pld = q->front();
    auto legacy_req = tm_ring_cmd_to_req(cmd);
    flow_ctrl_->update_tokens(time());
    if (!flow_ctrl_->can_send_to_target(target_id_, legacy_req, pld)) {
        return;
    }

    auto& next_issue = next_req_issue_time(cmd);
    if (time() < next_issue) {
        return;
    }

    if (inf_->send(tm_ring_cmd_bus_channel(cmd), pld)) {
        q->pop_front();
        flow_ctrl_->consume_target_credit(target_id_, legacy_req, pld);
        next_issue =
            time() + flow_ctrl_->calc_issue_busy_cycles(target_id_, pld);
    }
}

void
TmRingTargetPort::recv_rd_cmd_rsp()
{
    for (uint32_t lane = 0; lane < rd_rsp_port_num_; ++lane) {
        if (!has_response(PldCmd::RD, lane)) {
            continue;
        }

        auto& next_issue = next_rsp_issue_time(PldCmd::RD, lane);
        if (time() < next_issue) {
            continue;
        }

        auto rsp = front_response(PldCmd::RD, lane);
        rsp->cmd = PldCmd::RD_RSP;
        rsp->ring_subnet = tm_ring_subnet_index(TmRingSubnet::RSP);
        rsp->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
        rsp->ring_rsp_lane = lane;
        if (!ring_inf_->send(ring_channel(PldCmd::RD_RSP, lane), rsp)) {
            continue;
        }
        pop_response(PldCmd::RD, lane);

        next_issue =
            time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                      tm_ring_cmd_to_req(PldCmd::RD));
    }
}

void
TmRingTargetPort::recv_wr_cmd_rsp()
{
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
    if (!ring_inf_->send(ring_channel(PldCmd::WR_RSP), rsp)) {
        return;
    }
    pop_response(PldCmd::WR);

    next_issue =
        time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                  tm_ring_cmd_to_req(PldCmd::WR));
}

void
TmRingTargetPort::recv_wr_dat_rsp()
{
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
    if (!ring_inf_->send(ring_channel(PldCmd::RSP), rsp)) {
        return;
    }
    pop_response(PldCmd::WR_DAT);

    next_issue =
        time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                  tm_ring_cmd_to_req(PldCmd::WR_DAT));
}

bool
TmRingTargetPort::has_response(PldCmd cmd, uint32_t lane) const
{
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
    inf_->pop_pld(response_channel(cmd, lane));
}

uint32_t
TmRingTargetPort::ring_channel(PldCmd cmd, uint32_t lane) const
{
    return tm_ring_packet_channel(cmd, lane);
}

tm_time_t&
TmRingTargetPort::next_req_issue_time(PldCmd cmd)
{
    return next_req_issue_time_[tm_ring_cmd_bus_channel(cmd)];
}

tm_time_t&
TmRingTargetPort::next_rsp_issue_time(PldCmd cmd, uint32_t lane)
{
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
