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

    req_q_ = tm_make_com_que(clk_, name_ + "_req_q", cfg_->mesh_req_fifo_depth);
    wr_dat_q_ =
        tm_make_com_que(clk_, name_ + "_wr_dat_q", cfg_->mesh_req_fifo_depth);

    rd_rsp_qs_.clear();
    for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
        rd_rsp_qs_.push_back(tm_make_com_que(
            clk_, name_ + "_rd_rsp_q_" + std::to_string(lane),
            cfg_->mesh_rsp_fifo_depth));
    }

    wr_req_rsp_q_ = tm_make_com_que(clk_, name_ + "_wr_req_rsp_q",
                                    cfg_->mesh_rsp_fifo_depth);
    wr_dat_rsp_q_ = tm_make_com_que(clk_, name_ + "_wr_dat_rsp_q",
                                    cfg_->mesh_rsp_fifo_depth);

    reset();
}

void
TmMeshRouter::reset()
{
    if (req_q_ != nullptr) {
        req_q_->clear();
    }
    if (wr_dat_q_ != nullptr) {
        wr_dat_q_->clear();
    }
    for (auto& q : rd_rsp_qs_) {
        if (q != nullptr) {
            q->clear();
        }
    }
    if (wr_req_rsp_q_ != nullptr) {
        wr_req_rsp_q_->clear();
    }
    if (wr_dat_rsp_q_ != nullptr) {
        wr_dat_rsp_q_->clear();
    }
    output_rr_ptr_.clear();
}

bool
TmMeshRouter::idle() const
{
    bool ret = (req_q_ == nullptr || req_q_->empty()) &&
               (wr_dat_q_ == nullptr || wr_dat_q_->empty()) &&
               (wr_req_rsp_q_ == nullptr || wr_req_rsp_q_->empty()) &&
               (wr_dat_rsp_q_ == nullptr || wr_dat_rsp_q_->empty());
    for (const auto& q : rd_rsp_qs_) {
        ret = ret && (q == nullptr || q->empty());
    }
    return ret;
}

p_tm_com_que_t
TmMeshRouter::req_q() const
{
    return req_q_;
}

p_tm_com_que_t
TmMeshRouter::wr_dat_q() const
{
    return wr_dat_q_;
}

p_tm_com_que_t
TmMeshRouter::rd_rsp_q(uint32_t lane) const
{
    return rd_rsp_qs_[lane];
}

p_tm_com_que_t
TmMeshRouter::wr_req_rsp_q() const
{
    return wr_req_rsp_q_;
}

p_tm_com_que_t
TmMeshRouter::wr_dat_rsp_q() const
{
    return wr_dat_rsp_q_;
}

uint32_t
TmMeshRouter::traffic_class_count() const
{
    return RD_RSP_BASE_CLASS + static_cast<uint32_t>(rd_rsp_qs_.size());
}

p_tm_com_que_t
TmMeshRouter::queue_for_class(uint32_t traffic_class) const
{
    if (traffic_class == REQ_CLASS) {
        return req_q_;
    }
    if (traffic_class == WR_DAT_CLASS) {
        return wr_dat_q_;
    }
    if (traffic_class == WR_REQ_RSP_CLASS) {
        return wr_req_rsp_q_;
    }
    if (traffic_class == WR_DAT_RSP_CLASS) {
        return wr_dat_rsp_q_;
    }

    uint32_t lane = traffic_class - RD_RSP_BASE_CLASS;
    if (lane < rd_rsp_qs_.size()) {
        return rd_rsp_qs_[lane];
    }
    return nullptr;
}

p_tm_pld_t
TmMeshRouter::front_packet(uint32_t traffic_class) const
{
    auto q = queue_for_class(traffic_class);
    return (q == nullptr || q->empty()) ? nullptr : q->front();
}

void
TmMeshRouter::pop_packet(uint32_t traffic_class)
{
    auto q = queue_for_class(traffic_class);
    if (q != nullptr && !q->empty()) {
        q->pop_front();
    }
}

bool
TmMeshRouter::pick_output_winner(uint64_t output_key,
                                 const std::vector<uint8_t>& eligible_mask,
                                 uint32_t& winner)
{
    winner = 0;
    if (eligible_mask.empty()) {
        return false;
    }

    uint32_t& start = output_rr_ptr_[output_key];
    if (start >= eligible_mask.size()) {
        start = 0;
    }

    for (uint32_t step = 0; step < eligible_mask.size(); ++step) {
        uint32_t cls = (start + step) % eligible_mask.size();
        if (eligible_mask[cls] == 0) {
            continue;
        }

        winner = cls;
        start = (cls + 1) % eligible_mask.size();
        return true;
    }
    return false;
}
