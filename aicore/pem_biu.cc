#include "pem_biu.h"
#include "types.h"

using namespace std;
using namespace tm_engine;

//-------------------------------------------------------------------------------------------------
PemBiu::PemBiu(const std::string& name, p_tm_clk_t clk, cfg::p_cfg_t cfg)
: TmModule(name)
, clk_(clk)
, cfg_(cfg)
{
    config();
}

PemBiu::~PemBiu()
{
}

//-------------------------------------------------------------------------------------------------
// APIs
void PemBiu::config() {
    // parameters
    ifu_port_num_ = cfg_->get_cfg<int32_t>("icache_port_num");
    dcache_port_num_ = cfg_->get_cfg<int32_t>("dcache_port_num");
    max_rd_otsd_ = cfg_->get_cfg<int32_t>("read_queue_size");
    max_wr_otsd_ = cfg_->get_cfg<int32_t>("write_queue_size");

    enable_memmap_             = (cfg_->get_cfg<int32_t>("enable_memmap") == 1);
    memory_entry_num_          = cfg_->get_cfg<int64_t>("mem_entry_num");
    memory_entry_size_         = cfg_->get_cfg<int64_t>("mem_entry_size");
    physical_memory_entry_num_ = cfg_->get_cfg<int64_t>("phy_mem_entry_num");
    l2_buf_size_               = memory_entry_num_ * memory_entry_size_;
    //log
    std::string rd_log_name = this->name() + ".rd_log";
    log_para_t  rd_log_para = log_para_t(rd_log_name);
    rd_log_ = pem_log::create_logger(rd_log_para);

    std::string wr_log_name = this->name() + ".wr_log";
    log_para_t  wr_log_para = log_para_t(wr_log_name);
    wr_log_ = pem_log::create_logger(wr_log_para);

    std::string log_name = this->name() + ".log";
    log_para_t  log_para = log_para_t(log_name);
    log_ = pem_log::create_logger(log_para);
    
    // instantiation
    rd_port_num = cfg_->get_cfg<int32_t>("bus_read_port_num");
    out_intf_ = tm_make_com_inf(clk_, "out_intf_", OUT_INF_DEPTH);
    out_intf_->set_chan_num(4);
    for(uint32_t i = 0; i < ifu_port_num_; ++i)
    {
        v_ifu_inf_.push_back(tm_make_com_inf(clk_, "biu_ifu_inf", BIU_INF_DEPTH));
    }
    ifu_rr_arb_ = make_shared<RR_Arb>(ifu_port_num_);
    for(uint32_t i = 0; i < dcache_port_num_; ++i)
    {
        v_dcache_rd_inf_.push_back(tm_make_com_inf(clk_, "biu_dcache_rd_inf", BIU_INF_DEPTH)); 
        v_dcache_wr_inf_.push_back(tm_make_com_inf(clk_, "biu_dcache_wr_inf", BIU_INF_DEPTH)); 
        v_dcache_wr_dat_que_.push_back(tm_make_com_que(clk_, "biu_dcache_wr_inf", BIU_INF_DEPTH)); 
    }
    dcache_rd_rr_arb_ = make_shared<RR_Arb>(dcache_port_num_);
    dcache_wr_rr_arb_ = make_shared<RR_Arb>(dcache_port_num_);

    mte_rd_inf_ = tm_make_com_inf(clk_, "biu_mte_rd_inf0", BIU_INF_DEPTH);
    mte_rd_inf_->set_chan_num(rd_port_num);
    mte_wr_inf_ = tm_make_com_inf(clk_, "biu_mte_wr_inf", BIU_INF_DEPTH);
    mte_wr_dat_inf_ = tm_make_com_inf(clk_, "biu_mte_wr_dat_inf_", BIU_INF_DEPTH);
    
    rd_cmds_ = tm_make_com_que(clk_, "biu_rd_cmds");
    wr_cmds_ = tm_make_com_que(clk_, "biu_wr_cmds");
    wr_data_ = tm_make_com_que(clk_, "biu_wr_data");
    // sensitive list
    // mte/ifu cmd
    auto recv_rd_proc = TM_MAKE_CPROC(&PemBiu::recv_rd_cmd);
    for(auto& it: v_ifu_inf_)
    {
        tm_sensitive(recv_rd_proc, it->vld);
    }
    for(auto& it: v_dcache_rd_inf_)
    {
        tm_sensitive(recv_rd_proc, it->vld);
    }
    tm_sensitive(recv_rd_proc, mte_rd_inf_->vld);

    auto recv_wr_proc = TM_MAKE_CPROC(&PemBiu::recv_wr_cmd);
    for(auto& it: v_dcache_wr_inf_)
    {
        tm_sensitive(recv_wr_proc, it->vld);
    }
    tm_sensitive(recv_wr_proc, mte_wr_inf_->vld);

    auto recv_wr_data_proc = TM_MAKE_CPROC(&PemBiu::recv_wr_dat);
    for(auto& it: v_dcache_wr_dat_que_)
    {
        tm_sensitive(recv_wr_data_proc, it->vld);
    }
    tm_sensitive(recv_wr_data_proc, mte_wr_dat_inf_->vld);
    // cmd_que
    //auto send_rw_proc = TM_MAKE_CPROC(&PemBiu::send_rw_cmd);
    //tm_sensitive(TM_MAKE_CPROC(&PemBiu::send_rd_cmd), rd_cmds_->vld);
    //tm_sensitive(TM_MAKE_CPROC(&PemBiu::send_wr_cmd), wr_cmds_->vld);
    tm_sensitive(TM_MAKE_CPROC(&PemBiu::send_rd_cmd), rd_cmds_->vld);
    tm_sensitive(TM_MAKE_CPROC(&PemBiu::send_wr_cmd), wr_cmds_->vld);
    tm_sensitive(TM_MAKE_CPROC(&PemBiu::send_wr_dat), wr_data_->vld);
    // rsp
    tm_sensitive(TM_MAKE_CPROC(&PemBiu::recv_rd_cmd_rsp), out_intf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&PemBiu::recv_wr_cmd_rsp), out_intf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&PemBiu::recv_wr_dat_rsp), out_intf_->vld);

    pmu_ = stats::stat->get_pmu(this->name());
    pmu_->register_event("biu.read_bandwidth"    , pmu_type::BW_TYPE);
    pmu_->register_event("biu.write_bandwidth"   , pmu_type::BW_TYPE);
    pmu_->register_event("biu.read_outstanding"    , pmu_type::NUMBER_TYPE);
    pmu_->register_event("biu.write_outstanding"   , pmu_type::NUMBER_TYPE);
}

void PemBiu::build() {

}

void PemBiu::reset() {
    out_intf_->reset();
    for(auto& it: v_ifu_inf_)
    {
        it->reset();
    }
    for(auto& it: v_dcache_rd_inf_)
    {
        it->reset();
    }
    for(auto& it: v_dcache_wr_inf_)
    {
        it->reset();
    }
    for(auto& it: v_dcache_wr_dat_que_)
    {
        it->clear();
    }
    mte_rd_inf_->reset();
    mte_wr_inf_->reset();
    mte_wr_dat_inf_->reset();
    rd_cmds_->clear();
    wr_cmds_->clear();
    wr_data_->clear();
    dbid_req_.clear();
    rd_otsd_ = 0;
    wr_otsd_ = 0;
}

bool PemBiu::idle() {
    bool ret = true;
    ret = ret && out_intf_->idle();
    for(auto& it: v_ifu_inf_)
    {
        ret = ret && it->idle();
    }
    for(auto& it: v_dcache_rd_inf_)
    {
        ret = ret && it->idle();
    }
    for(auto& it: v_dcache_wr_inf_)
    {
        ret = ret && it->idle();
    }
    for(auto& it: v_dcache_wr_dat_que_)
    {
        ret = ret && it->empty();
    }
    ret = ret && mte_rd_inf_->idle();
    ret = ret && mte_wr_inf_->idle();
    ret = ret && mte_wr_dat_inf_->idle();
    ret = ret && rd_cmds_->empty();
    ret = ret && wr_cmds_->empty();
    ret = ret && wr_data_->empty();
    ret = ret && dbid_req_.empty();
    return ret;
}

void PemBiu::attach(p_tm_mem_t out)
{
    out_ = out;
}

void PemBiu::attach(std::vector<p_pem_reg_t> v_reg)
{
    v_reg_ = v_reg;
}

bool PemBiu::pv_read(uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t thread_id){
    if(out_ == nullptr)
    {
        MODEL_LOG(MODEL_LOG_ERROR, "out is not connected.");
        return false;
    }
    //auto master_id = getSmmuMasterId(size, addr, v_reg_[thread_id]);
    //auto user_flag = getSmmuFlag(v_reg_[thread_id]);
    PEM_LOG_INFOI(rd_log_, "");
    bool ret = out_->pv_read(addr, size, ptr);
    pem_log::dump_buf(rd_log_, addr, size, ptr);
    return ret;
}

bool PemBiu::pv_write(uint64_t addr, uint32_t size, uint8_t* ptr, bool is_cosim, uint32_t thread_id){
    if(out_ == nullptr)
    {
        MODEL_LOG(MODEL_LOG_ERROR, "out is not connected.");
        return false;
    }
    PEM_LOG_INFOI(wr_log_, "");
    pem_log::dump_buf(wr_log_, addr, size, ptr);
    //auto master_id = getSmmuMasterId(size, addr, v_reg_[thread_id]);
    //auto user_flag = getSmmuFlag(v_reg_[thread_id]);
    return out_->pv_write(addr, size, ptr, false);
}

template<typename QUE, typename ARB>
bool PemBiu::is_inf_grp_vld(const std::vector<QUE>& inf_grp, uint32_t& idx, ARB arb)
{
    bool ret = false;
    idx = INV_ARB;
    for(uint32_t i = 0; i < inf_grp.size(); ++i)
    {
        if(inf_grp[i]->valid())
        {
            arb->req(i);
        }
    }
    idx = arb->get_arb();
    if (idx != INV_ARB) {
        ret = true;
    }
    return ret;
}

void PemBiu::recv_rd_cmd() {
    if(rd_otsd_ >= max_rd_otsd_) { return; }

    // IFU always have higher
    uint32_t idx = 0;
    if(is_inf_grp_vld(v_ifu_inf_, idx, ifu_rr_arb_)) {
        auto cmd = v_ifu_inf_[idx]->pop_pld();
        cmd->chan = CHAN_IFU + idx;
        (void)rd_cmds_->push_back(cmd);
    }
    else if(is_inf_grp_vld(v_dcache_rd_inf_, idx, dcache_rd_rr_arb_)) {
        auto cmd = v_dcache_rd_inf_[idx]->pop_pld();
        //cout << "[" << dec << time() << "] recv_rd_cmd: addr: " <<hex<< cmd->addr<< endl;
        cmd->chan = CHAN_DCACHE + idx;
        (void)rd_cmds_->push_back(cmd);
    }
    else if(mte_rd_inf_->valid(0)) {
        auto cmd = mte_rd_inf_->pop_pld(0);
        cmd->chan = CHAN_MTE;
        (void)rd_cmds_->push_back(cmd);
    }
}

void PemBiu::recv_wr_cmd() {
    if(wr_otsd_ >= max_wr_otsd_) { return; }

    uint32_t idx = 0;
    if(is_inf_grp_vld(v_dcache_wr_inf_, idx, dcache_wr_rr_arb_)) {
        auto cmd = v_dcache_wr_inf_[idx]->pop_pld();
        cmd->chan = CHAN_DCACHE + idx;
        (void)wr_cmds_->push_back(cmd);
    }     
    else if (mte_wr_inf_->valid()) {
        auto cmd = mte_wr_inf_->pop_pld();
        cmd->chan = CHAN_MTE;
        (void)wr_cmds_->push_back(cmd);
    }
}

void PemBiu::recv_wr_dat() {
    //auto dbid = dbid_req_.front();
    for (uint32_t i=0; i<v_dcache_wr_dat_que_.size(); ++i) {
        if (!v_dcache_wr_dat_que_[i]->valid()) continue;
        auto cmd = v_dcache_wr_dat_que_[i]->front();
        v_dcache_wr_dat_que_[i]->pop_front();
        cmd->chan = CHAN_DCACHE + i;
        (void)wr_data_->push_back(cmd);
        //dbid_req_.pop_front();
        // if (dbid == cmd->gid) {
        //     v_dcache_wr_dat_que_[i]->pop_front();
        //     cmd->chan = CHAN_DCACHE + i;
        //     (void)wr_data_->push_back(cmd);
        //     dbid_req_.pop_front();
        //     return;
        // }
    }
    if (mte_wr_dat_inf_->valid()) {
        auto cmd = mte_wr_dat_inf_->get_pld();
        mte_wr_dat_inf_->pop_pld();
        cmd->chan = CHAN_MTE;
        (void)wr_data_->push_back(cmd);
        // if (dbid == cmd->gid) {
        //     mte_wr_dat_inf_->pop_pld();
        //     cmd->chan = CHAN_MTE;
        //     (void)wr_data_->push_back(cmd);
        //     dbid_req_.pop_front();
        //     return;
        // }
    }
}

void PemBiu::send_rd_cmd() {
    if (rd_cmds_->empty()) { return; }

    auto cmd = rd_cmds_->front();
    if(out_intf_->send((uint32_t)aic_req_type_t::RD_REQ, cmd)) {
        pmu_->push("biu.read_bandwidth", time(), cmd->size);
        PEM_LOG_INFO(log_, "[{0:d}]: send_rd_cmd, chan: {1:d}, addr: 0x{2:x}, tnx_id: {3:d}", time(), cmd->chan, cmd->addr, cmd->gid);
        // set mst_id for pld
        cmd->mst_id = core_id_;
        (void)rd_cmds_->pop_front();
        ++rd_otsd_;
        pmu_->push("biu.read_outstanding", time(), rd_otsd_);
    }
}

void PemBiu::send_wr_cmd() {
    if (wr_cmds_->empty()) { return; }

    auto cmd = wr_cmds_->front();
    if(out_intf_->send((uint32_t)aic_req_type_t::WR_REQ, cmd)) {
        pmu_->push("biu.write_bandwidth", time(), cmd->size);
        PEM_LOG_INFO(log_, "[{0:d}]: send_wr_cmd, chan: {1:d}, addr: 0x{2:x}, tnx_id: {3:d}", time(), cmd->chan, cmd->addr, cmd->gid);
        // set mst_id for pld
        cmd->mst_id = core_id_;
        (void)wr_cmds_->pop_front();
        ++wr_otsd_;
        pmu_->push("biu.write_outstanding", time(), wr_otsd_);
    }
}

void PemBiu::send_wr_dat() {
    if (wr_data_->empty()) { return; }

    auto cmd = wr_data_->front();
    if(out_intf_->send((uint32_t)aic_req_type_t::WR_DAT, cmd)) {
        PEM_LOG_INFO(log_, "[{0:d}]: send_wr_dat, chan: {1:d}, addr: 0x{2:x}, tnx_id: {3:d}", time(), cmd->chan, cmd->addr, cmd->gid);
        // set mst_id for pld
        cmd->mst_id = core_id_;
        (void)wr_data_->pop_front();
    }
}

void PemBiu::recv_rd_cmd_rsp()
{
    for (uint32_t i=0; i < rd_port_num; ++i) {
        uint32_t port_id = (uint32_t)aic_req_type_t::RD_REQ+i;
        if (!out_intf_->valid(port_id)) continue;
        auto rsp = out_intf_->get_pld(port_id);
        bool success = false;
        if (rsp->mst_id != core_id_) {
            MODEL_LOG(MODEL_LOG_ERROR,"rsp->mst_id != core_id_.");
            return;
        }
        if((rsp->chan >= CHAN_IFU) && (rsp->chan < CHAN_IFU + ifu_port_num_)){
            if(v_ifu_inf_[rsp->chan - CHAN_IFU]->send(rsp)) {
                (void)out_intf_->pop_pld(port_id);
                success = true;
                --rd_otsd_;
            }
        }
        else if(rsp->chan < CHAN_DCACHE + dcache_port_num_){
            if(v_dcache_rd_inf_[rsp->chan - CHAN_DCACHE]->send(rsp)) {
                (void)out_intf_->pop_pld(port_id);
                success = true;
                --rd_otsd_;
            }
        }
        else if (rsp->chan == CHAN_MTE) {
            if (mte_rd_inf_->send(i, rsp)) {
                (void)out_intf_->pop_pld(port_id);
                success = true;
                if (rsp->tnx_id == 0) {
                    PEM_ASSERT(rd_otsd_ > 0);
                    --rd_otsd_;
                }
            }
        }
        else {
            PEM_ASSERT(false);
        }
        if (success) {
            pmu_->push("biu.read_outstanding", time(), rd_otsd_);
            PEM_LOG_INFO(log_, "[{0:d}]: recv_rd_cmd_rsp, chan: {1:d}, addr: 0x{2:x}, tnx_id: {3:d}", time(), rsp->chan, rsp->addr, rsp->gid);
        }
    }
}

void PemBiu::recv_wr_cmd_rsp()
{
    uint32_t port_id = (uint32_t)aic_req_type_t::WR_REQ;
    if (!out_intf_->valid(port_id)) return;
    auto rsp = out_intf_->get_pld(port_id);
    bool success = false;
    if (rsp->mst_id != core_id_) {
        MODEL_LOG(MODEL_LOG_ERROR,"rsp->mst_id != core_id_.");
        return;
    }
    if(rsp->chan < CHAN_DCACHE + dcache_port_num_){
        if (!v_dcache_wr_dat_que_[rsp->chan - CHAN_DCACHE]->full()) {
            v_dcache_wr_dat_que_[rsp->chan - CHAN_DCACHE]->push_back(rsp);
            (void)out_intf_->pop_pld(port_id);
            success = true;
        }
    }
    else if (rsp->chan == CHAN_MTE) {
        if (mte_wr_inf_->send(rsp)) {
            (void)out_intf_->pop_pld(port_id);
            success = true;
        }
    }
    else {
        PEM_ASSERT(false);
    }
    if (success) {
        dbid_req_.push_back(rsp->gid);
        PEM_LOG_INFO(log_, "[{0:d}]: recv_wr_cmd_rsp, chan: {1:d}, addr: 0x{2:x}, tnx_id: {3:d}", time(), rsp->chan, rsp->addr, rsp->gid);
    }
}

void PemBiu::recv_wr_dat_rsp()
{
    uint32_t port_id = (uint32_t)aic_req_type_t::WR_DAT;
    if (!out_intf_->valid(port_id)) return;
    auto rsp = out_intf_->get_pld(port_id);
    bool success = false;
    if (rsp->mst_id != core_id_) {
        MODEL_LOG(MODEL_LOG_ERROR,"rsp->mst_id != core_id_.");
        return;
    }
    if(rsp->chan < CHAN_DCACHE + dcache_port_num_){
        if(v_dcache_wr_inf_[rsp->chan - CHAN_DCACHE]->send(rsp)) {
            (void)out_intf_->pop_pld(port_id);
            PEM_ASSERT(wr_otsd_ > 0);
            --wr_otsd_;
            success = true;
        }
    }
    else if (rsp->chan == CHAN_MTE) {
        if (mte_wr_dat_inf_->send(rsp)) {
            (void)out_intf_->pop_pld(port_id);
            PEM_ASSERT(wr_otsd_ > 0);
            --wr_otsd_;
            success = true;
        }
    }
    else {
        PEM_ASSERT(false);
    }
    if (success) {
        pmu_->push("biu.write_outstanding", time(), wr_otsd_);
        PEM_LOG_INFO(log_, "[{0:d}]: recv_wr_dat_rsp, chan: {1:d}, addr: 0x{2:x}, tnx_id: {3:d}", time(), rsp->chan, rsp->addr, rsp->gid);
    }
}


//-------------------------------------------------------------------------------------------------
// Private


//-------------------------------------------------------------------------------------------------
// Funcs

//-------------------------------------------------------------------------------------------------
// End of file


