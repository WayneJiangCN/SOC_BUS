#ifndef _PEM_TRDEMO_H_
#define _PEM_TRDEMO_H_

#include <array>
#include <cstring>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>
#include "tm_engine.h"
#include "tm_que.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_pld.h"
#include "fp.h"
#include "isa.h"
#include "pem_log.h"
#include "pem_biu.h"

using namespace std;
using namespace tm_engine;

/*
 * pem_trdemo.h:
 * 一个轻量示例模块，用来驱动 PemBiu 产生读写事务，
 * 方便联调 bus/ring/mem 这一整条路径。
 */

const static uint BAND_WIDTH = 128;

// ----------------------------------------------
// UOP 结构体（精简版）
// ----------------------------------------------
using demo_uop_t = struct DemoUop
{
    uint32_t vld_time = 0;
    uint32_t req_id = 0;
    uint64_t addr = 0;
    uint32_t size = 0;
    uint64_t result = 0;

    DemoUop() {}
    DemoUop(uint32_t vt, uint32_t rid, uint64_t a, uint32_t s)
        : vld_time(vt), req_id(rid), addr(a), size(s), result(0) {}
};

using p_demo_uop_t = std::shared_ptr<demo_uop_t>;

// 流水线延迟参数
const static int UOP_GEN_LATCY = 2;
const static int CALC_LATCY = 6;

const uint32_t TOTAL_UOP_COUNT = 1600;
const uint32_t START_ADDR = 0x30000000;
const uint32_t END_ADDR = 0x40000000;

struct PairEntry
{
    uint64_t wr_addr = 0;
    uint32_t size = 0;
    std::vector<uint8_t> stored_data;
    bool has_data = false;

    void reset()
    {
        wr_addr = 0;
        size = 0;
        stored_data.clear();
        has_data = false;
    }
};

// ----------------------------------------------
// PemTrDemo 类
// ----------------------------------------------
class PemTrDemo : public tm_engine::TmModule
{
public:
    PemTrDemo(const std::string &name, tm_engine::p_tm_clk_t clk);
    ~PemTrDemo();

public:
    virtual void config() override;
    virtual void attach(p_pem_biu_t biu);
    virtual void build() override;
    virtual void reset() override;
    virtual bool idle() override;

public:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mem_t mem_ = nullptr;
    p_pem_biu_t biu_ = nullptr;

    p_tm_com_inf_t read_port_ = nullptr;
    p_tm_com_inf_t write_port_ = nullptr;

    p_tm_que_t<p_isa_t> instr_que_ = nullptr;

    p_tm_que_t<p_demo_uop_t> uop_que_ = nullptr;
    p_tm_que_t<p_demo_uop_t> pipe_que_ = nullptr;

    p_logger_t rd_log_ = nullptr;
    p_logger_t wr_log_ = nullptr;
    p_logger_t log_ = nullptr;

public:
    void gen_uop();
    void read_mem();
    void recv_rsp();
    void calc_pair(uint64_t wr_addr);
    void pipeline();
    void wr_recv_rsp();

private:
    uint32_t current_uop_count_ = 0;

    std::unordered_map<uint64_t, PairEntry> pair_buffer_;
    uint32_t max_pair_entries_ = TOTAL_UOP_COUNT;
    static constexpr uint32_t WRITE_BUF_POOL_SIZE = 64;
    uint8_t write_buf_pool_[WRITE_BUF_POOL_SIZE][4];
    std::queue<uint32_t> free_write_buf_ids_;

    bool handle_read_response(p_tm_pld_t rd_resp);
    uint32_t allocate_write_buf();
    void release_write_buf(uint32_t id);
};
