#ifndef TM_PLD_H_
#define TM_PLD_H_

//#include "boost/any.hpp"
#include <memory>
#include <vector>
#include <iostream>

//-------------------------------------------------------------------------------------------------
// type definition
using pld_cmd_t = enum class PldCmd {
    RD,
    WR,
    WR_DAT,
    RSP,
    RD_RSP,
    WR_RSP,
    UNDEF
};//lint !e612
using pld_data_t = uint8_t*;
using pld_rsp_t = enum class PldRsp {OK, ERR, UNDEF};
//using any_t = boost::any;
using any_t = uint32_t; // place holder

//-------------------------------------------------------------------------------------------------
class TmPld
{
public:
    TmPld();
    TmPld(any_t content);
    TmPld(uint32_t type_id, any_t content);
    TmPld(pld_cmd_t cmd, uint64_t addr, uint32_t size);
    TmPld(pld_cmd_t cmd, uint64_t addr, uint32_t size, pld_data_t data);
    TmPld(pld_cmd_t cmd, uint64_t mst_addr, uint64_t slv_addr, uint32_t size);
    TmPld(std::shared_ptr<TmPld> pld);
    ~TmPld();

public:
    void set_ts(uint64_t ts);
    uint64_t get_ts();
    uint32_t reg_type_id();
    bool is_type(uint32_t type_id);
    // debug funcs
    void print();
    void print(std::string prefix);

public:
    // primary fields
    uint64_t gid = 0;
    uint32_t type_id = 0;
    any_t content = 0;
    // optinal fields
    pld_cmd_t cmd = pld_cmd_t::UNDEF;
    uint64_t addr = 0;
    uint32_t mst_id = 0;
    uint32_t slv_id = 0;
    uint64_t mst_addr = 0;
    uint64_t slv_addr = 0;
    uint32_t size = 0;
    pld_rsp_t rsp = pld_rsp_t::UNDEF;
    std::shared_ptr<std::vector<uint8_t>> buf_u8 = nullptr;
    std::shared_ptr<std::vector<uint32_t>> buf_u32 = nullptr;
    std::shared_ptr<std::vector<uint64_t>> buf_u64 = nullptr;
    pld_data_t data = nullptr;  //lint !e524
    uint32_t chan = 0;      // alias for port/VC
    uint32_t latency = 0;
    uint32_t rsp_count = 1;
    uint64_t ts= 0;         // time stamp
    uint32_t tnx_id = 0;
    uint32_t tag_id = 0;
    uint32_t smmu_tnx_id = 0;
    uint32_t ring_subnet = 0;
    uint32_t ring_traffic_class = 0;
    uint32_t ring_rsp_lane = 0;
private:
    static uint64_t cur_gid;
    static uint32_t cur_type_id;
 
};

// init global static variable, run it before simulation
#define INIT_TM_PLD_GID     uint64_t TmPld::cur_gid     = 0;
#define INIT_TM_PLD_TYPEID  uint32_t TmPld::cur_type_id = 0;

using tm_pld_t   = TmPld;
using p_tm_pld_t = std::shared_ptr<TmPld>;

//-------------------------------------------------------------------------------------------------
// API
inline p_tm_pld_t tm_make_pld() {
    return std::make_shared<TmPld>();
}
inline p_tm_pld_t tm_make_pld(p_tm_pld_t pld) {
    return std::make_shared<TmPld>(pld);
}

inline p_tm_pld_t tm_make_pld(any_t content) {
    return std::make_shared<TmPld>(content);
}

inline p_tm_pld_t tm_make_pld(uint32_t type_id, any_t content) {
    return std::make_shared<TmPld>(type_id, content);
}

inline p_tm_pld_t tm_make_pld(pld_cmd_t cmd, uint64_t addr, uint32_t size) {
    return std::make_shared<TmPld>(cmd, addr, size);
}

inline p_tm_pld_t tm_make_pld(pld_cmd_t cmd, uint64_t addr, uint32_t size, pld_data_t data) {
    return std::make_shared<TmPld>(cmd, addr, size, data);
}

inline uint64_t tm_pld_txn_key(p_tm_pld_t pld) {
    return (static_cast<uint64_t>(pld->mst_id) << 32) | pld->gid;
}

inline void tm_pld_set_ring_route(p_tm_pld_t pld, uint32_t req_type,
                                  uint32_t target_id, uint32_t src_node,
                                  uint32_t dst_node) {
    pld->type_id = req_type;
    pld->slv_id = target_id;
    pld->mst_addr = src_node;
    pld->slv_addr = dst_node;
    pld->ts = time();
}

inline uint32_t tm_pld_req_type(p_tm_pld_t pld) {
    return pld->type_id;
}

inline uint32_t tm_pld_target_id(p_tm_pld_t pld) {
    return pld->slv_id;
}

inline uint32_t tm_pld_src_node(p_tm_pld_t pld) {
    return static_cast<uint32_t>(pld->mst_addr);
}

inline uint32_t tm_pld_dst_node(p_tm_pld_t pld) {
    return static_cast<uint32_t>(pld->slv_addr);
}

inline uint32_t tm_pld_rsp_count(p_tm_pld_t pld) {
    if (pld == nullptr || pld->rsp_count == 0) {
        return 1;
    }
    return pld->rsp_count;
}



#endif 
