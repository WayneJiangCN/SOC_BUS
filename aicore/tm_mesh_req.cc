#include <array>
#include <vector>

#include "tm_mesh.h"
#include "tm_mesh_inf.h"
#include "tm_mesh_link.h"
#include "tm_mesh_router.h"

using namespace std;
using namespace tm_engine;

namespace {

/*
 * RouterCandidate
 *
 * 表示“当前拍、当前 router 上，一个可能被发出去的候选包”。
 *
 * 这个结构是 `advance_mesh_routers()` 的临时工作集，
 * 用来把分散在各个输入方向、各个 traffic class 队头上的包，
 * 统一抽象成可参与仲裁的候选对象。
 *
 * 一个候选最终只有三种去向：
 * 1. 送到下一跳 router，对应 out_dir = NORTH/SOUTH/EAST/WEST。
 * 2. 送到本地 target，对应 out_dir = LOCAL 且包是 request / WR_DAT。
 * 3. 送回本地 master NIU，对应 out_dir = LOCAL 且包是 response。
 */
struct RouterCandidate {
  bool valid = false;
  TmMeshPortDir in_dir = TmMeshPortDir::LOCAL;
  TmMeshPortDir out_dir = TmMeshPortDir::LOCAL;
  uint32_t traffic_class = 0;
  aic_req_type_t req_type = aic_req_type_t::RD_REQ;
  uint32_t lane = 0;
  p_tm_pld_t pld = nullptr;
};

/*
 * 统一的方向扫描顺序。
 *
 * 当前 router 是 port-based 结构，因此很多循环都会按固定方向顺序：
 *   LOCAL -> NORTH -> SOUTH -> EAST -> WEST
 *
 * 这样做的好处是：
 * 1. 代码行为稳定，不会因为 unordered 容器顺序波动。
 * 2. 调试时更容易复现问题。
 */
constexpr std::array<TmMeshPortDir, 5> kPortOrder = {
    TmMeshPortDir::LOCAL, TmMeshPortDir::NORTH, TmMeshPortDir::SOUTH,
    TmMeshPortDir::EAST, TmMeshPortDir::WEST};

}  // namespace

void TmMeshFabric::recv_master_reqs() {
  /*
   * 第一步：先让每个 master-side NIU 从自己的 bus_inf_ 吸收请求。
   *
   * 这一步只做：
   * - 从上游接口读请求
   * - 放入 NIU 本地 pending queue
   * - 必要时建立 txn_ctx_
   *
   * 这一步还不会真正把请求放进 source router。
   */
  for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
    if (master >= master_nius_.size() || master_nius_[master] == nullptr) {
      continue;
    }
    master_nius_[master]->ingest_upstream_requests(master, topology_, txn_ctx_,
                                                   time());
  }
}

void TmMeshFabric::inject_mesh_reqs() {
  /*
   * 第二步：尝试把 NIU 本地 pending 请求注入 source router 的 LOCAL 输入口。
   *
   * 当前顺序固定为：
   * 1. WR_REQ
   * 2. RD_REQ
   * 3. WR_DAT
   *
   * 这里保留了一个比较明确的协议语义：
   * - WR_DAT 不是独立事务，只有在 WR_REQ 已经建档并且 grant 匹配时才允许注入。
   */
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

  // 从接口路径进来的包，可能还没补齐 mst_id，这里统一补上。
  if (pld->mst_id == 0) {
    pld->mst_id = topology_.port_master_id(master_port);
  }

  /*
   * 每个 master 在 topology 中都有一个本地接入 router。
   * 所有从这个 master 发出的请求，都会先进入这个 source router 的 LOCAL 输入队列。
   */
  const uint32_t router_id = topology_.master_node(master_port);
  auto mesh_fifo =
      get_mesh_req_fifo(router_id, TmMeshPortDir::LOCAL, req_type);
  if (mesh_fifo->full()) {
    return;
  }

  auto key = make_txn_key(pld);
  auto ctx_it = txn_ctx_.find(key);

  if (req_type == aic_req_type_t::WR_DAT) {
    /*
     * WR_DAT 是写事务后半段，不允许自己独立成事务：
     * 1. 必须先在 txn_ctx_ 里找到对应 WR_REQ。
     * 2. NIU 本地必须已经拿到 pending grant。
     * 3. grant 和当前 WR_DAT 必须匹配。
     */
    if (ctx_it == txn_ctx_.end() || !niu->has_pending_grant()) {
      return;
    }

    auto grant = niu->peek_pending_grant();
    if (!flow_ctrl_.wr_grant_match(grant.target_id, grant.gid,
                                   ctx_it->second.target_id, pld,
                                   cfg_->strict_wr_grant_order)) {
      return;
    }
  } else if (ctx_it == txn_ctx_.end()) {
    /*
     * 对 API 风格请求，可能还没经过 ingest_upstream_requests() 的建档路径，
     * 所以在真正进入 mesh 之前，这里要做一次兜底建档。
     */
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

  /*
   * 真正注入 source router 前，先从 NIU 本地 pending 中弹出。
   * 注入后，这个包的“归属地”就从 NIU 切换成了 mesh 内部 router queue。
   */
  niu->pop_pending_request(req_type);
  mesh_fifo->push_back(pld);
  if (ctx_it != txn_ctx_.end()) {
    ctx_it->second.state = tm_mesh_txn_state_t::IN_REQ_MESH;
  }
}

void TmMeshFabric::drain_ready_links() {
  /*
   * 第三步：把已经走完链路时延的在途包灌回目标 router 的输入口。
   *
   * 这一步的含义是：
   * - 上一拍/前几拍已经从某个输出口发上链路的包，
   *   到了 ready_time 后，才真正“出现在”目标 router 的 dst_dir 输入口。
   *
   * 这是当前 link latency 建模的关键：
   * 包不是瞬移到下一跳，而是先进入 link 的 inflight_packets_，
   * 过若干拍后再重新落地成 router queue。
   */
  for (auto& it : links_) {
    auto link = it.second;
    if (link == nullptr) {
      continue;
    }

    while (true) {
      const auto* transit = link->peek_ready_packet(time());
      if (transit == nullptr || transit->pld == nullptr) {
        break;
      }

      auto ctx_it = txn_ctx_.find(make_txn_key(transit->pld));
      if (ctx_it == txn_ctx_.end()) {
        // 理论上不该发生；如果事务已经被删掉，就直接把链路里的包丢弃掉。
        link->pop_ready_packet();
        continue;
      }

      auto dst_router = link->dst_router();
      auto dst_dir = link->dst_dir();

      /*
       * 按 traffic class 还原目标输入口应该进入哪类队列。
       * 这里相当于做“链路结束 -> router 输入侧 demux”。
       */
      p_tm_com_que_t dst_fifo = nullptr;
      if (transit->traffic_class == TmMeshRouter::REQ_CLASS) {
        dst_fifo =
            get_mesh_req_fifo(dst_router, dst_dir, ctx_it->second.req_type);
      } else if (transit->traffic_class == TmMeshRouter::WR_DAT_CLASS) {
        dst_fifo =
            get_mesh_req_fifo(dst_router, dst_dir, aic_req_type_t::WR_DAT);
      } else if (transit->traffic_class == TmMeshRouter::WR_REQ_RSP_CLASS) {
        dst_fifo = get_mesh_wr_req_rsp_fifo(dst_router, dst_dir);
      } else if (transit->traffic_class == TmMeshRouter::WR_DAT_RSP_CLASS) {
        dst_fifo = get_mesh_wr_dat_rsp_fifo(dst_router, dst_dir);
      } else {
        uint32_t lane = transit->traffic_class - TmMeshRouter::RD_RSP_BASE_CLASS;
        dst_fifo = get_mesh_rd_rsp_fifo(dst_router, dst_dir, lane);
      }

      if (dst_fifo == nullptr) {
        link->pop_ready_packet();
        continue;
      }
      if (dst_fifo->full()) {
        // 目标输入口满时，不把包弹掉，继续留在 link ready 队头等待下一拍。
        break;
      }

      dst_fifo->push_back(transit->pld);
      link->pop_ready_packet();
    }
  }
}

void TmMeshFabric::advance_mesh_routers() {
  /*
   * 第四步：推进整张 mesh 的 router。
   *
   * 这是当前模型最核心的主路径，做了 4 件事：
   * 1. 先 drain ready links，把“链路到站”的包灌回 router 输入口。
   * 2. 从每个 router 的每个输入方向、每个 traffic class 取队头包，形成候选集。
   * 3. 按输出方向做仲裁：每个 output port 每拍只选一个 winner。
   * 4. winner 要么发往 LOCAL 出口，要么发往下一跳 link。
   */
  drain_ready_links();

  for (uint32_t router_id = 0; router_id < mesh_router_count_; ++router_id) {
    auto router = routers_[router_id];
    if (router == nullptr) {
      continue;
    }

    uint32_t class_count = router->traffic_class_count();
    uint32_t contender_count = router->contender_count();
    std::vector<RouterCandidate> candidates(contender_count);

    /*
     * 简化 crossbar 约束：
     * 同一个输入口在同一拍最多只能赢一次。
     *
     * 含义是：
     * - 即使某个输入口队头上的不同 class 都匹配到了不同输出方向，
     *   当前拍也只允许这个输入口真正送出一个单位。
     */
    std::array<uint8_t, tm_mesh_port_count()> used_input_ports = {0};

    /*
     * 第一步：候选收集。
     *
     * 对每个输入方向、每个 traffic class：
     * - 取队头包
     * - 查事务上下文
     * - 根据事务当前阶段和 src/dst 节点决定 out_dir
     * - 如果链路/本地出口当前拍不可用，则这个队头本拍不进入候选集
     */
    for (auto in_dir : kPortOrder) {
      for (uint32_t cls = 0; cls < class_count; ++cls) {
        auto pld = router->front_packet(in_dir, cls);
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
        cand.in_dir = in_dir;
        cand.traffic_class = cls;
        cand.pld = pld;

        if (cls == TmMeshRouter::REQ_CLASS) {
          /*
           * REQ_CLASS 实际承载两种请求：
           * - RD_REQ
           * - WR_REQ
           *
           * 具体是哪种，要去 txn_ctx_ 里看 ctx.req_type。
           */
          cand.req_type = ctx.req_type;
          if (router_id == ctx.dst_node) {
            /*
             * 请求已经到目标 router。
             * 下一步不再往 mesh 内部走，而是尝试进入本地 target queue。
             */
            auto target_fifo = get_target_fifo(ctx.target_id, cand.req_type);
            if (target_fifo->full()) {
              continue;
            }
            cand.out_dir = TmMeshPortDir::LOCAL;
          } else {
            /*
             * 还没到目标节点，按 topology 计算下一跳方向。
             * 只有对应 link 本拍能发时，这个候选才算有效。
             */
            cand.out_dir = topology_.route_direction(router_id, ctx.dst_node);
            uint32_t next_router = topology_.neighbor(router_id, cand.out_dir);
            auto link = get_mesh_link(router_id, cand.out_dir, next_router,
                                      tm_mesh_opposite_dir(cand.out_dir));
            if (link == nullptr || !link->can_send(time())) {
              continue;
            }
          }
        } else if (cls == TmMeshRouter::WR_DAT_CLASS) {
          /*
           * WR_DAT 逻辑和 request 类似，也是朝 dst_node 前进；
           * 区别只在于它已经是写事务后半段。
           */
          cand.req_type = aic_req_type_t::WR_DAT;
          if (router_id == ctx.dst_node) {
            auto target_fifo = get_target_fifo(ctx.target_id, cand.req_type);
            if (target_fifo->full()) {
              continue;
            }
            cand.out_dir = TmMeshPortDir::LOCAL;
          } else {
            cand.out_dir = topology_.route_direction(router_id, ctx.dst_node);
            uint32_t next_router = topology_.neighbor(router_id, cand.out_dir);
            auto link = get_mesh_link(router_id, cand.out_dir, next_router,
                                      tm_mesh_opposite_dir(cand.out_dir));
            if (link == nullptr || !link->can_send(time())) {
              continue;
            }
          }
        } else if (cls == TmMeshRouter::WR_REQ_RSP_CLASS) {
          /*
           * WR_REQ_RSP 是写事务 grant 的返回路径。
           * 它的回程目标不是 target，而是 source master 对应的 src_node。
           */
          if (router_id == ctx.src_node) {
            // 已回到源节点，本拍可直接尝试送回 NIU。
            cand.out_dir = TmMeshPortDir::LOCAL;
          } else {
            cand.out_dir = topology_.route_direction(router_id, ctx.src_node);
            uint32_t next_router = topology_.neighbor(router_id, cand.out_dir);
            auto link = get_mesh_link(router_id, cand.out_dir, next_router,
                                      tm_mesh_opposite_dir(cand.out_dir));
            if (link == nullptr || !link->can_send(time())) {
              continue;
            }
          }
        } else if (cls == TmMeshRouter::WR_DAT_RSP_CLASS) {
          /*
           * WR_DAT_RSP 是写事务最终完成响应，也要沿 src_node 方向回到源 NIU。
           */
          if (router_id == ctx.src_node) {
            cand.out_dir = TmMeshPortDir::LOCAL;
          } else {
            cand.out_dir = topology_.route_direction(router_id, ctx.src_node);
            uint32_t next_router = topology_.neighbor(router_id, cand.out_dir);
            auto link = get_mesh_link(router_id, cand.out_dir, next_router,
                                      tm_mesh_opposite_dir(cand.out_dir));
            if (link == nullptr || !link->can_send(time())) {
              continue;
            }
          }
        } else {
          /*
           * 剩下的 traffic class 都是 RD_RSP。
           * lane 编号编码在 class id 中，这里先还原出来。
           */
          cand.lane = cls - TmMeshRouter::RD_RSP_BASE_CLASS;
          if (router_id == ctx.src_node) {
            cand.out_dir = TmMeshPortDir::LOCAL;
          } else {
            cand.out_dir = topology_.route_direction(router_id, ctx.src_node);
            uint32_t next_router = topology_.neighbor(router_id, cand.out_dir);
            auto link = get_mesh_link(router_id, cand.out_dir, next_router,
                                      tm_mesh_opposite_dir(cand.out_dir));
            if (link == nullptr || !link->can_send(time())) {
              continue;
            }
          }
        }

        candidates[router->contender_id(in_dir, cls)] = cand;
      }
    }

    /*
     * 第二步：按输出方向做仲裁。
     *
     * 这里的含义是：
     * - 每个输出口每拍只选一个 winner
     * - 但同一个 router 可以在同一拍里，分别对多个输出方向各选一个 winner
     * - 真正能不能发，还要继续受 input-port 单拍唯一、LOCAL 容量、link can_send 等约束
     */
    for (auto out_dir : kPortOrder) {
      std::vector<uint8_t> eligible_mask(contender_count, 0);
      bool has_eligible = false;

      /*
       * 只把满足以下条件的 contender 放进本输出口仲裁：
       * 1. 本身有效
       * 2. 本拍目标就是这个 out_dir
       * 3. 对应输入口本拍还没被其他输出口用掉
       */
      for (uint32_t contender = 0; contender < contender_count; ++contender) {
        if (!candidates[contender].valid ||
            candidates[contender].out_dir != out_dir ||
            used_input_ports[tm_mesh_port_index(candidates[contender].in_dir)] !=
                0) {
          continue;
        }
        eligible_mask[contender] = 1;
        has_eligible = true;
      }
      if (!has_eligible) {
        continue;
      }

      uint32_t winner = 0;
      if (!router->pick_output_winner(out_dir, eligible_mask, winner)) {
        continue;
      }

      const auto& cand = candidates[winner];
      if (!cand.valid || cand.pld == nullptr) {
        continue;
      }

      uint32_t input_port_idx = tm_mesh_port_index(cand.in_dir);
      if (used_input_ports[input_port_idx] != 0) {
        continue;
      }

      auto ctx_it = txn_ctx_.find(make_txn_key(cand.pld));
      if (ctx_it == txn_ctx_.end()) {
        continue;
      }
      auto& ctx = ctx_it->second;

      if (out_dir == TmMeshPortDir::LOCAL) {
        /*
         * LOCAL 方向表示已经到本节点本地出口。
         * 这时不再经过 router-to-router link，而是分成两种情况：
         * 1. request / WR_DAT：进入 TargetPort 本地队列
         * 2. response：送回 source master 的 NIU
         */
        if (cand.traffic_class == TmMeshRouter::REQ_CLASS ||
            cand.traffic_class == TmMeshRouter::WR_DAT_CLASS) {
          auto target_fifo = get_target_fifo(ctx.target_id, cand.req_type);
          if (target_fifo->full()) {
            continue;
          }

          router->pop_packet(cand.in_dir, cand.traffic_class);
          target_fifo->push_back(cand.pld);
          used_input_ports[input_port_idx] = 1;
          ctx.state = tm_mesh_txn_state_t::IN_TARGET_FIFO;
          continue;
        }

        uint32_t master_port = ctx.master_port;
        if (master_port >= master_nius_.size() ||
            master_nius_[master_port] == nullptr) {
          continue;
        }
        auto niu = master_nius_[master_port];

        if (cand.traffic_class == TmMeshRouter::WR_REQ_RSP_CLASS) {
          /*
           * WR_REQ_RSP 回到源 NIU 时，会先转成一个 grant。
           * grant 进入 NIU 的 wr_grant_fifo_，供后续 WR_DAT 匹配使用。
           */
          TmMeshGrant grant;
          grant.gid = cand.pld->gid;
          grant.target_id = ctx.target_id;
          grant.chan = cand.pld->chan;
          grant.dbid = cand.pld->tnx_id;

          if (!niu->accept_write_request_response(cand.pld, grant)) {
            continue;
          }

          router->pop_packet(cand.in_dir, cand.traffic_class);
          used_input_ports[input_port_idx] = 1;
          continue;
        }

        if (cand.traffic_class == TmMeshRouter::WR_DAT_RSP_CLASS) {
          /*
           * WR_DAT_RSP 是写事务最终完成点。
           * 成功送回 NIU 后，这笔事务可以真正退休，并释放 WR_DAT credit。
           */
          if (!niu->accept_write_data_response(cand.pld)) {
            continue;
          }

          router->pop_packet(cand.in_dir, cand.traffic_class);
          used_input_ports[input_port_idx] = 1;
          flow_ctrl_.release_target_credit(ctx.target_id,
                                           aic_req_type_t::WR_DAT);
          ctx.state = tm_mesh_txn_state_t::DONE;
          txn_ctx_.erase(ctx_it);
          continue;
        }

        /*
         * 剩下的 LOCAL response 都是 RD_RSP。
         * 读响应可能按多拍/多 lane 返回，所以只有最后一个响应看到后才退休事务。
         */
        if (!niu->accept_read_response(cand.pld, cand.lane)) {
          continue;
        }

        router->pop_packet(cand.in_dir, cand.traffic_class);
        used_input_ports[input_port_idx] = 1;
        if (ctx.rsp_expected == 1 && cand.pld->latency > 1) {
          ctx.rsp_expected = cand.pld->latency;
        }
        ctx.rsp_seen++;
        if (!ctx.slot_released) {
          flow_ctrl_.release_target_credit(ctx.target_id, aic_req_type_t::RD_REQ);
          ctx.slot_released = true;
        }
        if (ctx.rsp_seen >= ctx.rsp_expected) {
          ctx.state = tm_mesh_txn_state_t::DONE;
          txn_ctx_.erase(ctx_it);
        }
        continue;
      }

      /*
       * 非 LOCAL 方向表示本拍要继续穿过一跳有向 link。
       * 这里会：
       * 1. 从当前 router 输入队列弹出
       * 2. 标记该输入口本拍已被使用
       * 3. 把包送入 link 的 in-flight 队列
       * 4. 更新事务状态为“在 request mesh 中”或“在 response mesh 中”
       */
      uint32_t next_router = topology_.neighbor(router_id, out_dir);
      auto dst_dir = tm_mesh_opposite_dir(out_dir);
      auto link = get_mesh_link(router_id, out_dir, next_router, dst_dir);
      if (link == nullptr || !link->can_send(time())) {
        continue;
      }

      router->pop_packet(cand.in_dir, cand.traffic_class);
      used_input_ports[input_port_idx] = 1;
      link->enqueue(cand.pld, cand.traffic_class, time());
      if (cand.traffic_class == TmMeshRouter::REQ_CLASS ||
          cand.traffic_class == TmMeshRouter::WR_DAT_CLASS) {
        ctx.state = tm_mesh_txn_state_t::IN_REQ_MESH;
      } else {
        ctx.state = tm_mesh_txn_state_t::IN_RSP_MESH;
      }
    }
  }
}

void TmMeshFabric::send_target_reqs() {
  /*
   * 第五步：把已经到达 target local queue 的请求真正送到 target/TmMem。
   *
   * 这一步和 inject_mesh_reqs() 对称：
   * - inject_mesh_reqs() 负责 “NIU -> source router”
   * - send_target_reqs() 负责 “target local queue -> target inf()”
   */
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
    /*
     * target 不 ready、credit 不够、token 不够或 busy time 未到，
     * 这些情况都在这里统一挡住，本拍先不发。
     */
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

  // 目标口如果自己还在 busy 窗口里，本拍同样不能继续发。
  if (time() < *next_issue) {
    return;
  }

  auto target_port = target_ports_[target_id];
  if (target_port != nullptr &&
      target_port->inf()->send(static_cast<uint32_t>(req_type), pld)) {
    /*
     * 只有真正送入 target inf() 成功后，才认为这拍发射成功：
     * - 弹出 target local queue
     * - 消耗 target credit
     * - 更新事务状态
     */
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

    /*
     * WR_DAT 成功送入 target 后，才真正消费掉 source NIU 里缓存的 grant。
     * 这样可以保证“grant 只在写数据真正发出去后才退休”。
     */
    if (req_type == aic_req_type_t::WR_DAT) {
      uint32_t master_port = topology_.find_master_port(pld->mst_id);
      if (master_port < master_nius_.size() &&
          master_nius_[master_port] != nullptr) {
        master_nius_[master_port]->pop_pending_grant();
      }
    }

    // 目标口发射成功后，进入一段按 flow_ctrl 计算出的 busy 时间。
    *next_issue = time() + flow_ctrl_.calc_issue_busy_cycles(target_id, pld);
  }
}
