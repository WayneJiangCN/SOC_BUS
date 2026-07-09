#include "tm_mesh_router.h"

using namespace tm_engine;

TmMeshRouter::TmMeshRouter()
{
}

TmMeshRouter::TmMeshRouter(const std::string& name, p_tm_clk_t clk,
                           p_tm_mesh_cfg_t cfg)
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
    clk_ = clk;
    cfg_ = cfg;

    req_qs_.clear();
    wr_dat_qs_.clear();
    rd_rsp_qs_.clear();
    wr_req_rsp_qs_.clear();
    wr_dat_rsp_qs_.clear();
    output_arb_debugs_.assign(port_count(), OutputArbDebug());

    // 每个输入方向都拥有一套 request/data/response 本地队列。
    for (uint32_t port = 0; port < port_count(); ++port) {
        req_qs_.push_back(
            tm_make_com_que(clk_, name_ + "_req_q_" + std::to_string(port),
                            cfg_->mesh_req_fifo_depth));
        wr_dat_qs_.push_back(
            tm_make_com_que(clk_, name_ + "_wr_dat_q_" + std::to_string(port),
                            cfg_->mesh_req_fifo_depth));

        std::vector<p_tm_com_que_t> lane_qs;
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            lane_qs.push_back(tm_make_com_que(
                clk_, name_ + "_rd_rsp_q_" + std::to_string(port) + "_" +
                          std::to_string(lane),
                cfg_->mesh_rsp_fifo_depth));
        }
        rd_rsp_qs_.push_back(lane_qs);

        wr_req_rsp_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_wr_req_rsp_q_" + std::to_string(port),
            cfg_->mesh_rsp_fifo_depth));
        wr_dat_rsp_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_wr_dat_rsp_q_" + std::to_string(port),
            cfg_->mesh_rsp_fifo_depth));
    }

    reset();
}

void
TmMeshRouter::reset()
{
    for (auto& q : req_qs_) {
        if (q != nullptr) {
            q->clear();
        }
    }
    for (auto& q : wr_dat_qs_) {
        if (q != nullptr) {
            q->clear();
        }
    }
    for (auto& lane_qs : rd_rsp_qs_) {
        for (auto& q : lane_qs) {
            if (q != nullptr) {
                q->clear();
            }
        }
    }
    for (auto& q : wr_req_rsp_qs_) {
        if (q != nullptr) {
            q->clear();
        }
    }
    for (auto& q : wr_dat_rsp_qs_) {
        if (q != nullptr) {
            q->clear();
        }
    }
    output_arb_debugs_.assign(port_count(), OutputArbDebug());
    output_rr_ptr_.clear();
}

bool
TmMeshRouter::idle() const
{
    bool ret = true;
    for (const auto& q : req_qs_) {
        ret = ret && (q == nullptr || q->empty());
    }
    for (const auto& q : wr_dat_qs_) {
        ret = ret && (q == nullptr || q->empty());
    }
    for (const auto& lane_qs : rd_rsp_qs_) {
        for (const auto& q : lane_qs) {
            ret = ret && (q == nullptr || q->empty());
        }
    }
    for (const auto& q : wr_req_rsp_qs_) {
        ret = ret && (q == nullptr || q->empty());
    }
    for (const auto& q : wr_dat_rsp_qs_) {
        ret = ret && (q == nullptr || q->empty());
    }
    return ret;
}

uint32_t
TmMeshRouter::port_count() const
{
    return tm_mesh_port_count();
}

uint32_t
TmMeshRouter::traffic_class_count() const
{
    // 固定类数量 + 可配置的 RD_RSP lane 数量。
    return RD_RSP_BASE_CLASS + static_cast<uint32_t>(cfg_->rd_rsp_port_num);
}

uint32_t
TmMeshRouter::contender_count() const
{
    return port_count() * traffic_class_count();
}

uint32_t
TmMeshRouter::contender_id(TmMeshPortDir in_dir, uint32_t traffic_class) const
{
    return tm_mesh_port_index(in_dir) * traffic_class_count() + traffic_class;
}

uint32_t
TmMeshRouter::contender_input_port(uint32_t contender) const
{
    return traffic_class_count() == 0 ? 0 : contender / traffic_class_count();
}

uint32_t
TmMeshRouter::contender_traffic_class(uint32_t contender) const
{
    return traffic_class_count() == 0 ? 0 : contender % traffic_class_count();
}

TmMeshPortDir
TmMeshRouter::contender_input_dir(uint32_t contender) const
{
    return static_cast<TmMeshPortDir>(contender_input_port(contender));
}

TmMeshRouter::ArbTrafficKind
TmMeshRouter::traffic_kind(uint32_t traffic_class) const
{
    if (traffic_class == REQ_CLASS) {
        return ArbTrafficKind::REQ;
    }
    if (traffic_class == WR_DAT_CLASS) {
        return ArbTrafficKind::WR_DAT;
    }
    if (traffic_class >= WR_REQ_RSP_CLASS) {
        return ArbTrafficKind::RSP;
    }
    return ArbTrafficKind::NONE;
}

p_tm_com_que_t
TmMeshRouter::req_q(TmMeshPortDir in_dir) const
{
    return req_qs_[tm_mesh_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::wr_dat_q(TmMeshPortDir in_dir) const
{
    return wr_dat_qs_[tm_mesh_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::rd_rsp_q(TmMeshPortDir in_dir, uint32_t lane) const
{
    return rd_rsp_qs_[tm_mesh_port_index(in_dir)][lane];
}

p_tm_com_que_t
TmMeshRouter::wr_req_rsp_q(TmMeshPortDir in_dir) const
{
    return wr_req_rsp_qs_[tm_mesh_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::wr_dat_rsp_q(TmMeshPortDir in_dir) const
{
    return wr_dat_rsp_qs_[tm_mesh_port_index(in_dir)];
}

p_tm_com_que_t
TmMeshRouter::queue_for_class(TmMeshPortDir in_dir, uint32_t traffic_class) const
{
    uint32_t port = tm_mesh_port_index(in_dir);
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

    uint32_t lane = traffic_class - RD_RSP_BASE_CLASS;
    if (lane < cfg_->rd_rsp_port_num) {
        return rd_rsp_qs_[port][lane];
    }
    return nullptr;
}

p_tm_pld_t
TmMeshRouter::front_packet(TmMeshPortDir in_dir, uint32_t traffic_class) const
{
    auto q = queue_for_class(in_dir, traffic_class);
    return (q == nullptr || q->empty()) ? nullptr : q->front();
}

void
TmMeshRouter::pop_packet(TmMeshPortDir in_dir, uint32_t traffic_class)
{
    auto q = queue_for_class(in_dir, traffic_class);
    if (q != nullptr && !q->empty()) {
        q->pop_front();
    }
}

bool
TmMeshRouter::pick_output_winner(TmMeshPortDir out_dir,
                                 const std::vector<uint8_t>& eligible_mask,
                                 uint32_t& winner)
{
    winner = 0;
    if (eligible_mask.empty()) {
        return false;
    }

    uint32_t out_key = tm_mesh_port_index(out_dir);
    auto& dbg = output_arb_debugs_[out_key];
    dbg.arbitration_rounds++;
    dbg.last_eligible_count = 0;
    bool has_req = false;
    bool has_wr_dat = false;
    bool has_rsp = false;

    for (uint32_t contender = 0; contender < eligible_mask.size(); ++contender) {
        if (eligible_mask[contender] == 0) {
            continue;
        }

        dbg.last_eligible_count++;
        switch (traffic_kind(contender_traffic_class(contender))) {
          case ArbTrafficKind::REQ:
            dbg.req_eligible_contenders++;
            has_req = true;
            break;
          case ArbTrafficKind::WR_DAT:
            dbg.wr_dat_eligible_contenders++;
            has_wr_dat = true;
            break;
          case ArbTrafficKind::RSP:
            dbg.rsp_eligible_contenders++;
            has_rsp = true;
            break;
          case ArbTrafficKind::NONE:
          default:
            break;
        }
    }
    if (dbg.last_eligible_count > 1) {
        dbg.contention_rounds++;
    }
    if (has_req) {
        dbg.req_eligible_rounds++;
    }
    if (has_wr_dat) {
        dbg.wr_dat_eligible_rounds++;
    }
    if (has_rsp) {
        dbg.rsp_eligible_rounds++;
    }

    uint32_t& start = output_rr_ptr_[out_key];
    if (start >= eligible_mask.size()) {
        start = 0;
    }

    // 简单 RR：从上次 winner 的下一个 contender 开始扫描。
    for (uint32_t step = 0; step < eligible_mask.size(); ++step) {
        uint32_t id = (start + step) % eligible_mask.size();
        if (eligible_mask[id] == 0) {
            continue;
        }

        winner = id;
        start = (id + 1) % eligible_mask.size();
        dbg.last_winner_contender = id;
        dbg.last_winner_class = contender_traffic_class(id);
        dbg.last_winner_in_dir = contender_input_dir(id);
        dbg.last_winner_kind = traffic_kind(dbg.last_winner_class);
        switch (dbg.last_winner_kind) {
          case ArbTrafficKind::REQ:
            dbg.req_wins++;
            break;
          case ArbTrafficKind::WR_DAT:
            dbg.wr_dat_wins++;
            break;
          case ArbTrafficKind::RSP:
            dbg.rsp_wins++;
            break;
          case ArbTrafficKind::NONE:
          default:
            break;
        }
        return true;
    }
    return false;
}

const TmMeshRouter::OutputArbDebug&
TmMeshRouter::output_arb_debug(TmMeshPortDir out_dir) const
{
    return output_arb_debugs_[tm_mesh_port_index(out_dir)];
}
