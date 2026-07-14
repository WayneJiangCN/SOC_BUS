#include "tm_ring_inf.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "tm_bus_flow_ctrl.h"
#include "tm_pld.h"

using namespace std;
using namespace tm_engine;

TmRingInf::TmRingInf(const std::string& name, p_tm_clk_t clk,
                         uint32_t inf_id, p_tm_ring_cfg_t cfg)
    : TmModule(name), clk_(clk), cfg_(cfg), inf_id_(inf_id) {
  config();
}

TmRingInf::~TmRingInf() {}

void TmRingInf::config() {
  uint32_t chan_num =
      static_cast<uint32_t>(aic_req_type_t::RD_REQ) + cfg_->rd_rsp_port_num;

  bus_inf_ =
      tm_make_com_inf(clk_, this->name() + "_bus_inf", cfg_->master_inf_depth);
  bus_inf_->set_chan_num(chan_num);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_rd_cmd), bus_inf_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_wr_cmd), bus_inf_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_wr_dat), bus_inf_->vld);

  wr_grant_fifo_ = tm_make_que<p_tm_pld_t>(
      clk_, this->name() + "_wr_grant_fifo", cfg_->master_wr_grant_fifo_depth);

  reset();
}

void TmRingInf::reset() {
  bus_inf_->reset();
  wr_grant_fifo_->clear();
  bus_req_list_.clear();
  api_req_map_.clear();
  req_id_ = 0;
  rd_outstanding_ = 0;
  wr_outstanding_ = 0;
}

bool TmRingInf::idle() {
  return bus_inf_->idle() && wr_grant_fifo_->empty() && bus_req_list_.empty() &&
         api_req_map_.empty() && rd_outstanding_ == 0 && wr_outstanding_ == 0;
}

void TmRingInf::attach(p_tm_com_inf_t inf) { bus_inf_->connect(inf); }

void TmRingInf::attach(uint32_t master_port, p_tm_ring_topology_t topology,
                         p_tm_ring_flow_ctrl_t flow_ctrl,
                         p_tm_com_que_t router_req_q,
                         p_tm_com_que_t router_wr_dat_q,
                         tm_ring_osd_reserve_t global_osd_reserve) {
  master_port_ = master_port;
  topology_ = topology;
  flow_ctrl_ = flow_ctrl;
  router_req_q_ = router_req_q;
  router_wr_dat_q_ = router_wr_dat_q;
  global_osd_reserve_ = std::move(global_osd_reserve);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_rd_cmd), router_req_q_->rdy);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_wr_cmd), router_req_q_->rdy);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_wr_dat), router_wr_dat_q_->rdy);
}

void TmRingInf::set_master_id(uint32_t mst_id) { inf_id_ = mst_id; }

uint32_t TmRingInf::send_rd_req(uint64_t address, uint32_t size) {
  auto req = tm_make_pld(PldCmd::RD, address, size);
  req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
  req->data = pld_data_t((req->buf_u32)->data());
  req->mst_id = inf_id_;

  uint32_t cur_req_id = req_id_++;
  req->gid = cur_req_id;
  if (!issue_cmd_to_ring(aic_req_type_t::RD_REQ, req)) {
    return static_cast<uint32_t>(-1);
  }
  track_api_request(cur_req_id, req, aic_req_type_t::RD_REQ);
  return cur_req_id;
}

uint32_t TmRingInf::send_wr_req(uint64_t address, uint32_t size) {
  auto req = tm_make_pld(PldCmd::WR, address, size);
  req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
  req->data = pld_data_t((req->buf_u32)->data());
  req->mst_id = inf_id_;

  uint32_t cur_req_id = req_id_++;
  req->gid = cur_req_id;
  if (!issue_cmd_to_ring(aic_req_type_t::WR_REQ, req)) {
    return static_cast<uint32_t>(-1);
  }
  track_api_request(cur_req_id, req, aic_req_type_t::WR_REQ);
  return cur_req_id;
}

bool TmRingInf::is_request_completed(uint32_t req_id) {
  auto iter = find_if(bus_req_list_.begin(), bus_req_list_.end(),
                      [req_id](const pair<uint32_t, p_tm_pld_t>& req) {
                        return req.first == req_id;
                      });
  return iter == bus_req_list_.end();
}

bool TmRingInf::can_send_rd_req() {
  return rd_outstanding_ < cfg_->master_rd_osd && !router_req_q_->full();
}

bool TmRingInf::can_send_wr_req() {
  return wr_outstanding_ < cfg_->master_wr_osd && !router_req_q_->full();
}

void TmRingInf::recv_rd_cmd() {
  auto req_type = aic_req_type_t::RD_REQ;
  uint32_t chan = static_cast<uint32_t>(req_type);

  if (bus_inf_->valid(chan)) {
    auto cmd = bus_inf_->get_pld(chan);
    cmd->mst_id = inf_id_;
    if (!issue_cmd_to_ring(req_type, cmd)) {
      return;
    }
    bus_inf_->pop_pld(chan);
  }
}

void TmRingInf::recv_wr_cmd() {
  auto req_type = aic_req_type_t::WR_REQ;
  uint32_t chan = static_cast<uint32_t>(req_type);

  if (bus_inf_->valid(chan)) {
    auto cmd = bus_inf_->get_pld(chan);
    cmd->mst_id = inf_id_;
    if (!issue_cmd_to_ring(req_type, cmd)) {
      return;
    }
    bus_inf_->pop_pld(chan);
  }
}

void TmRingInf::recv_wr_dat() {
  auto req_type = aic_req_type_t::WR_DAT;
  uint32_t chan = static_cast<uint32_t>(req_type);

  if (bus_inf_->valid(chan)) {
    auto cmd = bus_inf_->get_pld(chan);
    cmd->mst_id = inf_id_;
    if (!issue_cmd_to_ring(req_type, cmd)) {
      return;
    }
    bus_inf_->pop_pld(chan);
  }
}

bool TmRingInf::issue_cmd_to_ring(aic_req_type_t req_type, p_tm_pld_t pld) {
  pld->mst_id = topology_->port_master_id(master_port_);
  if (req_type == aic_req_type_t::RD_REQ) {
    pld->cmd = PldCmd::RD;
  } else if (req_type == aic_req_type_t::WR_REQ) {
    pld->cmd = PldCmd::WR;
  } else {
    pld->cmd = PldCmd::WR_DAT;
  }
  prepare_request_metadata(pld, req_type);

  auto router_fifo =
      req_type == aic_req_type_t::WR_DAT ? router_wr_dat_q_ : router_req_q_;
  if (router_fifo->full()) {
    return false;
  }

  if (req_type == aic_req_type_t::WR_DAT) {
    if (wr_grant_fifo_->empty()) {
      return false;
    }

    auto grant = wr_grant_fifo_->front();
    if (!flow_ctrl_->wr_grant_match(tm_pld_target_id(grant), grant->gid,
                                    tm_pld_target_id(pld), pld,
                                    cfg_->strict_wr_grant_order)) {
      return false;
    }
  } else if (!reserve_transaction_osd(req_type)) {
    return false;
  }

  router_fifo->push_back(pld);
  return true;
}

void TmRingInf::pop_pending_grant() { wr_grant_fifo_->pop_front(); }

void TmRingInf::release_read_osd() {
  if (rd_outstanding_ > 0) {
    rd_outstanding_--;
  }
}

void TmRingInf::release_write_osd() {
  if (wr_outstanding_ > 0) {
    wr_outstanding_--;
  }
}

bool TmRingInf::accept_read_response(p_tm_pld_t rsp, uint32_t lane) {
  if (retire_api_read_response(rsp)) {
    return true;
  }
  if (!bus_inf_->send(response_channel(aic_req_type_t::RD_REQ, lane), rsp)) {
    return false;
  }
  return true;
}

bool TmRingInf::accept_write_request_response(p_tm_pld_t rsp) {
  bool is_api_write = is_api_write_request(rsp);
  if (wr_grant_fifo_->full()) {
    return false;
  }

  if (is_api_write) {
    if (router_wr_dat_q_->full()) {
      return false;
    }
    if (!flow_ctrl_->wr_grant_match(tm_pld_target_id(rsp), rsp->gid,
                                    tm_pld_target_id(rsp), rsp,
                                    cfg_->strict_wr_grant_order)) {
      return false;
    }
    wr_grant_fifo_->push_back(rsp);
    rsp->cmd = PldCmd::WR_DAT;
    rsp->type_id = static_cast<uint32_t>(aic_req_type_t::WR_DAT);
    router_wr_dat_q_->push_back(rsp);
    return true;
  }

  if (!is_api_write &&
      !bus_inf_->send(response_channel(aic_req_type_t::WR_REQ), rsp)) {
    return false;
  }

  wr_grant_fifo_->push_back(rsp);
  retire_tracked_request(rsp);
  recv_wr_dat();
  return true;
}

bool TmRingInf::accept_write_data_response(p_tm_pld_t rsp) {
  if (retire_api_write_response(rsp)) {
    return true;
  }
  if (!bus_inf_->send(response_channel(aic_req_type_t::WR_DAT), rsp)) {
    return false;
  }
  retire_tracked_request(rsp);
  return true;
}

uint32_t TmRingInf::response_channel(aic_req_type_t req_type,
                                       uint32_t lane) const {
  if (req_type == aic_req_type_t::RD_REQ) {
    return static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
  }
  return static_cast<uint32_t>(req_type);
}

void TmRingInf::prepare_request_metadata(p_tm_pld_t pld,
                                           aic_req_type_t req_type) {
  auto target_id = topology_->decode_target(pld->addr);
  tm_pld_set_ring_route(pld, static_cast<uint32_t>(req_type), target_id,
                        topology_->master_node(master_port_),
                        topology_->target_node(target_id), time());
}

bool TmRingInf::can_reserve_master_osd(aic_req_type_t req_type) const {
  if (req_type == aic_req_type_t::RD_REQ) {
    return rd_outstanding_ < cfg_->master_rd_osd;
  }
  if (req_type == aic_req_type_t::WR_REQ) {
    return wr_outstanding_ < cfg_->master_wr_osd;
  }
  return true;
}

bool TmRingInf::reserve_transaction_osd(aic_req_type_t req_type) {
  if (!can_reserve_master_osd(req_type)) {
    return false;
  }
  if (global_osd_reserve_ && !global_osd_reserve_(req_type)) {
    return false;
  }
  if (req_type == aic_req_type_t::RD_REQ) {
    rd_outstanding_++;
  } else if (req_type == aic_req_type_t::WR_REQ) {
    wr_outstanding_++;
  }
  return true;
}

void TmRingInf::track_api_request(uint32_t req_id, p_tm_pld_t req,
                                    aic_req_type_t req_type) {
  bus_req_list_.push_back(std::make_pair(req_id, req));

  TmRingInfApiReq state;
  state.req_type = req_type;
  api_req_map_[req_id] = state;
}

void TmRingInf::retire_tracked_request(p_tm_pld_t rsp) {
  auto iter = find_if(bus_req_list_.begin(), bus_req_list_.end(),
                      [rsp](const pair<uint32_t, p_tm_pld_t>& req) {
                        return req.first == rsp->gid || req.second == rsp;
                      });
  if (iter != bus_req_list_.end()) {
    bus_req_list_.erase(iter);
  }
}

bool TmRingInf::retire_api_read_response(p_tm_pld_t rsp) {
  auto it = api_req_map_.find(rsp->gid);
  if (it == api_req_map_.end() ||
      it->second.req_type != aic_req_type_t::RD_REQ) {
    return false;
  }

  if (it->second.rsp_expected == 1 && tm_pld_rsp_count(rsp) > 1) {
    it->second.rsp_expected = tm_pld_rsp_count(rsp);
  }
  it->second.rsp_seen++;
  if (it->second.rsp_seen >= it->second.rsp_expected) {
    api_req_map_.erase(it);
    retire_tracked_request(rsp);
  }
  return true;
}

bool TmRingInf::retire_api_write_response(p_tm_pld_t rsp) {
  auto it = api_req_map_.find(rsp->gid);
  if (it == api_req_map_.end() ||
      it->second.req_type != aic_req_type_t::WR_REQ) {
    return false;
  }

  api_req_map_.erase(it);
  retire_tracked_request(rsp);
  return true;
}

bool TmRingInf::is_api_write_request(p_tm_pld_t rsp) const {
  auto it = api_req_map_.find(rsp->gid);
  return it != api_req_map_.end() &&
         it->second.req_type == aic_req_type_t::WR_REQ;
}
