#include "tm_mesh_router.h"

#include <utility>

using namespace tm_engine;

TmMeshRouter::TmMeshRouter()
{
}

TmMeshRouter::TmMeshRouter(const std::string& name, p_tm_clk_t clk,
                           p_tm_mesh_cfg_t cfg)
    : TmModule(name)
{
    config(name, clk, cfg);
}

TmMeshRouter::~TmMeshRouter()
{
}

void
TmMeshRouter::config(const std::string& name, p_tm_clk_t clk,
                     p_tm_mesh_cfg_t cfg)
{
    name_ = name;
    this->name(name_);
    clk_ = clk;
    cfg_ = cfg;

    req_qs_.clear();
    wr_dat_qs_.clear();
    rd_rsp_qs_.clear();
    wr_req_rsp_qs_.clear();
    wr_dat_rsp_qs_.clear();
    output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(), 0);

    for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
        req_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_req_q_" + std::to_string(port),
            cfg_->ring_req_fifo_depth));

        wr_dat_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_wr_dat_q_" + std::to_string(port),
            cfg_->ring_req_fifo_depth));

        std::vector<p_tm_com_que_t> lane_qs;
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            lane_qs.push_back(tm_make_com_que(
                clk_,
                name_ + "_rd_rsp_q_" + std::to_string(port) + "_" +
                    std::to_string(lane),
                cfg_->ring_rsp_fifo_depth));
        }
        rd_rsp_qs_.push_back(lane_qs);

        wr_req_rsp_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_wr_req_rsp_q_" + std::to_string(port),
            cfg_->ring_rsp_fifo_depth));

        wr_dat_rsp_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_wr_dat_rsp_q_" + std::to_string(port),
            cfg_->ring_rsp_fifo_depth));
    }

    auto route_req_proc = TM_MAKE_CPROC(&TmMeshRouter::route_request);
    for (auto& q : req_qs_) {
        tm_sensitive(route_req_proc, q->vld);
    }

    auto route_wr_data_proc = TM_MAKE_CPROC(&TmMeshRouter::route_write_data);
    for (auto& q : wr_dat_qs_) {
        tm_sensitive(route_wr_data_proc, q->vld);
    }

    auto route_rsp_proc = TM_MAKE_CPROC(&TmMeshRouter::route_response);
    for (auto& lane_qs : rd_rsp_qs_) {
        for (auto& q : lane_qs) {
            tm_sensitive(route_rsp_proc, q->vld);
        }
    }
    for (auto& q : wr_req_rsp_qs_) {
        tm_sensitive(route_rsp_proc, q->vld);
    }
    for (auto& q : wr_dat_rsp_qs_) {
        tm_sensitive(route_rsp_proc, q->vld);
    }

    reset();
}

void
TmMeshRouter::reset()
{
    for (auto& q : req_qs_) {
        q->clear();
    }
    for (auto& q : wr_dat_qs_) {
        q->clear();
    }
    for (auto& lane_qs : rd_rsp_qs_) {
        for (auto& q : lane_qs) {
            q->clear();
        }
    }
    for (auto& q : wr_req_rsp_qs_) {
        q->clear();
    }
    for (auto& q : wr_dat_rsp_qs_) {
        q->clear();
    }
    output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(), 0);
}

bool
TmMeshRouter::idle() const
{
    bool ret = true;
    for (const auto& q : req_qs_) {
        ret = ret && q->empty();
    }
    for (const auto& q : wr_dat_qs_) {
        ret = ret && q->empty();
    }
    for (const auto& lane_qs : rd_rsp_qs_) {
        for (const auto& q : lane_qs) {
            ret = ret && q->empty();
        }
    }
    for (const auto& q : wr_req_rsp_qs_) {
        ret = ret && q->empty();
    }
    for (const auto& q : wr_dat_rsp_qs_) {
        ret = ret && q->empty();
    }
    return ret;
}

void
TmMeshRouter::attach(uint32_t router_id, route_resolver_t route_resolver,
                     route_ready_t route_ready, route_commit_t route_commit)
{
    router_id_ = router_id;
    route_resolver_ = std::move(route_resolver);
    route_ready_ = std::move(route_ready);
    route_commit_ = std::move(route_commit);
}

void
TmMeshRouter::route_request()
{
    advance_traffic(TrafficKind::REQ);
}

void
TmMeshRouter::route_write_data()
{
    advance_traffic(TrafficKind::WR_DAT);
}

void
TmMeshRouter::route_response()
{
    advance_traffic(TrafficKind::RSP);
}

uint32_t
TmMeshRouter::traffic_class_count() const
{
    return RD_RSP_BASE_CLASS + static_cast<uint32_t>(cfg_->rd_rsp_port_num);
}

TmMeshRouter::TrafficKind
TmMeshRouter::traffic_kind(uint32_t traffic_class) const
{
    if (traffic_class == REQ_CLASS) {
        return TrafficKind::REQ;
    }
    if (traffic_class == WR_DAT_CLASS) {
        return TrafficKind::WR_DAT;
    }
    return TrafficKind::RSP;
}

p_tm_com_que_t
TmMeshRouter::req_q(TmMeshPortDir in_dir) const
{
    return req_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::wr_dat_q(TmMeshPortDir in_dir) const
{
    return wr_dat_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::rd_rsp_q(TmMeshPortDir in_dir, uint32_t lane) const
{
    return rd_rsp_qs_[tm_ring_port_index(in_dir)][lane];
}

p_tm_com_que_t
TmMeshRouter::wr_req_rsp_q(TmMeshPortDir in_dir) const
{
    return wr_req_rsp_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::wr_dat_rsp_q(TmMeshPortDir in_dir) const
{
    return wr_dat_rsp_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::queue_for_class(TmMeshPortDir in_dir,
                              uint32_t traffic_class) const
{
    uint32_t port = tm_ring_port_index(in_dir);
    if (traffic_class == REQ_CLASS) {
        return req_qs_[port];
    }
    if (traffic_class == WR_DAT_CLASS) {
        return wr_dat_qs_[port];
    }
    if (traffic_class == WR_REQ_RSP_CLASS) {
        return wr_req_rsp_qs_[port];
    }
    if (traffic_class == WR_DAT_RSP_CLASS) {
        return wr_dat_rsp_qs_[port];
    }
    return rd_rsp_qs_[port][traffic_class - RD_RSP_BASE_CLASS];
}

p_tm_pld_t
TmMeshRouter::front_packet(TmMeshPortDir in_dir, uint32_t traffic_class) const
{
    auto q = queue_for_class(in_dir, traffic_class);
    return q->empty() ? nullptr : q->front();
}

void
TmMeshRouter::pop_packet(TmMeshPortDir in_dir, uint32_t traffic_class)
{
    auto q = queue_for_class(in_dir, traffic_class);
    if (!q->empty()) {
        q->pop_front();
    }
}

bool
TmMeshRouter::select_output_candidate(TmMeshPortDir out_dir,
                                      TrafficKind traffic_kind_filter,
                                      RouteCandidate& winner)
{
    std::vector<RouteCandidate> candidates;
    uint32_t class_num = traffic_class_count();

    for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
        auto in_dir = static_cast<TmMeshPortDir>(port);
        for (uint32_t cls = 0; cls < class_num; ++cls) {
            if (traffic_kind(cls) != traffic_kind_filter) {
                continue;
            }

            auto pld = front_packet(in_dir, cls);
            if (pld == nullptr) {
                continue;
            }

            RouteCandidate cand;
            cand.valid = true;
            cand.in_dir = in_dir;
            cand.traffic_class = cls;
            cand.pld = pld;

            if (!route_resolver_(router_id_, cand) || cand.out_dir != out_dir ||
                !route_ready_(router_id_, cand)) {
                continue;
            }

            candidates.push_back(cand);
        }
    }

    uint32_t winner_id = 0;
    if (!pick_output_winner(out_dir, traffic_kind_filter, candidates.size(),
                            winner_id)) {
        return false;
    }

    winner = candidates[winner_id];
    return true;
}

void
TmMeshRouter::advance_traffic(TrafficKind traffic_kind_filter)
{
    for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
        auto out_dir = static_cast<TmMeshPortDir>(port);
        RouteCandidate winner;
        if (!select_output_candidate(out_dir, traffic_kind_filter, winner)) {
            continue;
        }

        if (route_commit_(router_id_, winner)) {
            pop_packet(winner.in_dir, winner.traffic_class);
        }
    }
}

bool
TmMeshRouter::pick_output_winner(TmMeshPortDir out_dir,
                                 TrafficKind traffic_kind,
                                 uint32_t candidate_count, uint32_t& winner)
{
    if (candidate_count == 0) {
        return false;
    }

    uint32_t subnet = traffic_kind == TrafficKind::RSP
                          ? tm_ring_subnet_index(TmRingSubnet::RSP)
                          : tm_ring_subnet_index(TmRingSubnet::REQ);
    uint32_t out_idx = subnet * tm_ring_port_count() +
                       tm_ring_port_index(out_dir);
    auto& start = output_rr_ptr_[out_idx];
    if (start >= candidate_count) {
        start = 0;
    }

    winner = start;
    start = (start + 1) % candidate_count;
    return true;
}
