#include "tm_ring_inf.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

#include "tm_bus_flow_ctrl.h"
#include "tm_pld.h"

using namespace std;
using namespace tm_engine;

TmRingInf::TmRingInf(const std::string& name, p_tm_clk_t clk, uint32_t inf_id,
                     p_tm_ring_cfg_t cfg)
    : TmModule(name), clk_(clk), cfg_(cfg), inf_id_(inf_id) {
  config();
}

TmRingInf::~TmRingInf() {}

void TmRingInf::config() {
  log_para_t log_para(this->name() + ".log");
  log_ = pem_log::create_logger(log_para);
  PEM_LOG_INFO(log_, "[{0:d}] config inf_id:{1:d}", time(), inf_id_);

  uint32_t chan_num =
      std::max<uint32_t>(tm_ring_cmd_bus_channel(PldCmd::WR_DAT) + 1,
                         tm_ring_rd_rsp_bus_channel(0) + cfg_->rd_rsp_port_num);

  bus_inf_ =
      tm_make_com_inf(clk_, this->name() + "_bus_inf", cfg_->master_inf_depth);
  bus_inf_->set_chan_num(chan_num);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_rd_cmd), bus_inf_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_wr_cmd), bus_inf_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_wr_dat), bus_inf_->vld);

  router_inf_ = tm_make_com_inf(clk_, this->name() + "_router_inf",
                                cfg_->master_inf_depth);
  router_inf_->set_chan_num(tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::recv_router_rsp),
               router_inf_->vld);

  rd_cmds_ =
      tm_make_com_que(clk_, this->name() + "_rd_cmds", cfg_->master_inf_depth);
  wr_cmds_ =
      tm_make_com_que(clk_, this->name() + "_wr_cmds", cfg_->master_inf_depth);
  wr_data_ =
      tm_make_com_que(clk_, this->name() + "_wr_data", cfg_->master_inf_depth);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::send_rd_cmd), rd_cmds_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::send_wr_cmd), wr_cmds_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::send_wr_dat), wr_data_->vld);

  wr_dat_rsp_q_ = tm_make_com_que(clk_, this->name() + "_wr_dat_rsp_q",
                                  cfg_->master_inf_depth);
  tm_sensitive(TM_MAKE_CPROC(&TmRingInf::send_wr_dat_rsp), wr_dat_rsp_q_->vld);

  reset();
}

void TmRingInf::reset() {
  PEM_LOG_INFO(log_, "[{0:d}] reset", time());
  bus_inf_->reset();
  router_inf_->reset();
  rd_cmds_->clear();
  wr_cmds_->clear();
  wr_data_->clear();
  wr_dat_rsp_q_->clear();
  req_map_.clear();
  pending_writes_.clear();
  rd_rsp_states_.clear();
  req_id_ = 0;
  rd_outstanding_ = 0;
  wr_outstanding_ = 0;
}

bool TmRingInf::idle() {
  return bus_inf_->idle() && router_inf_->idle() && wr_dat_rsp_q_->empty() &&
         rd_cmds_->empty() && wr_cmds_->empty() && wr_data_->empty() &&
         req_map_.empty() && pending_writes_.empty() &&
         rd_rsp_states_.empty() &&
         rd_outstanding_ == 0 && wr_outstanding_ == 0;
}

void TmRingInf::attach(p_tm_com_inf_t inf) { bus_inf_->connect(inf); }

p_tm_com_inf_t TmRingInf::router_inf() const { return router_inf_; }

void TmRingInf::attach(uint32_t master_port, p_tm_ring_topology_t topology,
                       std::shared_ptr<TmBusFlowCtrl> flow_ctrl) {
  master_port_ = master_port;
  topology_ = topology;
  flow_ctrl_ = flow_ctrl;
}

void TmRingInf::set_master_id(uint32_t mst_id) { inf_id_ = mst_id; }

uint32_t TmRingInf::send_rd_req(uint64_t address, uint32_t size) {
  auto req = tm_make_pld(PldCmd::RD, address, size);
  req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
  req->data = pld_data_t((req->buf_u32)->data());
  req->mst_id = inf_id_;

  uint32_t cur_req_id = req_id_++;
  req->gid = cur_req_id;
  if (rd_cmds_->full()) {
    return static_cast<uint32_t>(-1);
  }
  rd_cmds_->push_back(req);
  track_request(cur_req_id, req, PldCmd::RD);
  PEM_LOG_INFO(log_, "[{0:d}] api_send_rd gid:{1:d} addr:0x{2:x} size:{3:d}",
               time(), req->gid, address, size);
  return cur_req_id;
}

uint32_t TmRingInf::send_wr_req(uint64_t address, uint32_t size) {
  auto req = tm_make_pld(PldCmd::WR, address, size);
  req->buf_u32 = make_shared<vector<uint32_t>>(size, 1);
  req->data = pld_data_t((req->buf_u32)->data());
  req->mst_id = inf_id_;

  uint32_t cur_req_id = req_id_++;
  req->gid = cur_req_id;
  if (wr_cmds_->full()) {
    return static_cast<uint32_t>(-1);
  }
  wr_cmds_->push_back(req);
  track_request(cur_req_id, req, PldCmd::WR);
  PEM_LOG_INFO(log_, "[{0:d}] api_send_wr gid:{1:d} addr:0x{2:x} size:{3:d}",
               time(), req->gid, address, size);
  return cur_req_id;
}

bool TmRingInf::is_request_completed(uint32_t req_id) {
  return req_map_.find(req_id) == req_map_.end();
}

bool TmRingInf::can_send_rd_req() {
  return rd_outstanding_ < cfg_->master_rd_osd && !rd_cmds_->full();
}

bool TmRingInf::can_send_wr_req() {
  return wr_outstanding_ < cfg_->master_wr_osd && !wr_cmds_->full();
}

void TmRingInf::recv_rd_cmd() { recv_cmd(PldCmd::RD); }

void TmRingInf::recv_wr_cmd() { recv_cmd(PldCmd::WR); }

void TmRingInf::recv_wr_dat() { recv_cmd(PldCmd::WR_DAT); }

void TmRingInf::send_rd_cmd() { send_cmd(PldCmd::RD); }

void TmRingInf::send_wr_cmd() { send_cmd(PldCmd::WR); }

void TmRingInf::send_wr_dat() { send_cmd(PldCmd::WR_DAT); }

void TmRingInf::send_wr_dat_rsp() {
  auto rsp = wr_dat_rsp_q_->front();
  if (bus_inf_->send(response_channel(PldCmd::WR_DAT), rsp)) {
    wr_dat_rsp_q_->pop_front();
  }
}

void TmRingInf::recv_router_rsp() {
  for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
    uint32_t chan = tm_ring_packet_channel(PldCmd::RD_RSP, lane);
    if (router_inf_->valid(chan)) {
      auto rsp = router_inf_->get_pld(chan);
      if (recv_rsp(rsp)) {
        router_inf_->pop_pld(chan);
      }
    }
  }

  uint32_t wr_rsp_chan = tm_ring_packet_channel(PldCmd::WR_RSP);
  if (router_inf_->valid(wr_rsp_chan)) {
    auto rsp = router_inf_->get_pld(wr_rsp_chan);
    if (recv_rsp(rsp)) {
      router_inf_->pop_pld(wr_rsp_chan);
    }
  }

  uint32_t rsp_chan = tm_ring_packet_channel(PldCmd::RSP);
  if (router_inf_->valid(rsp_chan)) {
    auto rsp = router_inf_->get_pld(rsp_chan);
    if (recv_rsp(rsp)) {
      router_inf_->pop_pld(rsp_chan);
    }
  }
}

void TmRingInf::recv_cmd(PldCmd cmd_type) {
  uint32_t chan = tm_ring_cmd_bus_channel(cmd_type);
  auto q = req_queue(cmd_type);

  if (bus_inf_->valid(chan)) {
    auto cmd = bus_inf_->get_pld(chan);
    cmd->mst_id = inf_id_;
    if (cmd_type == PldCmd::WR_DAT) {
      if (q->full()) {
        return;
      }
      q->push_back(cmd);
    } else {
      if (q->full()) {
        return;
      }
      q->push_back(cmd);
      if (cmd_type == PldCmd::WR) {
        pending_writes_[cmd->gid] = cmd;
      }
    }
    bus_inf_->pop_pld(chan);
    PEM_LOG_INFO(log_, "[{0:d}] recv_cmd cmd:{1:d} gid:{2:d} chan:{3:d} "
                       "addr:0x{4:x} size:{5:d}",
                 time(), static_cast<uint32_t>(cmd_type), cmd->gid,
                 cmd->chan, cmd->addr, cmd->size);
  }
}

void TmRingInf::send_cmd(PldCmd cmd_type) {
  auto q = req_queue(cmd_type);

  auto cmd = q->front();
  if (issue_cmd_to_ring(cmd_type, cmd)) {
    q->pop_front();
  }
}

p_tm_com_que_t TmRingInf::req_queue(PldCmd cmd) const {
  if (cmd == PldCmd::RD) {
    return rd_cmds_;
  }
  if (cmd == PldCmd::WR) {
    return wr_cmds_;
  }
  return wr_data_;
}

bool TmRingInf::recv_rsp(p_tm_pld_t rsp) {
  auto cmd = static_cast<PldCmd>(rsp->ring_traffic_class);
  bool accepted = false;
  if (cmd == PldCmd::WR_RSP) {
    accepted = accept_wr_req_rsp(rsp);
  } else if (cmd == PldCmd::RSP) {
    accepted = accept_wr_dat_rsp(rsp);
  } else {
    accepted = accept_rd_rsp(rsp);
  }

  if (!accepted) {
    return false;
  }
  complete_rsp(rsp);
  return true;
}

bool TmRingInf::issue_cmd_to_ring(PldCmd cmd, p_tm_pld_t pld) {
  // prepare the payload for the ring network
  pld->mst_id = topology_->port_master_id(master_port_);
  pld->cmd = cmd;
  pld->ring_subnet = tm_ring_subnet_index(TmRingSubnet::REQ);
  pld->ring_traffic_class = static_cast<uint32_t>(pld->cmd);
  prepare_request_metadata(pld, cmd);

  if (cmd != PldCmd::WR_DAT && !can_reserve_master_osd(cmd)) {
    return false;
  }

  if (router_inf_->send(tm_ring_packet_channel(cmd), pld)) {
    if (cmd == PldCmd::RD) {
      rd_outstanding_++;
    } else if (cmd == PldCmd::WR) {
      wr_outstanding_++;
    }
    PEM_LOG_INFO(log_, "[{0:d}] issue_ring cmd:{1:d} gid:{2:d} "
                       "target:{3:d} src:{4:d} dst:{5:d} addr:0x{6:x}",
                 time(), static_cast<uint32_t>(cmd), pld->gid,
                 tm_pld_target_id(pld), tm_pld_src_node(pld),
                 tm_pld_dst_node(pld), pld->addr);
    return true;
  }
  return false;
}

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

bool TmRingInf::accept_rd_rsp(p_tm_pld_t rsp) {
  if (retire_read_response(rsp)) {
    PEM_LOG_INFO(log_, "[{0:d}] api_rd_rsp_done gid:{1:d} lane:{2:d}",
                 time(), rsp->gid, rsp->ring_rsp_lane);
    return true;
  }

  bool sent = bus_inf_->send(response_channel(PldCmd::RD_RSP, rsp->ring_rsp_lane),
                             rsp);
  if (sent) {
    PEM_LOG_INFO(log_, "[{0:d}] send_rd_rsp_to_bus gid:{1:d} lane:{2:d} "
                       "addr:0x{3:x} size:{4:d}",
                 time(), rsp->gid, rsp->ring_rsp_lane, rsp->addr, rsp->size);
  }
  return sent;
}

bool TmRingInf::accept_wr_req_rsp(p_tm_pld_t rsp) {
  if (wr_data_->full() ||
      pending_writes_.find(rsp->gid) == pending_writes_.end()) {
    return false;
  }

  auto wr_dat = make_write_data(rsp);

  if (tm_pld_target_id(rsp) != tm_pld_target_id(wr_dat)) {
    return false;
  }
  wr_data_->push_back(wr_dat);
  PEM_LOG_INFO(log_, "[{0:d}] recv_wr_grant gid:{1:d} target:{2:d} "
                     "push_wr_dat size:{3:d}",
               time(), rsp->gid, tm_pld_target_id(rsp), wr_dat->size);
  return true;
}

bool TmRingInf::accept_wr_dat_rsp(p_tm_pld_t rsp) {
  // if the write response is for a write request, retire it and do not enqueue
  if (retire_write_response(rsp)) {
    return true;
  }
  if (wr_dat_rsp_q_->full()) {
    return false;
  }
  // otherwise, enqueue the write response for the master to read
  pending_writes_.erase(rsp->gid);
  wr_dat_rsp_q_->push_back(rsp);
  PEM_LOG_INFO(log_, "[{0:d}] enqueue_wr_dat_rsp gid:{1:d} addr:0x{2:x}",
               time(), rsp->gid, rsp->addr);
  return true;
}

uint32_t TmRingInf::response_channel(PldCmd cmd, uint32_t lane) const {
  if (cmd == PldCmd::RD_RSP) {
    return tm_ring_rd_rsp_bus_channel(lane);
  }
  if (cmd == PldCmd::WR_RSP) {
    return tm_ring_cmd_bus_channel(PldCmd::WR);
  }
  return tm_ring_cmd_bus_channel(PldCmd::WR_DAT);
}

void TmRingInf::prepare_request_metadata(p_tm_pld_t pld, PldCmd cmd) {
  auto target_id = topology_->decode_target(pld->addr);
  tm_pld_set_ring_route(pld, static_cast<uint32_t>(cmd), target_id,
                        topology_->master_node(master_port_),
                        topology_->target_node(target_id));
}

bool TmRingInf::can_reserve_master_osd(PldCmd cmd) const {
  if (cmd == PldCmd::RD) {
    return rd_outstanding_ < cfg_->master_rd_osd;
  }
  if (cmd == PldCmd::WR) {
    return wr_outstanding_ < cfg_->master_wr_osd;
  }
  return true;
}

void TmRingInf::complete_rsp(p_tm_pld_t rsp) {
  const uint32_t target_id = tm_pld_target_id(rsp);
  auto cmd = static_cast<PldCmd>(rsp->ring_traffic_class);

  if (cmd == PldCmd::WR_RSP) {
    return;
  }

  if (cmd == PldCmd::RSP) {
    release_write_osd();
    flow_ctrl_->release_target_credit(target_id,
                                      tm_ring_cmd_to_req(PldCmd::WR_DAT));
    PEM_LOG_INFO(log_, "[{0:d}] complete_wr gid:{1:d} target:{2:d}",
                 time(), rsp->gid, target_id);
    return;
  }

  auto key = tm_pld_txn_key(rsp);
  auto& rd_state = rd_rsp_states_[key];
  if (rd_state.rsp_expected == 1 && tm_pld_rsp_count(rsp) > 1) {
    rd_state.rsp_expected = tm_pld_rsp_count(rsp);
  }
  rd_state.rsp_seen++;

  if (!rd_state.slot_released) {
    flow_ctrl_->release_target_credit(target_id, tm_ring_cmd_to_req(PldCmd::RD));
    rd_state.slot_released = true;
  }

  if (rd_state.rsp_seen >= rd_state.rsp_expected) {
    release_read_osd();
    rd_rsp_states_.erase(key);
    PEM_LOG_INFO(log_, "[{0:d}] complete_rd gid:{1:d} target:{2:d}",
                 time(), rsp->gid, target_id);
  }
}



void TmRingInf::track_request(uint32_t req_id, p_tm_pld_t req, PldCmd cmd) {
  TmRingInfApiReq state;
  state.cmd = cmd;
  state.req = req;
  req_map_[req_id] = state;
  if (cmd == PldCmd::WR) {
    pending_writes_[req->gid] = req;
  }
}

p_tm_pld_t TmRingInf::make_write_data(p_tm_pld_t grant) {
  auto it = pending_writes_.find(grant->gid);
  if (it == pending_writes_.end() || it->second == nullptr) {
    std::cerr << this->name() << ": missing original write payload for gid "
              << grant->gid << std::endl;
    assert(false && "TmRingInf missing write data");
    return nullptr;
  }

  auto original = it->second;
  auto wr_dat =
      tm_make_pld(PldCmd::WR_DAT, original->addr, original->size,
                  original->data);

  wr_dat->gid = original->gid;
  wr_dat->mst_id = original->mst_id;
  wr_dat->slv_id = original->slv_id;
  wr_dat->mst_addr = original->mst_addr;
  wr_dat->slv_addr = original->slv_addr;
  wr_dat->type_id = static_cast<uint32_t>(PldCmd::WR_DAT);
  wr_dat->rsp = pld_rsp_t::UNDEF;

  wr_dat->buf_u8 = original->buf_u8;
  wr_dat->buf_u32 = original->buf_u32;
  wr_dat->buf_u64 = original->buf_u64;
  wr_dat->chan = original->chan;
  wr_dat->latency = original->latency;
  wr_dat->rsp_count = original->rsp_count;
  wr_dat->tnx_id = grant->tnx_id;
  wr_dat->tag_id = grant->tag_id;
  wr_dat->smmu_tnx_id = original->smmu_tnx_id;

  PEM_LOG_INFO(log_, "[{0:d}] make_wr_dat gid:{1:d} addr:0x{2:x} size:{3:d}",
               time(), wr_dat->gid, wr_dat->addr, wr_dat->size);
  return wr_dat;
}

bool TmRingInf::retire_read_response(p_tm_pld_t rsp) {
  auto it = req_map_.find(rsp->gid);
  if (it == req_map_.end() || it->second.cmd != PldCmd::RD) {
    return false;
  }

  if (it->second.rsp_expected == 1 && tm_pld_rsp_count(rsp) > 1) {
    it->second.rsp_expected = tm_pld_rsp_count(rsp);
  }
  it->second.rsp_seen++;
  if (it->second.rsp_seen >= it->second.rsp_expected) {
    req_map_.erase(it);
  }
  return true;
}

bool TmRingInf::retire_write_response(p_tm_pld_t rsp) {
  auto it = req_map_.find(rsp->gid);
  if (it == req_map_.end() || it->second.cmd != PldCmd::WR) {
    return false;
  }
  req_map_.erase(it);
  pending_writes_.erase(rsp->gid);
  return true;
}
