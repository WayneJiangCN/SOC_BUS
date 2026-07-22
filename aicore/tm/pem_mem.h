#ifndef _TM_MEM_H_
#define _TM_MEM_H_

#include <unordered_map>
#include "tm_engine.h"
#include "tm_clock.h"
#include "tm_inf.h"
#include "tm_que.h"
#include "pv_mem.h"
#include "cfg.h"
#define TM_MEM_PV_EN
//#define TM_MEM_DBG
enum class aic_req_type_t
{
    WR_REQ, WR_DAT, RD_REQ, NUM
};

//-------------------------------------------------------------------------------------------------
// Types
struct TmMemTrafficCfg{
    uint32_t addr_begin         = 0;
    uint32_t size               = 0x4000000;
    uint32_t max_acc_crdt       = 4096;
    uint32_t max_rd_crdt        = 4096;
    uint32_t max_wr_crdt        = 4096;
    uint32_t acc_bw_limit       = 4096;//33;
    uint32_t rd_bw_limit        = 4096;//33;
    uint32_t wr_bw_limit        = 4096;//33;
    uint32_t crdt_update_period = 1;//10;               // in term of ticks, normally should less than DDR latency
    uint32_t min_rd_lat         = 150;//226;
    uint32_t rd_lat_var         = 1;//200;
    uint32_t min_wr_lat         = 150;//120;
    uint32_t wr_lat_var         = 1;//200;
    uint32_t min_dbid_lat       = 150;//120;
    uint32_t dbid_lat_var       = 1;//200;
    uint32_t acc_crdt_update    = acc_bw_limit*crdt_update_period;
    uint32_t rd_crdt_update     = rd_bw_limit*crdt_update_period;
    uint32_t wr_crdt_update     = wr_bw_limit*crdt_update_period;
};

using tm_mem_traf_cfg_t = TmMemTrafficCfg;
using p_tm_mem_traf_cfg_t = std::shared_ptr<tm_mem_traf_cfg_t>;

struct TmMemCfg{
    // general
    std::string name = "";
    bool     inorder_acc        = true;
    bool     bw_stat            = false;
    uint32_t bw_stat_period     = 1000;             // in term of ticks
    uint8_t  reset_val          = 0x0;
    uint32_t rw_inf_buf_size    = 0x10;              // size of DDR TM_INF buf TGT port
    uint32_t acc_que_size       = 4096;//128;

    p_tm_mem_traf_cfg_t ddr     = std::make_shared<tm_mem_traf_cfg_t>();
    p_tm_mem_traf_cfg_t l2      = std::make_shared<tm_mem_traf_cfg_t>();

    // cons
    TmMemCfg() {}
    TmMemCfg(std::string name, cfg::p_cfg_t cfg=nullptr) {
        this->name = name;
        if (cfg == nullptr) return;
        this->inorder_acc = (uint32_t)cfg->get_cfg<int>("ARCH.inorder_acc") == 1;
        ddr->addr_begin = (uint32_t)cfg->get_cfg<int>("DDR.addr_begin");
        ddr->max_acc_crdt = (uint32_t)cfg->get_cfg<int>("DDR.max_credit_num");
        ddr->acc_bw_limit = (uint32_t)cfg->get_cfg<int>("DDR.bandwidth_limit");
        ddr->min_rd_lat = (uint32_t)cfg->get_cfg<int>("DDR.min_read_latency");
        ddr->rd_lat_var = (uint32_t)cfg->get_cfg<int>("DDR.read_latency_diver");
        ddr->min_wr_lat = (uint32_t)cfg->get_cfg<int>("DDR.min_write_latency");
        ddr->wr_lat_var = (uint32_t)cfg->get_cfg<int>("DDR.write_latency_diver");
        ddr->min_dbid_lat = (uint32_t)cfg->get_cfg<int>("DDR.min_dbid_latency");
        ddr->dbid_lat_var = (uint32_t)cfg->get_cfg<int>("DDR.dbid_latency_diver");
        ddr->acc_crdt_update = ddr->acc_bw_limit;

        l2->addr_begin = (uint32_t)cfg->get_cfg<int>("L2.addr_begin");
        l2->size = (uint32_t)cfg->get_cfg<int>("L2.size");
        l2->max_acc_crdt = (uint32_t)cfg->get_cfg<int>("L2.max_credit_num");
        l2->acc_bw_limit = (uint32_t)cfg->get_cfg<int>("L2.bandwidth_limit");
        l2->rd_bw_limit = (uint32_t)cfg->get_cfg<int>("L2.read_bandwidth_limit");
        l2->wr_bw_limit = (uint32_t)cfg->get_cfg<int>("L2.write_bandwidth_limit");
        l2->min_rd_lat = (uint32_t)cfg->get_cfg<int>("L2.min_read_latency");
        l2->rd_lat_var = (uint32_t)cfg->get_cfg<int>("L2.read_latency_diver");
        l2->min_wr_lat = (uint32_t)cfg->get_cfg<int>("L2.min_write_latency");
        l2->wr_lat_var = (uint32_t)cfg->get_cfg<int>("L2.write_latency_diver");
        l2->min_dbid_lat = (uint32_t)cfg->get_cfg<int>("L2.min_dbid_latency");
        l2->dbid_lat_var = (uint32_t)cfg->get_cfg<int>("L2.dbid_latency_diver");
        l2->acc_crdt_update = l2->acc_bw_limit;
        l2->rd_crdt_update = l2->rd_bw_limit;
        l2->wr_crdt_update = l2->wr_bw_limit;
    }
    void print_params() {
        std::cout << std::dec << std::noshowbase;
        std::cout << "[TmMem]: name is: \t\t" << name << std::endl;
        std::cout << "[TmMem]: inorder_acc is: \t" << inorder_acc << std::endl;
        std::cout << "[TmMem]: bw_stat is: \t\t" << bw_stat << std::endl;
        std::cout << "[TmMem]: bw_stat_period is: \t" << bw_stat_period << std::endl;
        std::cout << "[TmMem]: reset_val is: \t" << (uint32_t)reset_val << std::endl;
        std::cout << "[TmMem]: rw_inf_buf_size is: \t" << rw_inf_buf_size << std::endl;
        std::cout << "[TmMem]: acc_que_size is: \t" << acc_que_size << std::endl;
        std::cout << std::endl;
    }
};
using tm_mem_cfg_t = TmMemCfg;
using p_tm_mem_cfg_t = std::shared_ptr<tm_mem_cfg_t>;

using tm_mem_acc_tab_t = std::unordered_map<tm_engine::tm_time_t, std::deque<p_tm_pld_t>>;

//-------------------------------------------------------------------------------------------------
class TmMem : public tm_engine::TmModule {
    public:
        TmMem();
        TmMem(tm_engine::p_tm_clk_t clk, p_tm_mem_cfg_t cfg);
        virtual ~TmMem();

    public: // user APIs
        virtual void reset();
        virtual bool idle();
        virtual void recv();
        virtual void get_dbid();
        virtual void prepare_rd_req_rsp();
        virtual void prepare_wr_req_rsp();
        virtual void prepare_wr_dat_rsp();
        virtual void rd_req_rsp();
        virtual void wr_req_rsp();
        virtual void wr_dat_rsp();

        virtual void prepare_rsp(aic_req_type_t);
        virtual void rsp(aic_req_type_t);
        virtual void update_crdt();
    public: // Aux funcs
        virtual bool check_input(aic_req_type_t req_type);
        // virtual bool send_rsp();
        virtual uint32_t load_bin(uint64_t addr, std::string file_name);
        virtual bool dump_bin(uint64_t addr, uint32_t size, std::string file_name);
        virtual bool direct_read(uint64_t addr, uint32_t size, uint8_t* ptr);
        virtual bool direct_write(uint64_t addr, uint32_t size, uint8_t* ptr, bool is_cosim);
        virtual bool pv_read(uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag);
        virtual bool pv_write(uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag);
        virtual bool direct_read(uint32_t port, uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag);
        virtual bool direct_write(uint32_t port, uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t smmu_master_id, uint32_t smmu_flag);
        virtual bool pv_read(uint64_t addr, uint32_t size, uint8_t* ptr);
        virtual bool pv_write(uint64_t addr, uint32_t size, uint8_t* ptr, bool is_cosim);
        virtual bool addr_in_range(uint64_t addr, uint64_t begin, uint64_t end);

    public:
        tm_engine::p_tm_clk_t clk_ = nullptr;
        p_tm_mem_cfg_t cfg_ = nullptr;
        p_tm_com_inf_t rw_inf_;
        p_pv_mem_t mem_ = nullptr;
        tm_engine::p_tm_clk_t crdt_clk_ = nullptr;
    
    protected:
        std::vector<tm_engine::p_tm_event_t> acc_rdy;
        std::vector<tm_mem_acc_tab_t> acc_tab_;
        uint32_t acc_num_ = 0;
        std::vector<p_tm_com_que_t> rsp_que_;
        uint32_t ddr_acc_crdt_ = 0;
        uint32_t l2_acc_crdt_ = 0;
        uint32_t l2_rd_crdt_ = 0;
        uint32_t l2_wr_crdt_ = 0;
        uint32_t traffic_ = 0;
        uint32_t bw_ = 0;
};
using tm_mem_t = TmMem;
using p_tm_mem_t = std::shared_ptr<TmMem>;

//-------------------------------------------------------------------------------------------------
// API
inline p_tm_mem_cfg_t tm_make_mem_cfg() {
    return std::make_shared<TmMemCfg>();
}

inline p_tm_mem_cfg_t tm_make_mem_cfg(std::string name) {
    return std::make_shared<TmMemCfg>(name);
}

inline p_tm_mem_cfg_t tm_make_mem_cfg(std::string name, cfg::p_cfg_t cfg) {
    return std::make_shared<TmMemCfg>(name, cfg);
}

inline p_tm_mem_t tm_make_mem(tm_engine::p_tm_clk_t clk, p_tm_mem_cfg_t cfg) {
    return std::make_shared<TmMem>(clk, cfg);
}


#endif  // _TM_MEM_H_

//-------------------------------------------------------------------------------------------------
// End