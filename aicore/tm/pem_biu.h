#ifndef _PEM_BIU_H_
#define _PEM_BIU_H_

#include "tm_engine.h"
#include "tm_que.h"
#include "cfg.h"
#include "pem_log.h"
#include "pem_reg.h"
#include "tm_mem.h"
#include "pem_biu_defines.h"
#include "arbiter.h"

//-------------------------------------------------------------------------------------------------
// Forward declarition
class PemBiu;
using pem_biu_t = PemBiu;
using p_pem_biu_t = std::shared_ptr<pem_biu_t>;

//-------------------------------------------------------------------------------------------------
// Types


//-------------------------------------------------------------------------------------------------
class PemBiu: public tm_engine::TmModule
{
public:
    PemBiu(const std::string& name, tm_engine::p_tm_clk_t clk, cfg::p_cfg_t cfg);
    ~PemBiu();

public:
    void config();
    void build();
    void reset();
    bool idle();
    void attach(p_tm_mem_t out);
    void attach(std::vector<p_pem_reg_t> reg);
    //void recv_ifu_cmd();
    //void recv_mte_rd_cmd();
    void send_data();
    void recv_rd_cmd();
    void recv_wr_cmd();
    void recv_wr_dat();

    void send_rd_cmd();
    void send_wr_cmd();
    void send_wr_dat();

    void recv_rd_cmd_rsp();
    void recv_wr_cmd_rsp();
    void recv_wr_dat_rsp();

    bool pv_read(uint64_t addr, uint32_t size, uint8_t* ptr, uint32_t thread_id=0);
    bool pv_write(uint64_t addr, uint32_t size, uint8_t* ptr, bool is_cosim, uint32_t thread_id=0);
private:
    template<typename QUE, typename ARB>
    bool is_inf_grp_vld(const std::vector<QUE>& inf_grp, uint32_t& idx, ARB arb);

public:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    cfg::p_cfg_t cfg_ = nullptr;
    p_logger_t               rd_log_ = nullptr;
    p_logger_t               wr_log_ = nullptr;
    p_logger_t               log_ = nullptr;

    bool last_req_rd_ = false;
    uint32_t core_id_ = 0;
    p_tm_com_que_t rd_cmds_ = nullptr;
    p_tm_com_que_t wr_cmds_ = nullptr;
    p_tm_com_que_t wr_data_ = nullptr;
    // to OUT, r/w
    p_tm_com_inf_t out_intf_ = nullptr;
    // from IFU
    std::vector<p_tm_com_inf_t> v_ifu_inf_;
    std::vector<p_tm_com_inf_t> v_dcache_rd_inf_;
    std::vector<p_tm_com_inf_t> v_dcache_wr_inf_;
    std::vector<p_tm_com_que_t> v_dcache_wr_dat_que_;

    p_rr_arb_t ifu_rr_arb_;
    p_rr_arb_t dcache_rd_rr_arb_;
    p_rr_arb_t dcache_wr_rr_arb_;
    // from MTE
    p_tm_com_inf_t mte_rd_inf_ = nullptr;
    p_tm_com_inf_t mte_wr_inf_ = nullptr;
    p_tm_com_inf_t mte_wr_dat_inf_ = nullptr;

    uint32_t ifu_port_num_ = 0;
    uint32_t dcache_port_num_ = 0;

    uint32_t max_rd_otsd_ = 0;
    uint32_t max_wr_otsd_ = 0;
    uint32_t rd_otsd_ = 0;
    uint32_t wr_otsd_ = 0;

    bool mte_rd_stall = false;
    bool mte_wr_stall = false;

    p_tm_mem_t out_ = nullptr;
    std::vector<p_pem_reg_t> v_reg_;

    //p_tm_event_t pos_edge = nullptr;
    //p_tm_event_t sample = nullptr;
    //p_tm_event_t update = nullptr;
    bool     enable_memmap_             = false;
    uint64_t memory_entry_num_          = 0;
    uint64_t memory_entry_size_         = 0;
    uint64_t physical_memory_entry_num_ = 0;
    uint64_t l2_buf_size_               = 0;
    uint32_t rd_port_num                = 1;
    p_pmu_t pmu_ = nullptr;
private:
    std::deque<uint32_t> dbid_req_;
    std::map<uint32_t, uint32_t> rd_data_cnt_;
};

//-------------------------------------------------------------------------------------------------
// Funcs



#endif  // _PEM_BIU_H_
//-------------------------------------------------------------------------------------------------
// End of file