 #ifndef _PEM_DEMO_H_
#define _PEM_DEMO_H_

#include <functional>
#include <unordered_map>
#include <array>
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
 * demo.cc:
 * 示例流量发生器实现文件。
 * 它不属于 fabric 核心逻辑，但经常用于验证 PemBiu 和互连模型是否打通。
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
// DemoModule 类
// ----------------------------------------------
class DemoModule : public tm_engine::TmModule
{
public:
    DemoModule(const std::string &name, tm_engine::p_tm_clk_t clk);
    ~DemoModule();

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
    uint32_t max_pair_entries_ = 4;
    static constexpr uint32_t WRITE_BUF_POOL_SIZE = 64;
    uint8_t write_buf_pool_[WRITE_BUF_POOL_SIZE][4];
    std::queue<uint32_t> free_write_buf_ids_;

    bool handle_read_response(uint32_t req_id, uint64_t addr, uint32_t size);
    uint32_t allocate_write_buf();
    void release_write_buf(uint32_t id);
};

#endif #include "pem_trdemo.h"

//----------------------------------------------------------------------------------------
DemoModule::DemoModule(const std::string &name, tm_engine::p_tm_clk_t clk)
    : TmModule("demo"), clk_(clk)
{
    config();
}

DemoModule::~DemoModule()
{
}

//-----------------------------------------------------------------------------------------
void DemoModule::config()
{
    instr_que_ = tm_make_que<p_isa_t>(clk_, "instr_que", tm_engine::TM_MAX_UL);
    uop_que_ = tm_make_que<p_demo_uop_t>(clk_, "uop_que", UOP_GEN_LATCY + 1, UOP_GEN_LATCY);
    read_port_ = tm_make_inf<p_tm_pld_t>(clk_, "read_port");
    write_port_ = tm_make_inf<p_tm_pld_t>(clk_, "write_port");
    pipe_que_ = tm_make_que<p_demo_uop_t>(clk_, "pipe_que", CALC_LATCY + 1, CALC_LATCY);
    // pipe_que_->enable_rdy_event();
    tm_sensitive(TM_MAKE_CPROC(&DemoModule::gen_uop), instr_que_->vld);
    tm_sensitive(TM_MAKE_CPROC(&DemoModule::read_mem), uop_que_->vld);
    tm_sensitive(TM_MAKE_CPROC(&DemoModule::recv_rsp), read_port_->vld);
    tm_sensitive(TM_MAKE_CPROC(&DemoModule::pipeline), pipe_que_->vld);
    // tm_sensitive(TM_MAKE_CPROC(&DemoModule::process_ready_pairs), pipe_que_->rdy);
    tm_sensitive(TM_MAKE_CPROC(&DemoModule::wr_recv_rsp), write_port_->vld);
    // log
    std::string log_name = this->name() + ".log";
    log_para_t log_para = log_para_t(log_name);
    log_ = pem_log::create_logger(log_para);
    std::string rd_log_name = this->name() + "rd.log";
    log_para_t rd_log_para = log_para_t(rd_log_name);
    rd_log_ = pem_log::create_logger(rd_log_para);
    std::string wr_log_name = this->name() + "wr.log";
    log_para_t wr_log_para = log_para_t(wr_log_name);
    wr_log_ = pem_log::create_logger(wr_log_para);

    // 初始化配对缓冲区
    pair_buffer_.clear();
    for (uint32_t i = 0; i < WRITE_BUF_POOL_SIZE; ++i)
    {
        free_write_buf_ids_.push(i);
    }
}
void DemoModule::attach(p_pem_biu_t biu)
{
    biu_ = biu;
}

void DemoModule::build()
{
    read_port_->connect(biu_->v_dcache_rd_inf_[0]);
    write_port_->connect(biu_->v_dcache_wr_inf_[0]);
}

bool DemoModule::idle()
{
    return instr_que_->empty();
}

void DemoModule::reset()
{
}

void DemoModule::gen_uop()
{
    if (instr_que_->valid() && !instr_que_->empty())
    {
        auto instr = instr_que_->front();
        if (instr == nullptr)
        {
            std::cout << "ERROR: instr is nullptr!" << std::endl;
            instr_que_->pop_front();
            return;
        }

        while (current_uop_count_ < TOTAL_UOP_COUNT && !uop_que_->full() && instr != nullptr)
        {
            uint64_t src_addr = instr->start_addr_ + current_uop_count_ * BAND_WIDTH;
            int vld_time = time();
            auto uop = make_shared<demo_uop_t>(vld_time, current_uop_count_, src_addr, BAND_WIDTH);

            uop_que_->push_back(uop);
            current_uop_count_++;
        }
        if (current_uop_count_ == TOTAL_UOP_COUNT)
        {
            current_uop_count_ = 0;
            instr_que_->pop_front();
        }
    }
}
void DemoModule::read_mem()
{
    if (uop_que_->empty())
        return;
    auto uop = uop_que_->front();
    auto rd_pld = tm_make_pld(pld_cmd_t::RD, uop->addr, uop->size);
    if (read_port_->send(rd_pld))
    {
        uop_que_->pop_front();
        PEM_LOG_INFO(rd_log_, "M2,time:{2:d} ,  发送读请求,UOP[{0:d}], 地址=0x{1:x}",
                     uop->req_id, uop->addr, time());
    }
    else
    {
        PEM_LOG_INFO(rd_log_, "time:{0:d},read_port_->send失败", time());
    }
}
void DemoModule::recv_rsp()
{

    if (!read_port_->valid())
        return;
    auto rd_resp = read_port_->get_pld();
    if (rd_resp == nullptr)
        return;

    uint32_t req_id = (rd_resp->addr - START_ADDR) / BAND_WIDTH;
    bool success = handle_read_response(req_id, rd_resp->addr, rd_resp->size);
    if (success)
        read_port_->pop_pld();
    else
        std::cout << "WARNING: 读响应处理失败，req_id=" << req_id << std::endl;
}
bool DemoModule::handle_read_response(uint32_t req_id, uint64_t addr, uint32_t size)
{
    uint64_t wr_addr = END_ADDR + (req_id / 2) * sizeof(uint32_t);
    PEM_LOG_INFO(log_, "Pair[{0:x}] req_id:{1:d},pair_buffer_.size():{2:d}", wr_addr, req_id, pair_buffer_.size());
    PEM_LOG_INFO(rd_log_, "time:{0:d},Pair[{1:x}] 到达 (req_id={2:d})", time(), wr_addr, req_id);
    std::vector<uint8_t> ptr(size);
    biu_->pv_read(addr, size, ptr.data(), 0);
    auto it = pair_buffer_.find(wr_addr);

    if (it == pair_buffer_.end())
    {
        if (pair_buffer_.size() >= max_pair_entries_)
        {
            PEM_LOG_ERROR(rd_log_, "Pair Buffer 已满，无法处理 req_id={{0:d}}", req_id);
            return false;
        }
        PairEntry entry;
        entry.wr_addr = wr_addr;
        entry.size = size;
        entry.stored_data.resize(size);
        memcpy(entry.stored_data.data(), ptr.data(), size);
        entry.has_data = true;
        pair_buffer_[wr_addr] = entry;
        return true;
    }
    if (pipe_que_->full())
        return false;
    PairEntry &entry = it->second;
    if (!entry.has_data)
    {
        PEM_LOG_ERROR(rd_log_, "Pair[{0:x}] 状态异常：has_data=false", wr_addr);
        return false;
    }

    uint32_t count = size / sizeof(uint32_t);
    const uint32_t *p1 = reinterpret_cast<const uint32_t *>(entry.stored_data.data());
    const uint32_t *p2 = reinterpret_cast<const uint32_t *>(ptr.data());
    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        sum += p1[i] + p2[i];
    }
    auto result_uop = make_shared<demo_uop_t>(time(), 0, wr_addr, sizeof(uint32_t));
    result_uop->result = sum;
    if (!pipe_que_->full())
    {
        pipe_que_->push_back(result_uop);
        PEM_LOG_INFO(log_, "Pair[{0:x}] 累加完成, 结果={1:d}", wr_addr, sum);
    }
    else
    {
        PEM_LOG_ERROR(log_, "pipe_que_ 已满，Pair[{0:x}] 结果丢失!", wr_addr);
        return false;
    }
    pair_buffer_.erase(it);
    return true;
}

void DemoModule::pipeline()
{
    if (pipe_que_->empty())
        return;
    p_demo_uop_t uop = pipe_que_->front();
    uint32_t buf_id = allocate_write_buf();
    if (buf_id == UINT32_MAX)
    {
        // std::cout << "ERROR: buf_id is nullptr!" << std::endl;
        return;
    }
    PEM_LOG_INFO(log_, "time:{0:d},pipe_que_", clk_->time());

    uint32_t value = static_cast<uint32_t>(uop->result);
    uint8_t *write_buf = write_buf_pool_[buf_id];
    memcpy(write_buf, &value, sizeof(value));
    auto wr_pld = tm_make_pld(pld_cmd_t::WR, uop->addr, uop->size, write_buf);
    wr_pld->tnx_id = buf_id;
    if (write_port_->send(wr_pld))
    {
        pipe_que_->pop_front();
        PEM_LOG_INFO(wr_log_, "M4,time:{3:d},size : {0:d}, 地址=0x{1:x}, 数据={2:d}",
                     wr_pld->size, uop->addr, value, time());
    }
    else
    {
        release_write_buf(buf_id);
        std::cout << "write_port_->send失败" << std::endl;
    }
    //  rd_pld->print();
}

void DemoModule::wr_recv_rsp()
{
    PEM_LOG_INFO(wr_log_, "M4");
    if (!write_port_->valid())
    {
        return;
    }

    auto wr_resp = write_port_->pop_pld();
    if (wr_resp == nullptr)
    {
        std::cout << "ERROR: recv_rsp() - rd_resp is nullptr!" << std::endl;
        return;
    }
    uint32_t buf_id = wr_resp->tnx_id; // 取出之前存入的 ID
    release_write_buf(buf_id);
    PEM_LOG_INFO(wr_log_, "M4,time:{3:d},size : {0:d}, 地址=0x{1:x}, 数据={2:d}",
                 wr_resp->size, wr_resp->addr, wr_resp->data[0], time());
    biu_->pv_write(wr_resp->addr, wr_resp->size, wr_resp->data, 0, 0);
}

uint32_t DemoModule::allocate_write_buf()
{
    if (free_write_buf_ids_.empty())
        return UINT32_MAX;
    uint32_t id = free_write_buf_ids_.front();
    free_write_buf_ids_.pop();
    return id;
}
void DemoModule::release_write_buf(uint32_t id)
{
    free_write_buf_ids_.push(id);
}
//--------------------------------------------------------------------------------------------
// model 
