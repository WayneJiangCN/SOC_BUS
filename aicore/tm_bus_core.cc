#include "tm_bus.h"

#include <algorithm>

using namespace std;
using namespace tm_engine;

/*
 * tm_bus_core.cc
 *
 * 这里放 ring 版本 fabric 的框架逻辑：
 * - 资源创建
 * - reset / idle / tick
 * - attach / bind
 *
 * 真正的数据流推进见：
 * - tm_bus_req.cc
 * - tm_bus_rsp.cc
 */

TmBusFabric::TmBusFabric()
{
}

TmBusFabric::TmBusFabric(p_tm_clk_t clk, p_tm_bus_cfg_t cfg)
    : clk_(clk), cfg_(cfg)
{
    this->name(cfg_->name);
    config();
}

TmBusFabric::~TmBusFabric()
{
}

void
TmBusFabric::config()
{
    /* 先配置子模块，再根据拓扑创建 ring 需要的所有 FIFO 和时间状态。 */
    topology_.config(cfg_);
    flow_ctrl_.config(cfg_);
    arbiter_.config(cfg_);

    ring_node_count_ = topology_.ring_node_count();
    ring_link_latency_ = cfg_->ring_link_latency;

    const uint32_t chan_num = static_cast<uint32_t>(aic_req_type_t::RD_REQ) +
                              cfg_->rd_rsp_port_num;

    v_master_inf_.clear();
    v_target_inf_.clear();

    m_rd_req_fifo_.clear();
    m_wr_req_fifo_.clear();
    m_wr_dat_fifo_.clear();
    m_rd_rsp_fifo_.clear();
    m_wr_req_rsp_fifo_.clear();
    m_wr_dat_rsp_fifo_.clear();
    m_wr_grant_fifo_.clear();

    t_rd_req_fifo_.clear();
    t_wr_req_fifo_.clear();
    t_wr_dat_fifo_.clear();

    n_rd_req_fifo_.clear();
    n_wr_req_fifo_.clear();
    n_wr_dat_fifo_.clear();
    n_rd_rsp_fifo_.clear();
    n_wr_req_rsp_fifo_.clear();
    n_wr_dat_rsp_fifo_.clear();

    next_rd_issue_time_.assign(cfg_->num_targets, 0);
    next_wr_req_issue_time_.assign(cfg_->num_targets, 0);
    next_wr_dat_issue_time_.assign(cfg_->num_targets, 0);
    next_rd_rsp_issue_time_.resize(cfg_->num_masters);
    next_wr_req_rsp_issue_time_.assign(cfg_->num_masters, 0);
    next_wr_dat_rsp_issue_time_.assign(cfg_->num_masters, 0);

    next_n_rd_req_hop_time_.assign(ring_node_count_, 0);
    next_n_wr_req_hop_time_.assign(ring_node_count_, 0);
    next_n_wr_dat_hop_time_.assign(ring_node_count_, 0);
    next_n_rd_rsp_hop_time_.assign(ring_node_count_, 0);
    next_n_wr_req_rsp_hop_time_.assign(ring_node_count_, 0);
    next_n_wr_dat_rsp_hop_time_.assign(ring_node_count_, 0);

    topology_.reset(cfg_->num_masters);

    /* 为每个 master 创建入口接口、入口 FIFO 和回包 FIFO。 */
    for (uint32_t i = 0; i < cfg_->num_masters; ++i) {
        auto master_inf = tm_make_com_inf(clk_, this->name() + "_master_inf" +
                                          std::to_string(i),
                                          cfg_->master_inf_depth);
        master_inf->set_chan_num(chan_num);
        v_master_inf_.push_back(master_inf);

        auto rd_fifo = tm_make_com_que(clk_, this->name() + "_m_rd_req_fifo" +
                                                 std::to_string(i),
                                       cfg_->master_rd_req_fifo_depth);
        auto wr_req_fifo = tm_make_com_que(clk_,
                                           this->name() + "_m_wr_req_fifo" +
                                               std::to_string(i),
                                           cfg_->master_wr_req_fifo_depth);
        auto wr_dat_fifo = tm_make_com_que(clk_,
                                           this->name() + "_m_wr_dat_fifo" +
                                               std::to_string(i),
                                           cfg_->master_wr_dat_fifo_depth);
        m_rd_req_fifo_.push_back(rd_fifo);
        m_wr_req_fifo_.push_back(wr_req_fifo);
        m_wr_dat_fifo_.push_back(wr_dat_fifo);

        next_rd_rsp_issue_time_[i].assign(cfg_->rd_rsp_port_num, 0);
        m_rd_rsp_fifo_.emplace_back();
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            auto rd_rsp_fifo =
                tm_make_com_que(clk_, this->name() + "_m_rd_rsp_fifo" +
                                          std::to_string(i) + "_" +
                                          std::to_string(lane),
                                cfg_->master_rd_rsp_fifo_depth);
            m_rd_rsp_fifo_[i].push_back(rd_rsp_fifo);
        }

        auto wr_req_rsp_fifo =
            tm_make_com_que(clk_, this->name() + "_m_wr_req_rsp_fifo" +
                                      std::to_string(i),
                            cfg_->master_wr_req_rsp_fifo_depth);
        auto wr_dat_rsp_fifo =
            tm_make_com_que(clk_, this->name() + "_m_wr_dat_rsp_fifo" +
                                      std::to_string(i),
                            cfg_->master_wr_dat_rsp_fifo_depth);
        m_wr_req_rsp_fifo_.push_back(wr_req_rsp_fifo);
        m_wr_dat_rsp_fifo_.push_back(wr_dat_rsp_fifo);
        m_wr_grant_fifo_.emplace_back();
    }

    /* 为每个 target 创建本地接收 FIFO。 */
    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];

        auto target_inf = tm_make_com_inf(clk_, this->name() + "_target_inf" +
                                          std::to_string(i),
                                          cfg_->target_inf_depth);
        target_inf->set_chan_num(chan_num);
        v_target_inf_.push_back(target_inf);

        auto rd_fifo = tm_make_com_que(clk_, this->name() + "_t_rd_req_fifo" +
                                                 std::to_string(i),
                                       target_cfg->rd_req_fifo_depth);
        auto wr_req_fifo = tm_make_com_que(clk_,
                                           this->name() + "_t_wr_req_fifo" +
                                               std::to_string(i),
                                           target_cfg->wr_req_fifo_depth);
        auto wr_dat_fifo = tm_make_com_que(clk_,
                                           this->name() + "_t_wr_dat_fifo" +
                                               std::to_string(i),
                                           target_cfg->wr_dat_fifo_depth);
        t_rd_req_fifo_.push_back(rd_fifo);
        t_wr_req_fifo_.push_back(wr_req_fifo);
        t_wr_dat_fifo_.push_back(wr_dat_fifo);
    }

    /* 为 ring 中每个节点创建在途 request/response FIFO。 */
    n_rd_rsp_fifo_.resize(ring_node_count_);
    for (uint32_t node = 0; node < ring_node_count_; ++node) {
        auto rd_req_fifo = tm_make_com_que(clk_,
                                           this->name() + "_n_rd_req_fifo_" +
                                               std::to_string(node),
                                           cfg_->ring_req_fifo_depth);
        auto wr_req_fifo = tm_make_com_que(clk_,
                                           this->name() + "_n_wr_req_fifo_" +
                                               std::to_string(node),
                                           cfg_->ring_req_fifo_depth);
        auto wr_dat_fifo = tm_make_com_que(clk_,
                                           this->name() + "_n_wr_dat_fifo_" +
                                               std::to_string(node),
                                           cfg_->ring_req_fifo_depth);
        n_rd_req_fifo_.push_back(rd_req_fifo);
        n_wr_req_fifo_.push_back(wr_req_fifo);
        n_wr_dat_fifo_.push_back(wr_dat_fifo);

        auto wr_req_rsp_fifo = tm_make_com_que(
            clk_, this->name() + "_n_wr_req_rsp_fifo_" + std::to_string(node),
            cfg_->ring_rsp_fifo_depth);
        auto wr_dat_rsp_fifo = tm_make_com_que(
            clk_, this->name() + "_n_wr_dat_rsp_fifo_" + std::to_string(node),
            cfg_->ring_rsp_fifo_depth);
        n_wr_req_rsp_fifo_.push_back(wr_req_rsp_fifo);
        n_wr_dat_rsp_fifo_.push_back(wr_dat_rsp_fifo);

        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            auto rd_rsp_fifo =
                tm_make_com_que(clk_, this->name() + "_n_rd_rsp_fifo_" +
                                          std::to_string(node) + "_" +
                                          std::to_string(lane),
                                cfg_->ring_rsp_fifo_depth);
            n_rd_rsp_fifo_[node].push_back(rd_rsp_fifo);
        }
    }

    tm_sensitive(TM_MAKE_CPROC(&TmBusFabric::tick), clk_->pos_edge);
    reset();
}

void
TmBusFabric::build()
{
}

void
TmBusFabric::reset()
{
    /* reset 要把所有可见状态清空，避免事务跨次运行残留。 */
    txn_ctx_.clear();

    for (auto& inf : v_master_inf_) {
        inf->reset();
    }
    for (auto& inf : v_target_inf_) {
        inf->reset();
    }

    for (auto& q : m_rd_req_fifo_) {
        q->clear();
    }
    for (auto& q : m_wr_req_fifo_) {
        q->clear();
    }
    for (auto& q : m_wr_dat_fifo_) {
        q->clear();
    }

    for (auto& q : t_rd_req_fifo_) {
        q->clear();
    }
    for (auto& q : t_wr_req_fifo_) {
        q->clear();
    }
    for (auto& q : t_wr_dat_fifo_) {
        q->clear();
    }

    for (auto& q : n_rd_req_fifo_) {
        q->clear();
    }
    for (auto& q : n_wr_req_fifo_) {
        q->clear();
    }
    for (auto& q : n_wr_dat_fifo_) {
        q->clear();
    }

    for (auto& lanes : n_rd_rsp_fifo_) {
        for (auto& q : lanes) {
            q->clear();
        }
    }
    for (auto& q : n_wr_req_rsp_fifo_) {
        q->clear();
    }
    for (auto& q : n_wr_dat_rsp_fifo_) {
        q->clear();
    }

    for (auto& lanes : m_rd_rsp_fifo_) {
        for (auto& q : lanes) {
            q->clear();
        }
    }
    for (auto& q : m_wr_req_rsp_fifo_) {
        q->clear();
    }
    for (auto& q : m_wr_dat_rsp_fifo_) {
        q->clear();
    }

    for (auto& grants : m_wr_grant_fifo_) {
        grants.clear();
    }

    std::fill(next_rd_issue_time_.begin(), next_rd_issue_time_.end(), 0);
    std::fill(next_wr_req_issue_time_.begin(), next_wr_req_issue_time_.end(), 0);
    std::fill(next_wr_dat_issue_time_.begin(), next_wr_dat_issue_time_.end(), 0);
    std::fill(next_wr_req_rsp_issue_time_.begin(),
              next_wr_req_rsp_issue_time_.end(), 0);
    std::fill(next_wr_dat_rsp_issue_time_.begin(),
              next_wr_dat_rsp_issue_time_.end(), 0);
    std::fill(next_n_rd_req_hop_time_.begin(), next_n_rd_req_hop_time_.end(), 0);
    std::fill(next_n_wr_req_hop_time_.begin(), next_n_wr_req_hop_time_.end(), 0);
    std::fill(next_n_wr_dat_hop_time_.begin(), next_n_wr_dat_hop_time_.end(), 0);
    std::fill(next_n_rd_rsp_hop_time_.begin(), next_n_rd_rsp_hop_time_.end(), 0);
    std::fill(next_n_wr_req_rsp_hop_time_.begin(),
              next_n_wr_req_rsp_hop_time_.end(), 0);
    std::fill(next_n_wr_dat_rsp_hop_time_.begin(),
              next_n_wr_dat_rsp_hop_time_.end(), 0);
    for (auto& lanes : next_rd_rsp_issue_time_) {
        std::fill(lanes.begin(), lanes.end(), 0);
    }

    flow_ctrl_.reset();
    arbiter_.reset();
}

bool
TmBusFabric::idle()
{
    bool ret = txn_ctx_.empty();

    for (auto& inf : v_master_inf_) {
        ret = ret && inf->idle();
    }
    for (auto& inf : v_target_inf_) {
        ret = ret && inf->idle();
    }

    for (auto& q : m_rd_req_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : m_wr_req_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : m_wr_dat_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : t_rd_req_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : t_wr_req_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : t_wr_dat_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : n_rd_req_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : n_wr_req_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : n_wr_dat_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& lanes : n_rd_rsp_fifo_) {
        for (auto& q : lanes) {
            ret = ret && q->empty();
        }
    }
    for (auto& q : n_wr_req_rsp_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : n_wr_dat_rsp_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& lanes : m_rd_rsp_fifo_) {
        for (auto& q : lanes) {
            ret = ret && q->empty();
        }
    }
    for (auto& q : m_wr_req_rsp_fifo_) {
        ret = ret && q->empty();
    }
    for (auto& q : m_wr_dat_rsp_fifo_) {
        ret = ret && q->empty();
    }
    for (const auto& grants : m_wr_grant_fifo_) {
        ret = ret && grants.empty();
    }
    return ret;
}

void
TmBusFabric::tick()
{
    /*
     * 固定按“回程优先、去程其次”的顺序推进：
     * 1. 更新 target token
     * 2. 处理回包并尽快释放 credit
     * 3. 再接收并推进新的请求
     */
    flow_ctrl_.update_tokens(time());
    recv_target_rsps();
    advance_ring_rsps();
    send_master_rsps();
    recv_master_reqs();
    inject_ring_reqs();
    advance_ring_reqs();
    send_target_reqs();
}

void
TmBusFabric::attach_master(uint32_t idx, p_tm_com_inf_t inf)
{
    v_master_inf_[idx]->connect(inf);
}

void
TmBusFabric::attach_master(uint32_t idx, p_pem_biu_t biu)
{
    attach_master(idx, biu->out_intf_);
    bind_master_id(idx, biu->core_id_);
}

void
TmBusFabric::attach_master(p_pem_biu_t biu)
{
    attach_master(biu->core_id_, biu);
}

void
TmBusFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf)
{
    v_target_inf_[idx]->connect(inf);
}

void
TmBusFabric::attach_target(uint32_t idx, p_tm_mem_t mem)
{
    attach_target(idx, mem->rw_inf_);
}

void
TmBusFabric::bind_master_id(uint32_t port_id, uint32_t mst_id)
{
    /* 让物理端口号和协议里的 master_id 解耦。 */
    topology_.bind_master_id(port_id, mst_id);
}

p_tm_com_que_t
TmBusFabric::get_master_fifo(uint32_t master_port, aic_req_type_t req_type) const
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return m_rd_req_fifo_[master_port];
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return m_wr_req_fifo_[master_port];
    }
    return m_wr_dat_fifo_[master_port];
}

p_tm_com_que_t
TmBusFabric::get_target_fifo(uint32_t target_id, aic_req_type_t req_type) const
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return t_rd_req_fifo_[target_id];
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return t_wr_req_fifo_[target_id];
    }
    return t_wr_dat_fifo_[target_id];
}

p_tm_com_que_t
TmBusFabric::get_ring_req_fifo(uint32_t node_id, aic_req_type_t req_type) const
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return n_rd_req_fifo_[node_id];
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return n_wr_req_fifo_[node_id];
    }
    return n_wr_dat_fifo_[node_id];
}

p_tm_com_que_t
TmBusFabric::get_ring_rd_rsp_fifo(uint32_t node_id, uint32_t lane) const
{
    return n_rd_rsp_fifo_[node_id][lane];
}

p_tm_com_que_t
TmBusFabric::get_ring_wr_req_rsp_fifo(uint32_t node_id) const
{
    return n_wr_req_rsp_fifo_[node_id];
}

p_tm_com_que_t
TmBusFabric::get_ring_wr_dat_rsp_fifo(uint32_t node_id) const
{
    return n_wr_dat_rsp_fifo_[node_id];
}

uint64_t
TmBusFabric::make_txn_key(uint32_t mst_id, uint32_t gid) const
{
    return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t
TmBusFabric::make_txn_key(p_tm_pld_t pld) const
{
    return make_txn_key(pld->mst_id, pld->gid);
}
