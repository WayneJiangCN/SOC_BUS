#include "tm_mesh_target_port.h"

#include <algorithm>
#include <utility>

using namespace tm_engine;

TmMeshTargetPort::TmMeshTargetPort()
{
}

TmMeshTargetPort::TmMeshTargetPort(const std::string& name, p_tm_clk_t clk,
                                   p_tm_mesh_target_cfg_t cfg,
                                   uint32_t rd_rsp_port_num,
                                   uint32_t inf_depth)
    : TmModule(name)
{
    config(name, clk, cfg, rd_rsp_port_num, inf_depth);
}

TmMeshTargetPort::~TmMeshTargetPort()
{
}

void
TmMeshTargetPort::config(const std::string& name, p_tm_clk_t clk,
                         p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t inf_depth)
{
    name_ = name;
    this->name(name_);
    clk_ = clk;
    cfg_ = cfg;

    // target 接口通道数 = WR_REQ / WR_DAT / RD_RSP lanes。
    uint32_t chan_num = static_cast<uint32_t>(aic_req_type_t::RD_REQ) +
                        rd_rsp_port_num;
    inf_ = tm_make_com_inf(clk_, name_ + "_inf", inf_depth);
    inf_->set_chan_num(chan_num);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_response_available),
                 inf_->vld);

    // target-local queues：在真正发给 target 前先做本地缓存。
    rd_req_q_ = tm_make_com_que(clk_, name_ + "_rd_req_q",
                                cfg_->rd_req_fifo_depth);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_request_available),
                 rd_req_q_->vld);
    wr_req_q_ = tm_make_com_que(clk_, name_ + "_wr_req_q",
                                cfg_->wr_req_fifo_depth);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_request_available),
                 wr_req_q_->vld);
    wr_dat_q_ = tm_make_com_que(clk_, name_ + "_wr_dat_q",
                                cfg_->wr_dat_fifo_depth);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_request_available),
                 wr_dat_q_->vld);

    issue_retry_events_[static_cast<uint32_t>(aic_req_type_t::RD_REQ)] =
        tm_make_event(name_ + "_rd_issue_retry");
    issue_retry_events_[static_cast<uint32_t>(aic_req_type_t::WR_REQ)] =
        tm_make_event(name_ + "_wr_req_issue_retry");
    issue_retry_events_[static_cast<uint32_t>(aic_req_type_t::WR_DAT)] =
        tm_make_event(name_ + "_wr_dat_issue_retry");
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_rd_issue_retry),
                 issue_retry_events_[static_cast<uint32_t>(aic_req_type_t::RD_REQ)]);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_wr_req_issue_retry),
                 issue_retry_events_[static_cast<uint32_t>(aic_req_type_t::WR_REQ)]);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshTargetPort::on_wr_dat_issue_retry),
                 issue_retry_events_[static_cast<uint32_t>(aic_req_type_t::WR_DAT)]);

    reset();
}

void
TmMeshTargetPort::reset()
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
}

void
TmMeshTargetPort::attach(event_callback_t on_response, event_callback_t on_issue)
{
    on_response_ = std::move(on_response);
    on_issue_ = std::move(on_issue);
}

void
TmMeshTargetPort::notify_issue_retry(aic_req_type_t req_type,
                                     tm_time_t delay)
{
    uint32_t req_idx = static_cast<uint32_t>(req_type);
    if (req_idx >= issue_retry_events_.size() ||
        issue_retry_events_[req_idx] == nullptr) {
        return;
    }
    issue_retry_events_[req_idx]->notify_after(std::max<tm_time_t>(1, delay));
}

void
TmMeshTargetPort::on_response_available()
{
    if (on_response_) {
        on_response_();
    }
}

void
TmMeshTargetPort::on_request_available()
{
    if (on_issue_) {
        on_issue_();
    }
}

void
TmMeshTargetPort::on_rd_issue_retry()
{
    notify_issue_callback();
}

void
TmMeshTargetPort::on_wr_req_issue_retry()
{
    notify_issue_callback();
}

void
TmMeshTargetPort::on_wr_dat_issue_retry()
{
    notify_issue_callback();
}

void
TmMeshTargetPort::notify_issue_callback()
{
    if (on_issue_) {
        on_issue_();
    }
}

bool
TmMeshTargetPort::idle() const
{
    return (inf_ == nullptr || inf_->idle()) &&
           (rd_req_q_ == nullptr || rd_req_q_->empty()) &&
           (wr_req_q_ == nullptr || wr_req_q_->empty()) &&
           (wr_dat_q_ == nullptr || wr_dat_q_->empty());
}

void
TmMeshTargetPort::attach(p_tm_com_inf_t inf)
{
    inf_->connect(inf);
}

void
TmMeshTargetPort::attach(p_tm_mem_t mem)
{
    if (mem != nullptr) {
        attach(mem->rw_inf_);
    }
}

p_tm_com_que_t
TmMeshTargetPort::req_q(aic_req_type_t req_type) const
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
TmMeshTargetPort::can_accept_request(aic_req_type_t req_type) const
{
    return !req_q(req_type)->full();
}

void
TmMeshTargetPort::accept_request(aic_req_type_t req_type, p_tm_pld_t pld)
{
    req_q(req_type)->push_back(pld);
}

bool
TmMeshTargetPort::has_request(aic_req_type_t req_type) const
{
    return !req_q(req_type)->empty();
}

p_tm_pld_t
TmMeshTargetPort::front_request(aic_req_type_t req_type) const
{
    return req_q(req_type)->front();
}

void
TmMeshTargetPort::pop_request(aic_req_type_t req_type)
{
    req_q(req_type)->pop_front();
}

bool
TmMeshTargetPort::send_request(aic_req_type_t req_type, p_tm_pld_t pld)
{
    return inf_->send(static_cast<uint32_t>(req_type), pld);
}

bool
TmMeshTargetPort::has_response(aic_req_type_t rsp_type, uint32_t lane) const
{
    return inf_->valid(response_channel(rsp_type, lane));
}

p_tm_pld_t
TmMeshTargetPort::front_response(aic_req_type_t rsp_type, uint32_t lane) const
{
    return inf_->get_pld(response_channel(rsp_type, lane));
}

void
TmMeshTargetPort::pop_response(aic_req_type_t rsp_type, uint32_t lane)
{
    inf_->pop_pld(response_channel(rsp_type, lane));
}

uint32_t
TmMeshTargetPort::response_channel(aic_req_type_t rsp_type, uint32_t lane) const
{
    if (rsp_type == aic_req_type_t::RD_REQ) {
        return static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
    }
    return static_cast<uint32_t>(rsp_type);
}
