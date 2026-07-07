#include "tm_mesh_inf.h"

#include <algorithm>
#include <vector>

using namespace std;
using namespace tm_engine;

Tm_mesh_inf::Tm_mesh_inf(const std::string& name, p_tm_clk_t clk,
                         uint32_t inf_id, p_tm_mesh_cfg_t cfg)
    : TmModule(name), clk_(clk), cfg_(cfg), inf_id_(inf_id)
{
    config();
}

Tm_mesh_inf::~Tm_mesh_inf()
{
}

void
Tm_mesh_inf::config()
{
    uint32_t chan_num = static_cast<uint32_t>(aic_req_type_t::RD_REQ) +
                        cfg_->rd_rsp_port_num;
    bus_inf_ = tm_make_com_inf(clk_, this->name() + "_bus_inf",
                               cfg_->master_inf_depth);
    bus_inf_->set_chan_num(chan_num);

    rd_req_fifo_ = tm_make_com_que(clk_, this->name() + "_rd_req_fifo",
                                   cfg_->master_rd_req_fifo_depth);
    wr_req_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_req_fifo",
                                   cfg_->master_wr_req_fifo_depth);
    wr_dat_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_dat_fifo",
                                   cfg_->master_wr_dat_fifo_depth);

    rd_rsp_fifo_.clear();
    next_rd_rsp_issue_time_.assign(cfg_->rd_rsp_port_num, 0);
    for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
        rd_rsp_fifo_.push_back(
            tm_make_com_que(clk_, this->name() + "_rd_rsp_fifo_" +
                                      std::to_string(lane),
                            cfg_->master_rd_rsp_fifo_depth));
    }

    wr_req_rsp_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_req_rsp_fifo",
                                       cfg_->master_wr_req_rsp_fifo_depth);
    wr_dat_rsp_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_dat_rsp_fifo",
                                       cfg_->master_wr_dat_rsp_fifo_depth);

    reset();
}

void
Tm_mesh_inf::reset()
{
    bus_inf_->reset();
    rd_req_fifo_->clear();
    wr_req_fifo_->clear();
    wr_dat_fifo_->clear();
    for (auto& q : rd_rsp_fifo_) {
        q->clear();
    }
    wr_req_rsp_fifo_->clear();
    wr_dat_rsp_fifo_->clear();
    wr_grant_fifo_.clear();
    api_req_map_.clear();
    req_id_ = 0;
    std::fill(next_rd_rsp_issue_time_.begin(), next_rd_rsp_issue_time_.end(), 0);
    next_wr_req_rsp_issue_time_ = 0;
    next_wr_dat_rsp_issue_time_ = 0;
}

bool
Tm_mesh_inf::idle()
{
    bool ret = bus_inf_->idle() && rd_req_fifo_->empty() && wr_req_fifo_->empty() &&
               wr_dat_fifo_->empty() && wr_req_rsp_fifo_->empty() &&
               wr_dat_rsp_fifo_->empty() && wr_grant_fifo_.empty() &&
               api_req_map_.empty();
    for (auto& q : rd_rsp_fifo_) {
        ret = ret && q->empty();
    }
    return ret;
}

void
Tm_mesh_inf::connect_upstream(p_tm_com_inf_t inf)
{
    bus_inf_->connect(inf);
}

void
Tm_mesh_inf::bind_master_id(uint32_t mst_id)
{
    inf_id_ = mst_id;
}

uint32_t
Tm_mesh_inf::send_rd_req(uint64_t address, uint32_t size)
{
    auto req = tm_make_pld(pld_cmd_t::RD, address, size);
    req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
    req->data = pld_data_t((req->buf_u32)->data());
    req->mst_id = inf_id_;

    uint32_t cur_req_id = req_id_++;
    req->gid = cur_req_id;
    api_req_map_[cur_req_id].req_type = aic_req_type_t::RD_REQ;
    rd_req_fifo_->push_back(req);
    return cur_req_id;
}

uint32_t
Tm_mesh_inf::send_wr_req(uint64_t address, uint32_t size)
{
    auto req = tm_make_pld(pld_cmd_t::WR, address, size);
    req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
    req->data = pld_data_t((req->buf_u32)->data());
    req->mst_id = inf_id_;

    uint32_t cur_req_id = req_id_++;
    req->gid = cur_req_id;
    api_req_map_[cur_req_id].req_type = aic_req_type_t::WR_REQ;
    wr_req_fifo_->push_back(req);
    return cur_req_id;
}

bool
Tm_mesh_inf::completed(uint32_t req_id)
{
    return api_req_map_.find(req_id) == api_req_map_.end();
}

bool
Tm_mesh_inf::canSendRdReq()
{
    return !rd_req_fifo_->full();
}

bool
Tm_mesh_inf::canSendWrReq()
{
    return !wr_req_fifo_->full();
}

void
Tm_mesh_inf::ingest_upstream_reqs(uint32_t master_port,
                                  const TmMeshTopology& topology,
                                  unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
                                  tm_time_t now)
{
    const aic_req_type_t req_order[] = {
        aic_req_type_t::WR_REQ, aic_req_type_t::WR_DAT, aic_req_type_t::RD_REQ};

    for (auto req_type : req_order) {
        uint32_t chan = static_cast<uint32_t>(req_type);
        auto fifo = req_fifo(req_type);

        while (bus_inf_->valid(chan) && !fifo->full()) {
            auto pld = bus_inf_->get_pld(chan);
            if (pld->mst_id == 0) {
                pld->mst_id = inf_id_;
            }

            auto key = make_txn_key(pld);
            auto it = txn_ctx.find(key);
            if (req_type == aic_req_type_t::WR_DAT) {
                if (it == txn_ctx.end()) {
                    break;
                }
                it->second.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
            } else {
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

            fifo->push_back(pld);
            bus_inf_->pop_pld(chan);
        }
    }
}

bool
Tm_mesh_inf::has_req(aic_req_type_t req_type) const
{
    return !req_fifo(req_type)->empty();
}

p_tm_pld_t
Tm_mesh_inf::front_req(aic_req_type_t req_type) const
{
    auto fifo = req_fifo(req_type);
    return fifo->empty() ? nullptr : fifo->front();
}

void
Tm_mesh_inf::pop_req(aic_req_type_t req_type)
{
    auto fifo = req_fifo(req_type);
    if (!fifo->empty()) {
        fifo->pop_front();
    }
}

bool
Tm_mesh_inf::has_grant() const
{
    return !wr_grant_fifo_.empty();
}

const TmMeshGrant&
Tm_mesh_inf::front_grant() const
{
    return wr_grant_fifo_.front();
}

void
Tm_mesh_inf::pop_grant()
{
    if (!wr_grant_fifo_.empty()) {
        wr_grant_fifo_.pop_front();
    }
}

bool
Tm_mesh_inf::can_accept_rd_rsp(uint32_t lane) const
{
    return lane < rd_rsp_fifo_.size() && !rd_rsp_fifo_[lane]->full();
}

bool
Tm_mesh_inf::can_accept_wr_req_rsp() const
{
    return !wr_req_rsp_fifo_->full();
}

bool
Tm_mesh_inf::can_accept_wr_dat_rsp() const
{
    return !wr_dat_rsp_fifo_->full();
}

bool
Tm_mesh_inf::can_accept_grant() const
{
    return wr_grant_fifo_.size() < cfg_->master_wr_grant_fifo_depth;
}

bool
Tm_mesh_inf::accept_rd_rsp(p_tm_pld_t rsp, uint32_t lane)
{
    auto it = api_req_map_.find(rsp->gid);
    if (it != api_req_map_.end() && it->second.req_type == aic_req_type_t::RD_REQ) {
        if (it->second.rsp_expected == 1 && rsp->latency > 1) {
            it->second.rsp_expected = rsp->latency;
        }
        it->second.rsp_seen++;
        if (it->second.rsp_seen >= it->second.rsp_expected) {
            api_req_map_.erase(it);
        }
        return true;
    }

    if (!can_accept_rd_rsp(lane)) {
        return false;
    }
    rd_rsp_fifo_[lane]->push_back(rsp);
    return true;
}

bool
Tm_mesh_inf::accept_wr_req_rsp(p_tm_pld_t rsp, const TmMeshGrant& grant)
{
    auto it = api_req_map_.find(rsp->gid);
    bool is_api_write =
        it != api_req_map_.end() && it->second.req_type == aic_req_type_t::WR_REQ;

    if (!can_accept_grant() || (is_api_write && wr_dat_fifo_->full())) {
        return false;
    }
    wr_grant_fifo_.push_back(grant);

    if (is_api_write) {
        wr_dat_fifo_->push_back(rsp);
        return true;
    }

    if (!can_accept_wr_req_rsp()) {
        wr_grant_fifo_.pop_back();
        return false;
    }
    wr_req_rsp_fifo_->push_back(rsp);
    return true;
}

bool
Tm_mesh_inf::accept_wr_dat_rsp(p_tm_pld_t rsp)
{
    if (retire_api_req(rsp)) {
        return true;
    }

    if (!can_accept_wr_dat_rsp()) {
        return false;
    }
    wr_dat_rsp_fifo_->push_back(rsp);
    return true;
}

void
Tm_mesh_inf::service_rsp_outputs(const TmBusFlowCtrl& flow_ctrl,
                                 const unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
                                 tm_time_t now)
{
    for (uint32_t lane = 0; lane < rd_rsp_fifo_.size(); ++lane) {
        auto fifo = rd_rsp_fifo_[lane];
        if (fifo->empty() || now < next_rd_rsp_issue_time_[lane]) {
            continue;
        }

        auto rsp = fifo->front();
        uint32_t chan = static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
        if (bus_inf_->send(chan, rsp)) {
            fifo->pop_front();
            auto ctx_it = txn_ctx.find(make_txn_key(rsp));
            if (ctx_it != txn_ctx.end()) {
                next_rd_rsp_issue_time_[lane] =
                    now + flow_ctrl.calc_rsp_busy_cycles(ctx_it->second.target_id, rsp);
            }
        }
    }

    if (!wr_req_rsp_fifo_->empty() && now >= next_wr_req_rsp_issue_time_) {
        auto rsp = wr_req_rsp_fifo_->front();
        if (bus_inf_->send(static_cast<uint32_t>(aic_req_type_t::WR_REQ), rsp)) {
            wr_req_rsp_fifo_->pop_front();
            auto ctx_it = txn_ctx.find(make_txn_key(rsp));
            if (ctx_it != txn_ctx.end()) {
                next_wr_req_rsp_issue_time_ =
                    now + flow_ctrl.calc_rsp_busy_cycles(ctx_it->second.target_id, rsp);
            }
        }
    }

    if (!wr_dat_rsp_fifo_->empty() && now >= next_wr_dat_rsp_issue_time_) {
        auto rsp = wr_dat_rsp_fifo_->front();
        if (bus_inf_->send(static_cast<uint32_t>(aic_req_type_t::WR_DAT), rsp)) {
            wr_dat_rsp_fifo_->pop_front();
            auto ctx_it = txn_ctx.find(make_txn_key(rsp));
            if (ctx_it != txn_ctx.end()) {
                next_wr_dat_rsp_issue_time_ =
                    now + flow_ctrl.calc_rsp_busy_cycles(ctx_it->second.target_id, rsp);
            }
        }
    }
}

p_tm_com_que_t
Tm_mesh_inf::req_fifo(aic_req_type_t req_type) const
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return rd_req_fifo_;
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return wr_req_fifo_;
    }
    return wr_dat_fifo_;
}

uint64_t
Tm_mesh_inf::make_txn_key(uint32_t mst_id, uint32_t gid) const
{
    return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t
Tm_mesh_inf::make_txn_key(p_tm_pld_t pld) const
{
    return make_txn_key(pld->mst_id, pld->gid);
}

bool
Tm_mesh_inf::retire_api_req(p_tm_pld_t rsp)
{
    auto it = api_req_map_.find(rsp->gid);
    if (it == api_req_map_.end()) {
        return false;
    }
    if (it->second.req_type != aic_req_type_t::WR_REQ) {
        return false;
    }

    api_req_map_.erase(it);
    return true;
}
