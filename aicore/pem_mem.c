#include "tm_mem.h"

using namespace std;
using namespace tm_engine;


//-------------------------------------------------------------------------------------------------
TmMem::TmMem()
{
}

TmMem::TmMem(p_tm_clk_t clk, p_tm_mem_cfg_t cfg) 
: clk_(clk)
, cfg_(cfg)
{
    this->name(cfg->name);
    rw_inf_ = tm_make_com_inf(clk_, cfg_->rw_inf_buf_size, TM_INF_DEF_DELAY, 0, this->name()+"_rw_inf");
    rw_inf_->set_chan_num(4);
    for (uint32_t i=0; i<(uint32_t)aic_req_type_t::NUM; ++i) {
        acc_rdy.push_back(tm_make_event("acc_rdy"+std::to_string(i)));
        rsp_que_.push_back(tm_make_com_que(clk_));
    }
    acc_tab_.resize((uint32_t)aic_req_type_t::NUM);
#ifdef TM_MEM_PV_EN
    mem_ = make_shared<pv_mem_t>(cfg_->reset_val);
#endif
    crdt_clk_ = tm_make_clk("crdt_clk", cfg_->ddr->crdt_update_period);
    // set sensitive
    tm_sensitive(TM_MAKE_CPROC(&TmMem::recv), rw_inf_->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmMem::get_dbid), rw_inf_->vld);

    tm_sensitive(TM_MAKE_CPROC(&TmMem::prepare_rd_req_rsp), acc_rdy[(uint32_t)aic_req_type_t::RD_REQ]);
    tm_sensitive(TM_MAKE_CPROC(&TmMem::prepare_wr_req_rsp), acc_rdy[(uint32_t)aic_req_type_t::WR_REQ]);
    tm_sensitive(TM_MAKE_CPROC(&TmMem::prepare_wr_dat_rsp), acc_rdy[(uint32_t)aic_req_type_t::WR_DAT]);

    tm_sensitive(TM_MAKE_CPROC(&TmMem::rd_req_rsp), rsp_que_[(uint32_t)aic_req_type_t::RD_REQ]->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmMem::wr_req_rsp), rsp_que_[(uint32_t)aic_req_type_t::WR_REQ]->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmMem::wr_dat_rsp), rsp_que_[(uint32_t)aic_req_type_t::WR_DAT]->vld);

    tm_sensitive(TM_MAKE_CPROC(&TmMem::update_crdt), crdt_clk_->pos_edge);
    reset();
}

TmMem::~TmMem() 
{
}

void TmMem::reset() {
    
#ifdef TM_MEM_PV_EN
    mem_->reset(cfg_->reset_val);
#endif
    rw_inf_->reset();
    for (uint32_t i=0; i<(uint32_t)aic_req_type_t::NUM; ++i) {
        acc_tab_[(uint32_t)i].clear();
        rsp_que_[(uint32_t)i]->clear();
    }
    ddr_acc_crdt_ = cfg_->ddr->max_acc_crdt;

    l2_acc_crdt_  = cfg_->l2->max_acc_crdt;
    l2_rd_crdt_   = cfg_->l2->max_rd_crdt;
    l2_wr_crdt_   = cfg_->l2->max_wr_crdt;
    traffic_ = 0;
    bw_ = 0;
}

bool TmMem::idle() {
    bool ret = true;
    ret &= rw_inf_->idle();
    for (uint32_t i=0; i<(uint32_t)aic_req_type_t::NUM; ++i) {
        ret &= acc_tab_[(uint32_t)i].empty();
        ret &= rsp_que_[(uint32_t)i]->empty();
    }
    return ret;
}

void TmMem::recv() {
    while(check_input(aic_req_type_t::RD_REQ)) {}
    while(check_input(aic_req_type_t::WR_DAT)) {}
}

void TmMem::get_dbid() {
    uint32_t req_type = (uint32_t)aic_req_type_t::WR_REQ;
    while(rw_inf_->valid(req_type)) {
        auto pld = rw_inf_->get_pld(req_type);
        pld->rsp = pld_rsp_t::UNDEF;
        auto addr = pld->addr;
        bool is_l2_access = (addr >= cfg_->l2->addr_begin) && (addr < (cfg_->l2->addr_begin + cfg_->l2->size));
        auto mem_cfg = is_l2_access ? cfg_->l2 : cfg_->ddr;
        auto min_latency = mem_cfg->min_dbid_lat;
        auto latency_var = mem_cfg->dbid_lat_var;;
        decltype(min_latency) latency;
        if(cfg_->inorder_acc) { 
            latency = min_latency + latency_var/2; 
        }
        else { 
            latency = min_latency + rand()%latency_var; 
        }
        rw_inf_->pop_pld(req_type);
        acc_rdy[(uint32_t)req_type]->notify_after(latency);
        auto ts = time() + latency;
        if(acc_tab_[(uint32_t)req_type].find(ts) == acc_tab_[(uint32_t)req_type].end()) { acc_tab_[(uint32_t)req_type][ts] = deque<p_tm_pld_t>(); }
        acc_tab_[(uint32_t)req_type][ts].push_back(pld);
    }
}

void TmMem::prepare_rsp(aic_req_type_t req_type) {
    if(acc_tab_[(uint32_t)req_type].empty()) { 
        return; 
    }
    auto now = time();
    auto iter = acc_tab_[(uint32_t)req_type].find(now);
    if(iter != acc_tab_[(uint32_t)req_type].end()) {
        for(auto& pld: (*iter).second) {
            pld->rsp = pld_rsp_t::OK;
            rsp_que_[(uint32_t)req_type]->push_back(pld);
            if (acc_num_ > 0 && req_type != aic_req_type_t::WR_REQ){
                acc_num_--;
            }
        }
        acc_tab_[(uint32_t)req_type].erase(iter);
    }
}

void TmMem::prepare_rd_req_rsp()
{
    prepare_rsp(aic_req_type_t::RD_REQ);
}

void TmMem::prepare_wr_req_rsp()
{
    prepare_rsp(aic_req_type_t::WR_REQ);
}

void TmMem::prepare_wr_dat_rsp()
{
    prepare_rsp(aic_req_type_t::WR_DAT);
}

void TmMem::rsp(aic_req_type_t req_type) {
    while(!rsp_que_[(uint32_t)req_type]->empty()) {
        if(rw_inf_->send((uint32_t)req_type, rsp_que_[(uint32_t)req_type]->front())) {
            rsp_que_[(uint32_t)req_type]->pop_front();
        }
        else {
            break;
        }
    }
}

void TmMem::rd_req_rsp()
{
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    uint32_t tnx = 0;
    while(!rsp_que_[(uint32_t)req_type]->empty()) {
        auto pld = rsp_que_[(uint32_t)req_type]->front();
        uint32_t port_id = 0;
        if (pld->chan == 10) {
            if(cfg_->inorder_acc) {
                port_id = tnx % 2;
            }
            else {
                port_id = rand() % 2;
            }
        }
        if(rw_inf_->send((uint32_t)req_type+port_id, pld)) {
            rsp_que_[(uint32_t)req_type]->pop_front();
            ++tnx;
        }
        else {
            break;
        }
    }
}

void TmMem::wr_req_rsp()
{
    rsp(aic_req_type_t::WR_REQ);
}

void TmMem::wr_dat_rsp()
{
    rsp(aic_req_type_t::WR_DAT);
}

void TmMem::update_crdt() {
    ddr_acc_crdt_ += cfg_->ddr->acc_crdt_update;

    l2_acc_crdt_  += cfg_->l2->acc_crdt_update;
    l2_rd_crdt_   += cfg_->l2->rd_crdt_update;
    l2_wr_crdt_   += cfg_->l2->wr_crdt_update;

    if(ddr_acc_crdt_ > cfg_->ddr->max_acc_crdt) { 
        ddr_acc_crdt_ = cfg_->ddr->max_acc_crdt; 
    }
    if(l2_acc_crdt_ > cfg_->l2->max_acc_crdt) { 
        l2_acc_crdt_ = cfg_->l2->max_acc_crdt; 
    }
    if(l2_rd_crdt_ > cfg_->l2->max_rd_crdt) { 
        l2_rd_crdt_ = cfg_->l2->max_rd_crdt; 
    }
    if(l2_wr_crdt_ > cfg_->l2->max_wr_crdt) { 
        l2_wr_crdt_ = cfg_->l2->max_wr_crdt; 
    }
}

bool TmMem::check_input(aic_req_type_t req_type) {
    if(!rw_inf_->valid((uint32_t)req_type) || acc_num_ >=cfg_->acc_que_size ) {
        return false;
    }
    p_tm_pld_t pld = rw_inf_->get_pld((uint32_t)req_type);
    auto size = pld->size;
    auto addr = pld->addr;
    TM_ASSERT(req_type == aic_req_type_t::RD_REQ || req_type == aic_req_type_t::WR_DAT);
    bool is_l2_access = (addr >= cfg_->l2->addr_begin) && (addr < (cfg_->l2->addr_begin + cfg_->l2->size));

    uint32_t mem_acc_crdt = is_l2_access ? l2_acc_crdt_ : ddr_acc_crdt_;
    uint32_t rw_crdt = (req_type == aic_req_type_t::WR_DAT)? 
                       (is_l2_access ? l2_wr_crdt_ : ddr_acc_crdt_) :
                       (is_l2_access ? l2_rd_crdt_ : ddr_acc_crdt_);
    auto mem_cfg = is_l2_access ? cfg_->l2 : cfg_->ddr;
    auto min_latency = (req_type == aic_req_type_t::WR_DAT)? mem_cfg->min_wr_lat : mem_cfg->min_rd_lat;
    auto latency_var = (req_type == aic_req_type_t::WR_DAT)? mem_cfg->wr_lat_var : mem_cfg->rd_lat_var;
    decltype(min_latency) latency;
    if(size < mem_acc_crdt && size < rw_crdt) {
        rw_inf_->pop_pld((uint32_t)req_type);
        // calc latency
        uint32_t rsp_num = (pld->chan == 10 && req_type == aic_req_type_t::RD_REQ) ? ceil((float)pld->size / 128) : 1;
        pld->rsp_count = rsp_num;
        for (uint32_t i=0; i<rsp_num; ++i) {
            if(cfg_->inorder_acc) { 
                latency = min_latency + latency_var/2; 
            }
            else { 
                latency = min_latency + rand()%latency_var; 
            }
            acc_rdy[(uint32_t)req_type]->notify_after(latency);
            auto ts = time() + latency;
            if(acc_tab_[(uint32_t)req_type].find(ts) == acc_tab_[(uint32_t)req_type].end()) { acc_tab_[(uint32_t)req_type][ts] = deque<p_tm_pld_t>(); }
            if (pld->chan == 10 && req_type == aic_req_type_t::RD_REQ) {
                auto pld_mte = tm_make_pld(pld);
                pld_mte->tnx_id = i;
                acc_tab_[(uint32_t)req_type][ts].push_back(pld_mte);
            }
            else {
                acc_tab_[(uint32_t)req_type][ts].push_back(pld);
            }
            acc_num_++;
        }
        // calc traffic
        if(cfg_->bw_stat) { traffic_ += size; }
        // consume credit and do r/w

        if(is_l2_access){
            l2_acc_crdt_ -= size;
        }
        else{
            ddr_acc_crdt_ -= size;
        }

        if(req_type == aic_req_type_t::WR_DAT) {
            if(is_l2_access){
                l2_wr_crdt_ -= size;
            }
            else{
                ddr_acc_crdt_ -= size;
            } 
#ifdef TM_MEM_PV_EN
            TM_ASSERT(pld->data != nullptr);
            TM_ASSERT(mem_->write(pld->addr, size, pld->data));
#endif
        }
        else { 
            if(is_l2_access){
                l2_rd_crdt_ -= size;
            }
            else{
                ddr_acc_crdt_ -= size;
            } 
#ifdef TM_MEM_PV_EN
            mem_->read(pld->addr, size, pld->data);
#endif
        }
        return true;
    }
    // no credit
    else {
        return false;
    }
}

uint32_t TmMem::load_bin(uint64_t addr, string file_name) {
    return mem_->load_bin(addr, file_name);
}

bool TmMem::dump_bin(uint64_t addr, uint32_t size, string file_name) {
    return mem_->dump_bin(addr, size, file_name);
}

bool TmMem::direct_read(uint64_t addr, uint32_t size, uint8_t* ptr) {
    if(size==0 || ptr==nullptr) {
        return true;
    }
    else {
        return mem_->read(addr, size, ptr);
    }
}

bool TmMem::direct_write(uint64_t addr, uint32_t size, uint8_t* ptr, bool is_cosim) {
    if(size==0 || ptr==nullptr) {
        return true;
    }
    else {
        return mem_->write(addr, size, ptr);
    }
}

bool TmMem::direct_read(uint32_t port, uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag)
{
    return direct_read(addr, size, ptr);
}

bool TmMem::direct_write(uint32_t port, uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag)
{
    return direct_write(addr, size, ptr, false);
}

bool TmMem::pv_read(uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag)
{
    return direct_read(0, addr, size, ptr, smmu_master_id, smmu_flag);
}

bool TmMem::pv_write(uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag)
{
    return direct_write(0, addr, size, ptr, smmu_master_id, smmu_flag);
}

bool TmMem::pv_read(uint64_t addr, uint32_t size, uint8_t* ptr){
    return direct_read(addr, size, ptr);
}

bool TmMem::pv_write(uint64_t addr, uint32_t size, uint8_t* ptr, bool is_cosim){
    return direct_write(addr, size, ptr, is_cosim);
}

bool TmMem::addr_in_range(uint64_t addr, uint64_t begin, uint64_t end) {
    return (addr >= begin && addr <=end);
}
//---------------------------------------------------------------------------------------
// End





这是代码
