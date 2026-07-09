#include <limits>
#include <unordered_map>
#include <vector>

#include "tm_mesh.h"
#include "tm_mesh_inf.h"
#include "tm_mesh_router.h"

using namespace std;
using namespace tm_engine;

namespace {

constexpr uint64_t kInvalidOutputKey = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kTargetOutputBase = 1ull << 32;
constexpr uint64_t kMasterOutputBase = 1ull << 48;

inline uint64_t make_target_output_key(uint32_t target_id) {
  return kTargetOutputBase + target_id;
}

inline uint64_t make_master_output_key(uint32_t master_port) {
  return kMasterOutputBase + master_port;
}

inline bool is_target_output(uint64_t output_key) {
  return output_key >= kTargetOutputBase && output_key < kMasterOutputBase;
}

inline bool is_master_output(uint64_t output_key) {
  return output_key >= kMasterOutputBase;
}

inline uint32_t target_from_output(uint64_t output_key) {
  return static_cast<uint32_t>(output_key - kTargetOutputBase);
}

inline uint32_t master_from_output(uint64_t output_key) {
  return static_cast<uint32_t>(output_key - kMasterOutputBase);
}

struct RouterCandidate {
  bool valid = false;
  uint32_t traffic_class = 0;
  aic_req_type_t req_type = aic_req_type_t::RD_REQ;
  uint32_t lane = 0;
  uint32_t next_router = 0;
  uint64_t output_key = kInvalidOutputKey;
  p_tm_pld_t pld = nullptr;
};

}  // namespace

void TmMeshFabric::recv_master_reqs() {
  for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
    if (master >= master_nius_.size() || master_nius_[master] == nullptr) {
      continue;
    }
    master_nius_[master]->ingest_upstream_requests(master, topology_, txn_ctx_,
                                                   time());
  }
}

void TmMeshFabric::inject_mesh_reqs() {
  for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
    inject_mesh_req(master, aic_req_type_t::WR_REQ);
    inject_mesh_req(master, aic_req_type_t::RD_REQ);
    inject_mesh_req(master, aic_req_type_t::WR_DAT);
  }
}

void TmMeshFabric::inject_mesh_req(uint32_t master_port,
                                   aic_req_type_t req_type) {
  if (master_port >= master_nius_.size() ||
      master_nius_[master_port] == nullptr) {
    return;
  }

  auto niu = master_nius_[master_port];
  if (!niu->has_pending_request(req_type)) {
    return;
  }

  auto pld = niu->peek_pending_request(req_type);
  if (pld == nullptr) {
    return;
  }

  if (pld->mst_id == 0) {
    pld->mst_id = topology_.port_master_id(master_port);
  }

  const uint32_t router_id = topology_.master_node(master_port);
  auto mesh_fifo = get_mesh_req_fifo(router_id, req_type);
  if (mesh_fifo->full()) {
    return;
  }
  // 事务上下文表：在 fabric 端建立，供 router/target 端使用
  auto key = make_txn_key(pld);
  auto ctx_it = txn_ctx_.find(key);

  if (req_type == aic_req_type_t::WR_DAT) {
    if (ctx_it == txn_ctx_.end() || !niu->has_pending_grant()) {
      return;
    }

    auto grant = niu->peek_pending_grant();
    if (!flow_ctrl_.wr_grant_match(grant.target_id, grant.gid,
                                   ctx_it->second.target_id, pld,
                                   cfg_->strict_wr_grant_order)) {
      return;
    }
  }
  // TmMeshFabric::send_rd_req()
  // TmMeshFabric::send_wr_req()
  // Tm_mesh_inf::send_rd_req()
  // Tm_mesh_inf::send_wr_req()
  // 这些是绕过bus_inf_的请求，可以直接把请求注入到mesh中，避免了bus_inf_的流控限制
  else if (ctx_it == txn_ctx_.end()) {
    TmMeshTxnCtx ctx;
    ctx.master_port = master_port;
    ctx.target_id = topology_.decode_target(pld->addr);
    ctx.src_node = router_id;
    ctx.dst_node = topology_.target_node(ctx.target_id);
    ctx.req_type = req_type;
    ctx.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
    ctx.size = pld->size;
    ctx.issue_time = time();
    ctx_it = txn_ctx_.emplace(key, ctx).first;
  }

  niu->pop_pending_request(req_type);
  mesh_fifo->push_back(pld);
  if (ctx_it != txn_ctx_.end()) {
    ctx_it->second.state = tm_mesh_txn_state_t::IN_REQ_MESH;
  }
}
//推进 mesh 网络内部所有 router 的在途事务，让包从当前 router 往下一跳 router、target，或者回到 master NIU

void TmMeshFabric::advance_mesh_routers() {
  for (uint32_t router_id = 0; router_id < mesh_router_count_; ++router_id) {
    auto router = routers_[router_id];
    if (router == nullptr) {
      continue;
    }

    uint32_t class_count = router->traffic_class_count();
    std::vector<RouterCandidate> candidates(class_count);
    std::unordered_map<uint64_t, std::vector<uint8_t>> eligible_by_output;
    std::vector<uint64_t> output_order;

    for (uint32_t cls = 0; cls < class_count; ++cls) {
      auto pld = router->front_packet(cls);
      if (pld == nullptr) {
        continue;
      }

      auto ctx_it = txn_ctx_.find(make_txn_key(pld));
      if (ctx_it == txn_ctx_.end()) {
        continue;
      }
      auto& ctx = ctx_it->second;

      RouterCandidate cand;
      cand.valid = true;
      cand.traffic_class = cls;
      cand.pld = pld;

      if (cls == TmMeshRouter::REQ_CLASS) {
        cand.req_type = ctx.req_type;
        if (router_id == ctx.dst_node) {
          auto target_fifo = get_target_fifo(ctx.target_id, cand.req_type);
          if (target_fifo->full()) {
            continue;
          }
          cand.output_key = make_target_output_key(ctx.target_id);
        } else {
          cand.next_router =
              topology_.compute_next_node(router_id, ctx.dst_node);
          auto link = get_mesh_link(router_id, cand.next_router);
          auto next_fifo = get_mesh_req_fifo(cand.next_router, cand.req_type);
          if (link == nullptr || time() < link->next_ready_time() ||
              next_fifo->full()) {
            continue;
          }
          cand.output_key = cand.next_router;
        }
      } else if (cls == TmMeshRouter::WR_DAT_CLASS) {
        cand.req_type = aic_req_type_t::WR_DAT;
        if (router_id == ctx.dst_node) {
          auto target_fifo = get_target_fifo(ctx.target_id, cand.req_type);
          if (target_fifo->full()) {
            continue;
          }
          cand.output_key = make_target_output_key(ctx.target_id);
        } else {
          cand.next_router =
              topology_.compute_next_node(router_id, ctx.dst_node);
          auto link = get_mesh_link(router_id, cand.next_router);
          auto next_fifo = get_mesh_req_fifo(cand.next_router, cand.req_type);
          if (link == nullptr || time() < link->next_ready_time() ||
              next_fifo->full()) {
            continue;
          }
          cand.output_key = cand.next_router;
        }
      } else if (cls == TmMeshRouter::WR_REQ_RSP_CLASS) {
        if (router_id == ctx.src_node) {
          cand.output_key = make_master_output_key(ctx.master_port);
        } else {
          cand.next_router =
              topology_.compute_next_node(router_id, ctx.src_node);
          auto link = get_mesh_link(router_id, cand.next_router);
          auto next_fifo = get_mesh_wr_req_rsp_fifo(cand.next_router);
          if (link == nullptr || time() < link->next_ready_time() ||
              next_fifo->full()) {
            continue;
          }
          cand.output_key = cand.next_router;
        }
      } else if (cls == TmMeshRouter::WR_DAT_RSP_CLASS) {
        if (router_id == ctx.src_node) {
          cand.output_key = make_master_output_key(ctx.master_port);
        } else {
          cand.next_router =
              topology_.compute_next_node(router_id, ctx.src_node);
          auto link = get_mesh_link(router_id, cand.next_router);
          auto next_fifo = get_mesh_wr_dat_rsp_fifo(cand.next_router);
          if (link == nullptr || time() < link->next_ready_time() ||
              next_fifo->full()) {
            continue;
          }
          cand.output_key = cand.next_router;
        }
      } else {
        cand.lane = cls - TmMeshRouter::RD_RSP_BASE_CLASS;
        if (router_id == ctx.src_node) {
          cand.output_key = make_master_output_key(ctx.master_port);
        } else {
          cand.next_router =
              topology_.compute_next_node(router_id, ctx.src_node);
          auto link = get_mesh_link(router_id, cand.next_router);
          auto next_fifo = get_mesh_rd_rsp_fifo(cand.next_router, cand.lane);
          if (link == nullptr || time() < link->next_ready_time() ||
              next_fifo->full()) {
            continue;
          }
          cand.output_key = cand.next_router;
        }
      }

      if (cand.output_key == kInvalidOutputKey) {
        continue;
      }

      candidates[cls] = cand;
      auto it = eligible_by_output.find(cand.output_key);
      if (it == eligible_by_output.end()) {
        eligible_by_output.emplace(cand.output_key,
                                   std::vector<uint8_t>(class_count, 0));
        output_order.push_back(cand.output_key);
      }
      eligible_by_output[cand.output_key][cls] = 1;
    }

    for (auto output_key : output_order) {
      auto mask_it = eligible_by_output.find(output_key);
      if (mask_it == eligible_by_output.end()) {
        continue;
      }

      uint32_t winner = 0;
      if (!router->pick_output_winner(output_key, mask_it->second, winner)) {
        continue;
      }

      auto cand = candidates[winner];
      if (!cand.valid || cand.pld == nullptr) {
        continue;
      }

      auto ctx_it = txn_ctx_.find(make_txn_key(cand.pld));
      if (ctx_it == txn_ctx_.end()) {
        continue;
      }
      auto& ctx = ctx_it->second;

      if (is_target_output(output_key)) {
        auto target_id = target_from_output(output_key);
        auto target_fifo = get_target_fifo(target_id, cand.req_type);
        if (target_fifo->full()) {
          continue;
        }

        router->pop_packet(winner);
        target_fifo->push_back(cand.pld);
        ctx.state = tm_mesh_txn_state_t::IN_TARGET_FIFO;
        continue;
      }

      if (is_master_output(output_key)) {
        uint32_t master_port = master_from_output(output_key);
        if (master_port >= master_nius_.size() ||
            master_nius_[master_port] == nullptr) {
          continue;
        }
        auto niu = master_nius_[master_port];

        if (winner == TmMeshRouter::WR_REQ_RSP_CLASS) {
          TmMeshGrant grant;
          grant.gid = cand.pld->gid;
          grant.target_id = ctx.target_id;
          grant.chan = cand.pld->chan;
          grant.dbid = cand.pld->tnx_id;

          if (!niu->accept_write_request_response(cand.pld, grant)) {
            continue;
          }

          router->pop_packet(winner);
          continue;
        }

        if (winner == TmMeshRouter::WR_DAT_RSP_CLASS) {
          if (!niu->accept_write_data_response(cand.pld)) {
            continue;
          }

          router->pop_packet(winner);
          flow_ctrl_.release_target_credit(ctx.target_id,
                                           aic_req_type_t::WR_DAT);
          ctx.state = tm_mesh_txn_state_t::DONE;
          txn_ctx_.erase(ctx_it);
          continue;
        }

        if (!niu->accept_read_response(cand.pld, cand.lane)) {
          continue;
        }

        router->pop_packet(winner);
        if (ctx.rsp_expected == 1 && cand.pld->latency > 1) {
          ctx.rsp_expected = cand.pld->latency;
        }
        ctx.rsp_seen++;
        if (!ctx.slot_released) {
          flow_ctrl_.release_target_credit(ctx.target_id,
                                           aic_req_type_t::RD_REQ);
          ctx.slot_released = true;
        }
        if (ctx.rsp_seen >= ctx.rsp_expected) {
          ctx.state = tm_mesh_txn_state_t::DONE;
          txn_ctx_.erase(ctx_it);
        }
        continue;
      }

      auto next_router = static_cast<uint32_t>(output_key);
      auto link = get_mesh_link(router_id, next_router);
      if (link == nullptr || time() < link->next_ready_time()) {
        continue;
      }

      p_tm_com_que_t next_fifo = nullptr;
      if (winner == TmMeshRouter::REQ_CLASS ||
          winner == TmMeshRouter::WR_DAT_CLASS) {
        next_fifo = get_mesh_req_fifo(next_router, cand.req_type);
        if (next_fifo->full()) {
          continue;
        }
        router->pop_packet(winner);
        next_fifo->push_back(cand.pld);
        link->next_ready_time() = time() + link->latency();
        ctx.state = tm_mesh_txn_state_t::IN_REQ_MESH;
        continue;
      }

      if (winner == TmMeshRouter::WR_REQ_RSP_CLASS) {
        next_fifo = get_mesh_wr_req_rsp_fifo(next_router);
      } else if (winner == TmMeshRouter::WR_DAT_RSP_CLASS) {
        next_fifo = get_mesh_wr_dat_rsp_fifo(next_router);
      } else {
        next_fifo = get_mesh_rd_rsp_fifo(next_router, cand.lane);
      }

      if (next_fifo->full()) {
        continue;
      }

      router->pop_packet(winner);
      next_fifo->push_back(cand.pld);
      link->next_ready_time() = time() + link->latency();
      ctx.state = tm_mesh_txn_state_t::IN_RSP_MESH;
    }
  }
}

void TmMeshFabric::send_target_reqs() {
  for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
    send_target_req(target, aic_req_type_t::RD_REQ);
    send_target_req(target, aic_req_type_t::WR_REQ);
    send_target_req(target, aic_req_type_t::WR_DAT);
  }
}

void TmMeshFabric::send_target_req(uint32_t target_id,
                                   aic_req_type_t req_type) {
  auto target_fifo = get_target_fifo(target_id, req_type);
  if (target_fifo->empty()) {
    return;
  }

  auto pld = target_fifo->front();
  if (!flow_ctrl_.can_send_to_target(target_id, req_type, pld)) {
    return;
  }

  tm_time_t* next_issue = nullptr;
  if (req_type == aic_req_type_t::RD_REQ) {
    next_issue = &next_rd_issue_time_[target_id];
  } else if (req_type == aic_req_type_t::WR_REQ) {
    next_issue = &next_wr_req_issue_time_[target_id];
  } else {
    next_issue = &next_wr_dat_issue_time_[target_id];
  }

  if (time() < *next_issue) {
    return;
  }

  auto target_port = target_ports_[target_id];
  if (target_port != nullptr &&
      target_port->inf()->send(static_cast<uint32_t>(req_type), pld)) {
    target_fifo->pop_front();
    flow_ctrl_.consume_target_credit(target_id, req_type, pld);

    auto key = make_txn_key(pld);
    auto ctx_it = txn_ctx_.find(key);
    if (ctx_it != txn_ctx_.end()) {
      if (req_type == aic_req_type_t::RD_REQ) {
        ctx_it->second.state = tm_mesh_txn_state_t::WAIT_RD_RSP;
      } else if (req_type == aic_req_type_t::WR_REQ) {
        ctx_it->second.state = tm_mesh_txn_state_t::WAIT_WR_REQ_RSP;
      } else {
        ctx_it->second.state = tm_mesh_txn_state_t::WAIT_WR_DAT_RSP;
      }
    }

    if (req_type == aic_req_type_t::WR_DAT) {
      uint32_t master_port = topology_.find_master_port(pld->mst_id);
      if (master_port < master_nius_.size() &&
          master_nius_[master_port] != nullptr) {
        master_nius_[master_port]->pop_pending_grant();
      }
    }

    *next_issue = time() + flow_ctrl_.calc_issue_busy_cycles(target_id, pld);
  }
}
