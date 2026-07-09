#include "tm_mesh_inf.h"

#include <algorithm>
#include <vector>

using namespace std;
using namespace tm_engine;

Tm_mesh_inf::Tm_mesh_inf(const std::string& name, p_tm_clk_t clk,
                         uint32_t inf_id, p_tm_mesh_cfg_t cfg)
    : TmModule(name), clk_(clk), cfg_(cfg), inf_id_(inf_id) {
  config();
}

Tm_mesh_inf::~Tm_mesh_inf() {}

void Tm_mesh_inf::config() {
  uint32_t chan_num =
      static_cast<uint32_t>(aic_req_type_t::RD_REQ) + cfg_->rd_rsp_port_num;

  bus_inf_ =
      tm_make_com_inf(clk_, this->name() + "_bus_inf", cfg_->master_inf_depth);
  bus_inf_->set_chan_num(chan_num);

  wr_grant_fifo_ = tm_make_que<TmMeshGrant>(
      clk_, this->name() + "_wr_grant_fifo", cfg_->master_wr_grant_fifo_depth);

  reset();
}

void Tm_mesh_inf::reset() {
  bus_inf_->reset();
  req_pending_q_.clear();
  wr_dat_pending_q_.clear();
  wr_grant_fifo_->clear();
  bus_req_list_.clear();
  api_req_map_.clear();
  req_id_ = 0;
}

bool Tm_mesh_inf::idle() {
  return bus_inf_->idle() && req_pending_q_.empty() &&
         wr_dat_pending_q_.empty() && wr_grant_fifo_->empty() &&
         bus_req_list_.empty() && api_req_map_.empty();
}

void Tm_mesh_inf::tick() {
  /* endpoint 风格下，NIU 不主动往 fabric 再发一层 inf。
   * fabric 直接读取本地 pending request。 */
}

void Tm_mesh_inf::attach_upstream(p_tm_com_inf_t inf) {
  bus_inf_->connect(inf);
}

void Tm_mesh_inf::set_master_id(uint32_t mst_id) { inf_id_ = mst_id; }
// API 风格请求：直接进 req_pending_q_
uint32_t Tm_mesh_inf::send_rd_req(uint64_t address, uint32_t size) {
  if (!can_send_rd_req()) {
    return static_cast<uint32_t>(-1);
  }

  auto req = tm_make_pld(pld_cmd_t::RD, address, size);
  req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
  req->data = pld_data_t((req->buf_u32)->data());
  req->mst_id = inf_id_;

  uint32_t cur_req_id = req_id_++;
  req->gid = cur_req_id;
  req_pending_q_.push_back(req);
  track_api_request(cur_req_id, req, aic_req_type_t::RD_REQ);
  return cur_req_id;
}

uint32_t Tm_mesh_inf::send_wr_req(uint64_t address, uint32_t size) {
  if (!can_send_wr_req()) {
    return static_cast<uint32_t>(-1);
  }

  auto req = tm_make_pld(pld_cmd_t::WR, address, size);
  req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
  req->data = pld_data_t((req->buf_u32)->data());
  req->mst_id = inf_id_;

  uint32_t cur_req_id = req_id_++;
  req->gid = cur_req_id;
  req_pending_q_.push_back(req);
  track_api_request(cur_req_id, req, aic_req_type_t::WR_REQ);
  return cur_req_id;
}

bool Tm_mesh_inf::is_request_completed(uint32_t req_id) {
  auto iter = find_if(bus_req_list_.begin(), bus_req_list_.end(),
                      [req_id](const pair<uint32_t, p_tm_pld_t>& req) {
                        return req.first == req_id;
                      });
  return iter == bus_req_list_.end();
}

bool Tm_mesh_inf::can_send_rd_req() {
  return req_pending_q_.size() < request_queue_capacity();
}

bool Tm_mesh_inf::can_send_wr_req() {
  return req_pending_q_.size() < request_queue_capacity();
}
// 接口风格请求：从外部 send() 到 bus_inf_
void Tm_mesh_inf::ingest_upstream_requests(
    uint32_t master_port, const TmMeshTopology& topology,
    unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx, tm_time_t now) {
  const aic_req_type_t req_order[] = {
      aic_req_type_t::WR_REQ, aic_req_type_t::WR_DAT, aic_req_type_t::RD_REQ};

  for (auto req_type : req_order) {
    uint32_t chan = request_channel(req_type);

    while (bus_inf_->valid(chan)) {
      if (req_type == aic_req_type_t::WR_DAT &&
          wr_dat_pending_q_.size() >= write_data_queue_capacity()) {
        break;
      }
      if (req_type != aic_req_type_t::WR_DAT &&
          req_pending_q_.size() >= request_queue_capacity()) {
        break;
      }

      auto pld = bus_inf_->get_pld(chan);
      if (pld == nullptr) {
        break;
      }
      if (pld->mst_id == 0) {
        pld->mst_id = inf_id_;
      }
      // 事务上下文表：在 NIU 本地建立，供 fabric 端使用
      auto key = make_txn_key(pld);
      auto ctx_it = txn_ctx.find(key);
      if (req_type == aic_req_type_t::WR_DAT) {
        if (ctx_it == txn_ctx.end()) {
          break;
        }
        ctx_it->second.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
        wr_dat_pending_q_.push_back(pld);
      } else {
        if (ctx_it == txn_ctx.end()) {
          TmMeshTxnCtx ctx;
          ctx.master_port = master_port;
          ctx.target_id = topology.decode_target(pld->addr);
          ctx.src_node = topology.master_node(ctx.master_port);
          ctx.dst_node = topology.target_node(ctx.target_id);
          ctx.req_type = req_type;
          ctx.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
          ctx.size = pld->size;
          ctx.issue_time = now;
          txn_ctx.emplace(key, ctx);
        }
        req_pending_q_.push_back(pld);
      }

      bus_inf_->pop_pld(chan);
    }
  }
}

bool Tm_mesh_inf::has_pending_request(aic_req_type_t req_type) const {
  if (req_type == aic_req_type_t::WR_DAT) {
    return !wr_dat_pending_q_.empty();
  }
  return front_request_matches(req_type);
}

p_tm_pld_t Tm_mesh_inf::peek_pending_request(aic_req_type_t req_type) const {
  if (req_type == aic_req_type_t::WR_DAT) {
    return wr_dat_pending_q_.empty() ? nullptr : wr_dat_pending_q_.front();
  }
  if (!front_request_matches(req_type)) {
    return nullptr;
  }
  return req_pending_q_.front();
}

void Tm_mesh_inf::pop_pending_request(aic_req_type_t req_type) {
  if (req_type == aic_req_type_t::WR_DAT) {
    if (!wr_dat_pending_q_.empty()) {
      wr_dat_pending_q_.erase(wr_dat_pending_q_.begin());
    }
    return;
  }

  if (front_request_matches(req_type)) {
    req_pending_q_.erase(req_pending_q_.begin());
  }
}

bool Tm_mesh_inf::has_pending_grant() const { return !wr_grant_fifo_->empty(); }

TmMeshGrant Tm_mesh_inf::peek_pending_grant() const {
  return wr_grant_fifo_->front();
}

void Tm_mesh_inf::pop_pending_grant() {
  if (!wr_grant_fifo_->empty()) {
    wr_grant_fifo_->pop_front();
  }
}

bool Tm_mesh_inf::can_accept_write_grant() const {
  return !wr_grant_fifo_->full();
}

bool Tm_mesh_inf::accept_read_response(p_tm_pld_t rsp, uint32_t lane) {
  if (retire_api_read_response(rsp)) {
    return true;
  }
  return bus_inf_->send(response_channel(aic_req_type_t::RD_REQ, lane), rsp);
}

bool Tm_mesh_inf::accept_write_request_response(p_tm_pld_t rsp,
                                                const TmMeshGrant& grant) {
  bool is_api_write = is_api_write_request(rsp);
  bool can_take_rsp =
      !wr_grant_fifo_->full() &&
      (!is_api_write || wr_dat_pending_q_.size() < write_data_queue_capacity());
  if (!can_take_rsp) {
    return false;
  }

  if (!is_api_write &&
      !bus_inf_->send(response_channel(aic_req_type_t::WR_REQ), rsp)) {
    return false;
  }

  wr_grant_fifo_->push_back(grant);

  if (is_api_write) {
    wr_dat_pending_q_.push_back(rsp);
  } else {
    retire_tracked_request(rsp);
  }
  return true;
}

bool Tm_mesh_inf::accept_write_data_response(p_tm_pld_t rsp) {
  if (retire_api_write_response(rsp)) {
    return true;
  }
  if (!bus_inf_->send(response_channel(aic_req_type_t::WR_DAT), rsp)) {
    return false;
  }
  retire_tracked_request(rsp);
  return true;
}

size_t Tm_mesh_inf::request_queue_capacity() const {
  return static_cast<size_t>(cfg_->master_rd_req_fifo_depth) +
         static_cast<size_t>(cfg_->master_wr_req_fifo_depth);
}

size_t Tm_mesh_inf::write_data_queue_capacity() const {
  return cfg_->master_wr_dat_fifo_depth;
}

bool Tm_mesh_inf::front_request_matches(aic_req_type_t req_type) const {
  if (req_pending_q_.empty()) {
    return false;
  }

  auto pld = req_pending_q_.front();
  if (pld == nullptr) {
    return false;
  }

  if (req_type == aic_req_type_t::RD_REQ) {
    return pld->cmd == pld_cmd_t::RD;
  }
  if (req_type == aic_req_type_t::WR_REQ) {
    return pld->cmd == pld_cmd_t::WR;
  }
  return false;
}

uint32_t Tm_mesh_inf::request_channel(aic_req_type_t req_type) const {
  return static_cast<uint32_t>(req_type);
}

uint32_t Tm_mesh_inf::response_channel(aic_req_type_t req_type,
                                       uint32_t lane) const {
  if (req_type == aic_req_type_t::RD_REQ) {
    return static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
  }
  return static_cast<uint32_t>(req_type);
}

uint64_t Tm_mesh_inf::make_txn_key(uint32_t mst_id, uint32_t gid) const {
  return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t Tm_mesh_inf::make_txn_key(p_tm_pld_t pld) const {
  return make_txn_key(pld->mst_id, pld->gid);
}

void Tm_mesh_inf::track_api_request(uint32_t req_id, p_tm_pld_t req,
                                    aic_req_type_t req_type) {
  bus_req_list_.push_back(std::make_pair(req_id, req));

  TmMeshInfApiReq state;
  state.req_type = req_type;
  api_req_map_[req_id] = state;
}

void Tm_mesh_inf::retire_tracked_request(p_tm_pld_t rsp) {
  auto iter = find_if(bus_req_list_.begin(), bus_req_list_.end(),
                      [rsp](const pair<uint32_t, p_tm_pld_t>& req) {
                        return req.first == rsp->gid || req.second == rsp;
                      });
  if (iter != bus_req_list_.end()) {
    bus_req_list_.erase(iter);
  }
}

bool Tm_mesh_inf::retire_api_read_response(p_tm_pld_t rsp) {
  auto it = api_req_map_.find(rsp->gid);
  if (it == api_req_map_.end() ||
      it->second.req_type != aic_req_type_t::RD_REQ) {
    return false;
  }

  if (it->second.rsp_expected == 1 && rsp->latency > 1) {
    it->second.rsp_expected = rsp->latency;
  }
  it->second.rsp_seen++;
  if (it->second.rsp_seen >= it->second.rsp_expected) {
    api_req_map_.erase(it);
    retire_tracked_request(rsp);
  }
  return true;
}

bool Tm_mesh_inf::retire_api_write_response(p_tm_pld_t rsp) {
  auto it = api_req_map_.find(rsp->gid);
  if (it == api_req_map_.end() ||
      it->second.req_type != aic_req_type_t::WR_REQ) {
    return false;
  }

  api_req_map_.erase(it);
  retire_tracked_request(rsp);
  return true;
}

bool Tm_mesh_inf::is_api_write_request(p_tm_pld_t rsp) const {
  auto it = api_req_map_.find(rsp->gid);
  return it != api_req_map_.end() &&
         it->second.req_type == aic_req_type_t::WR_REQ;
}
