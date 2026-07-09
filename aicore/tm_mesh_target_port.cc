#include "tm_mesh_target_port.h"

using namespace tm_engine;

TmMeshTargetPort::TmMeshTargetPort()
{
}

TmMeshTargetPort::TmMeshTargetPort(const std::string& name, p_tm_clk_t clk,
                                   p_tm_mesh_target_cfg_t cfg,
                                   uint32_t rd_rsp_port_num,
                                   uint32_t inf_depth)
{
    config(name, clk, cfg, rd_rsp_port_num, inf_depth);
}

TmMeshTargetPort::~TmMeshTargetPort()
{
}

void
TmMeshTargetPort::config(const std::string& name, p_tm_clk_t clk,
                         p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t inf_depth)
{
    name_ = name;
    clk_ = clk;
    cfg_ = cfg;

    // target 接口通道数 = WR_REQ / WR_DAT / RD_RSP lanes。
    uint32_t chan_num = static_cast<uint32_t>(aic_req_type_t::RD_REQ) +
                        rd_rsp_port_num;
    inf_ = tm_make_com_inf(clk_, name_ + "_inf", inf_depth);
    inf_->set_chan_num(chan_num);

    // target-local queues：在真正发给 target 前先做本地缓存。
    rd_req_q_ = tm_make_com_que(clk_, name_ + "_rd_req_q",
                                cfg_->rd_req_fifo_depth);
    wr_req_q_ = tm_make_com_que(clk_, name_ + "_wr_req_q",
                                cfg_->wr_req_fifo_depth);
    wr_dat_q_ = tm_make_com_que(clk_, name_ + "_wr_dat_q",
                                cfg_->wr_dat_fifo_depth);

    reset();
}

void
TmMeshTargetPort::reset()
{
    if (inf_ != nullptr) {
        inf_->reset();
    }
    if (rd_req_q_ != nullptr) {
        rd_req_q_->clear();
    }
    if (wr_req_q_ != nullptr) {
        wr_req_q_->clear();
    }
    if (wr_dat_q_ != nullptr) {
        wr_dat_q_->clear();
    }
}

bool
TmMeshTargetPort::idle() const
{
    return (inf_ == nullptr || inf_->idle()) &&
           (rd_req_q_ == nullptr || rd_req_q_->empty()) &&
           (wr_req_q_ == nullptr || wr_req_q_->empty()) &&
           (wr_dat_q_ == nullptr || wr_dat_q_->empty());
}

void
TmMeshTargetPort::attach_downstream(p_tm_com_inf_t inf)
{
    inf_->connect(inf);
}

void
TmMeshTargetPort::attach_downstream(p_tm_mem_t mem)
{
    if (mem != nullptr) {
        attach_downstream(mem->rw_inf_);
    }
}

p_tm_com_inf_t
TmMeshTargetPort::inf() const
{
    return inf_;
}

p_tm_com_que_t
TmMeshTargetPort::req_q(aic_req_type_t req_type) const
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return rd_req_q_;
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return wr_req_q_;
    }
    return wr_dat_q_;
}
