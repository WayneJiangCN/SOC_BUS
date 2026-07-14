#include "tm_ring_target_port.h"

#include "tm_bus_flow_ctrl.h"
#include "tm_ring_router.h"
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
        static_cast<uint32_t>(aic_req_type_t::RD_REQ) + rd_rsp_port_num_;
    inf_ = tm_make_com_inf(clk_, name_ + "_inf", inf_depth);
    inf_->set_chan_num(chan_num);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_rd_cmd_rsp), inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_wr_cmd_rsp), inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingTargetPort::recv_wr_dat_rsp), inf_->vld);

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
                         std::shared_ptr<TmBusFlowCtrl> flow_ctrl,
                         const std::vector<p_tm_com_que_t>& rd_rsp_router_qs,
                         p_tm_com_que_t wr_req_rsp_router_q,
                         p_tm_com_que_t wr_dat_rsp_router_q)
{
    target_id_ = target_id;
    flow_ctrl_ = flow_ctrl;
    rd_rsp_router_qs_ = rd_rsp_router_qs;
    wr_req_rsp_router_q_ = wr_req_rsp_router_q;
    wr_dat_rsp_router_q_ = wr_dat_rsp_router_q;
}

bool
TmRingTargetPort::idle() const
{
    return (inf_ == nullptr || inf_->idle()) &&
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

p_tm_com_que_t
TmRingTargetPort::req_q(aic_req_type_t req_type) const
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return rd_req_q_;
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return wr_req_q_;
    }
    return wr_dat_q_;
}

bool
TmRingTargetPort::can_accept_request(aic_req_type_t req_type) const
{
    return !req_q(req_type)->full();
}

void
TmRingTargetPort::accept_request(aic_req_type_t req_type, p_tm_pld_t pld)
{
    req_q(req_type)->push_back(pld);
}

bool
TmRingTargetPort::has_request(aic_req_type_t req_type) const
{
    return !req_q(req_type)->empty();
}

p_tm_pld_t
TmRingTargetPort::front_request(aic_req_type_t req_type) const
{
    return req_q(req_type)->front();
}

void
TmRingTargetPort::pop_request(aic_req_type_t req_type)
{
    req_q(req_type)->pop_front();
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
    send_cmd(aic_req_type_t::RD_REQ);
}

void
TmRingTargetPort::send_wr_cmd()
{
    send_cmd(aic_req_type_t::WR_REQ);
}

void
TmRingTargetPort::send_wr_dat()
{
    send_cmd(aic_req_type_t::WR_DAT);
}

void
TmRingTargetPort::send_cmd(aic_req_type_t req_type)
{
    auto q = req_q(req_type);
    if (q->empty()) {
        return;
    }

    auto pld = q->front();
    if (!flow_ctrl_->can_send_to_target(target_id_, req_type, pld)) {
        return;
    }

    auto& next_issue = next_req_issue_time(req_type);
    if (time() < next_issue) {
        return;
    }

    if (inf_->send(static_cast<uint32_t>(req_type), pld)) {
        q->pop_front();
        flow_ctrl_->consume_target_credit(target_id_, req_type, pld);
        next_issue =
            time() + flow_ctrl_->calc_issue_busy_cycles(target_id_, pld);
    }
}

void
TmRingTargetPort::recv_rd_cmd_rsp()
{
    for (uint32_t lane = 0; lane < rd_rsp_port_num_; ++lane) {
        if (!has_response(aic_req_type_t::RD_REQ, lane)) {
            continue;
        }

        auto router_q = rsp_router_q(aic_req_type_t::RD_REQ, lane);
        if (router_q->full()) {
            continue;
        }

        auto& next_issue = next_rsp_issue_time(aic_req_type_t::RD_REQ, lane);
        if (time() < next_issue) {
            continue;
        }

        auto rsp = front_response(aic_req_type_t::RD_REQ, lane);
        rsp->cmd = PldCmd::RD_RSP;
        rsp->ring_traffic_class = TmRingRouter::traffic_class(PldCmd::RD_RSP);
        rsp->ring_rsp_lane = lane;
        router_q->push_back(rsp);
        pop_response(aic_req_type_t::RD_REQ, lane);

        next_issue =
            time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                      aic_req_type_t::RD_REQ);
    }
}

void
TmRingTargetPort::recv_wr_cmd_rsp()
{
    if (!has_response(aic_req_type_t::WR_REQ)) {
        return;
    }

    auto router_q = rsp_router_q(aic_req_type_t::WR_REQ);
    if (router_q->full()) {
        return;
    }

    auto& next_issue = next_rsp_issue_time(aic_req_type_t::WR_REQ);
    if (time() < next_issue) {
        return;
    }

    auto rsp = front_response(aic_req_type_t::WR_REQ);
    rsp->cmd = PldCmd::WR_RSP;
    rsp->ring_traffic_class = TmRingRouter::traffic_class(PldCmd::WR_RSP);
    router_q->push_back(rsp);
    pop_response(aic_req_type_t::WR_REQ);

    next_issue =
        time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                  aic_req_type_t::WR_REQ);
}

void
TmRingTargetPort::recv_wr_dat_rsp()
{
    if (!has_response(aic_req_type_t::WR_DAT)) {
        return;
    }

    auto router_q = rsp_router_q(aic_req_type_t::WR_DAT);
    if (router_q->full()) {
        return;
    }

    auto& next_issue = next_rsp_issue_time(aic_req_type_t::WR_DAT);
    if (time() < next_issue) {
        return;
    }

    auto rsp = front_response(aic_req_type_t::WR_DAT);
    rsp->cmd = PldCmd::RSP;
    rsp->ring_traffic_class = TmRingRouter::traffic_class(PldCmd::RSP);
    router_q->push_back(rsp);
    pop_response(aic_req_type_t::WR_DAT);

    next_issue =
        time() + flow_ctrl_->calc_rsp_busy_cycles(target_id_, rsp,
                                                  aic_req_type_t::WR_DAT);
}

bool
TmRingTargetPort::has_response(aic_req_type_t rsp_type, uint32_t lane) const
{
    return inf_->valid(response_channel(rsp_type, lane));
}

p_tm_pld_t
TmRingTargetPort::front_response(aic_req_type_t rsp_type, uint32_t lane) const
{
    return inf_->get_pld(response_channel(rsp_type, lane));
}

void
TmRingTargetPort::pop_response(aic_req_type_t rsp_type, uint32_t lane)
{
    inf_->pop_pld(response_channel(rsp_type, lane));
}

p_tm_com_que_t
TmRingTargetPort::rsp_router_q(aic_req_type_t rsp_type, uint32_t lane) const
{
    if (rsp_type == aic_req_type_t::RD_REQ) {
        return rd_rsp_router_qs_[lane];
    }
    if (rsp_type == aic_req_type_t::WR_REQ) {
        return wr_req_rsp_router_q_;
    }
    return wr_dat_rsp_router_q_;
}

tm_time_t&
TmRingTargetPort::next_req_issue_time(aic_req_type_t req_type)
{
    return next_req_issue_time_[static_cast<uint32_t>(req_type)];
}

tm_time_t&
TmRingTargetPort::next_rsp_issue_time(aic_req_type_t rsp_type, uint32_t lane)
{
    if (rsp_type == aic_req_type_t::RD_REQ) {
        return next_rd_rsp_issue_time_[lane];
    }
    if (rsp_type == aic_req_type_t::WR_REQ) {
        return next_wr_req_rsp_issue_time_;
    }
    return next_wr_dat_rsp_issue_time_;
}

uint32_t
TmRingTargetPort::response_channel(aic_req_type_t rsp_type, uint32_t lane) const
{
    if (rsp_type == aic_req_type_t::RD_REQ) {
        return static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
    }
    return static_cast<uint32_t>(rsp_type);
}
