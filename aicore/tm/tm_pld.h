#ifndef TM_PLD_H_
#define TM_PLD_H_

//#include "boost/any.hpp"
#include <memory>
#include <vector>
#include <iostream>

//-------------------------------------------------------------------------------------------------
// 基础类型定义。
using pld_cmd_t = enum class PldCmd {
    // RD/WR 是命令阶段，WR_DAT 是获得 grant 后的写数据阶段。
    RD,
    WR,
    WR_DAT,
    // RSP 表示最终写数据响应，RD_RSP 表示读数据响应，WR_RSP 表示写 grant。
    RSP,
    RD_RSP,
    WR_RSP,
    UNDEF
};//lint !e612
using pld_data_t = uint8_t*;
using pld_rsp_t = enum class PldRsp {OK, ERR, UNDEF};
//using any_t = boost::any;
using any_t = uint32_t; // 兼容旧接口的占位类型。

//-------------------------------------------------------------------------------------------------
class TmPld
{
public:
    // 各构造函数都会分配或继承 gid；拷贝构造保留原 gid 以维持事务关联。
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
    // 调试输出接口。
    void print();
    void print(std::string prefix);

public:
    // 核心字段。
    // gid 在一个 Master 内标识事务；跨 Master 唯一键应组合 mst_id 和 gid。
    uint64_t gid = 0;
    // type_id 在 Ring 中保存原请求类型，cmd 表示 payload 当前所处协议阶段。
    uint32_t type_id = 0;
    any_t content = 0;
    // 可选协议字段。
    pld_cmd_t cmd = pld_cmd_t::UNDEF;
    // addr/size/data 描述本次内存访问及其数据缓冲区。
    uint64_t addr = 0;
    // mst_id/slv_id 分别标识发起 Master 和目标 Target。
    uint32_t mst_id = 0;
    uint32_t slv_id = 0;
    // mst_addr/slv_addr 在 Ring 路径中复用为源 Router 节点和目的 Router 节点。
    uint64_t mst_addr = 0;
    uint64_t slv_addr = 0;
    uint32_t size = 0;
    pld_rsp_t rsp = pld_rsp_t::UNDEF;
    std::shared_ptr<std::vector<uint8_t>> buf_u8 = nullptr;
    std::shared_ptr<std::vector<uint32_t>> buf_u32 = nullptr;
    std::shared_ptr<std::vector<uint64_t>> buf_u64 = nullptr;
    pld_data_t data = nullptr;  //lint !e524
    // chan 表示上层端口/通道；rsp_count 表示一笔读事务预期的响应分片数。
    uint32_t chan = 0;      // 通道编号，也可表示端口或虚通道。
    uint32_t latency = 0;
    uint32_t rsp_count = 1;
    uint64_t ts= 0;         // 事务时间戳。
    // tnx_id/tag_id 可承载 Memory 返回的 grant/DBID 关联信息。
    uint32_t tnx_id = 0;
    uint32_t tag_id = 0;
    uint32_t smmu_tnx_id = 0;
    // Ring 字段在逐跳过程中保持稳定；当前输入/输出方向保存在 Router Candidate 中。
    uint32_t ring_subnet = 0;
    uint32_t ring_traffic_class = 0;
    uint32_t ring_rsp_lane = 0;
private:
    static uint64_t cur_gid;
    static uint32_t cur_type_id;

};

// 全局静态变量初始化，必须在仿真启动前完成。
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
    // 不同 Master 可以使用相同 gid，因此事务表必须把 mst_id 纳入键值。
    return (static_cast<uint64_t>(pld->mst_id) << 32) | pld->gid;
}

inline void tm_pld_set_ring_route(p_tm_pld_t pld, uint32_t req_type,
                                  uint32_t target_id, uint32_t src_node,
                                  uint32_t dst_node) {
    // NIU 注入时写入稳定路由元数据；Router 后续只据此计算当前下一跳。
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
    // 未显式设置分片数时按单响应处理，避免 0 导致事务永远无法完成。
    if (pld == nullptr || pld->rsp_count == 0) {
        return 1;
    }
    return pld->rsp_count;
}



#endif
