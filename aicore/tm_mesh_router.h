#ifndef _TM_MESH_ROUTER_H_
#define _TM_MESH_ROUTER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

/*
 * TmMeshRouter
 *
 * 显式方向化的 coarse transaction-level router。
 *
 * 结构上按 port 建模：
 * - LOCAL / NORTH / SOUTH / EAST / WEST
 * - 每个输入方向都有 request / data / response 队列
 * - 每个输出方向每拍做一次仲裁
 *
 * 这里仍然不是 flit/VC/router-pipeline 级模型，
 * 但已经能表达：
 *   输入口排队 -> 输出口仲裁 -> 链路发射
 */
class TmMeshRouter
{
  public:
    /*
     * 粗粒度仲裁统计里只区分三类流量：
     * 1. REQ      : RD_REQ / WR_REQ
     * 2. WR_DAT   : 写数据
     * 3. RSP      : 所有响应
     */
    enum class ArbTrafficKind : uint32_t
    {
        NONE = 0,
        REQ = 1,
        WR_DAT = 2,
        RSP = 3,
    };

    struct OutputArbDebug
    {
        /* 某个输出口一共经历了多少轮仲裁。 */
        uint64_t arbitration_rounds = 0;
        /* 某个输出口同时有多个候选时的轮数。 */
        uint64_t contention_rounds = 0;

        /* 各类流量曾参与该输出口仲裁的轮数。 */
        uint64_t req_eligible_rounds = 0;
        uint64_t wr_dat_eligible_rounds = 0;
        uint64_t rsp_eligible_rounds = 0;

        /* 各类流量累计有多少个 contender 进入过仲裁。 */
        uint64_t req_eligible_contenders = 0;
        uint64_t wr_dat_eligible_contenders = 0;
        uint64_t rsp_eligible_contenders = 0;

        /* 各类流量在该输出口累计赢过多少次。 */
        uint64_t req_wins = 0;
        uint64_t wr_dat_wins = 0;
        uint64_t rsp_wins = 0;

        /* 最近一次仲裁的调试快照。 */
        uint32_t last_eligible_count = 0;
        uint32_t last_winner_contender = static_cast<uint32_t>(-1);
        uint32_t last_winner_class = static_cast<uint32_t>(-1);
        TmMeshPortDir last_winner_in_dir = TmMeshPortDir::LOCAL;
        ArbTrafficKind last_winner_kind = ArbTrafficKind::NONE;
    };

    /*
     * 统一 traffic class 编号：
     * - REQ_CLASS         : RD_REQ / WR_REQ
     * - WR_DAT_CLASS      : WR_DAT
     * - WR_REQ_RSP_CLASS  : WR_REQ_RSP
     * - WR_DAT_RSP_CLASS  : WR_DAT_RSP
     * - RD_RSP_BASE_CLASS : 读响应 lane 从这里开始往后排
     */
    enum : uint32_t
    {
        REQ_CLASS = 0,
        WR_DAT_CLASS = 1,
        WR_REQ_RSP_CLASS = 2,
        WR_DAT_RSP_CLASS = 3,
        RD_RSP_BASE_CLASS = 4,
    };

    TmMeshRouter();
    TmMeshRouter(const std::string& name, tm_engine::p_tm_clk_t clk,
                 p_tm_mesh_cfg_t cfg);
    ~TmMeshRouter();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_mesh_cfg_t cfg);
    void reset();
    bool idle() const;

    uint32_t port_count() const;
    uint32_t traffic_class_count() const;
    uint32_t contender_count() const;
    /* 将 (输入方向, traffic class) 映射成仲裁器里的 contender 编号。 */
    uint32_t contender_id(TmMeshPortDir in_dir, uint32_t traffic_class) const;
    uint32_t contender_input_port(uint32_t contender) const;
    uint32_t contender_traffic_class(uint32_t contender) const;
    TmMeshPortDir contender_input_dir(uint32_t contender) const;
    ArbTrafficKind traffic_kind(uint32_t traffic_class) const;

    /* 各方向输入口的本地队列访问接口。 */
    p_tm_com_que_t req_q(TmMeshPortDir in_dir) const;
    p_tm_com_que_t wr_dat_q(TmMeshPortDir in_dir) const;
    p_tm_com_que_t rd_rsp_q(TmMeshPortDir in_dir, uint32_t lane) const;
    p_tm_com_que_t wr_req_rsp_q(TmMeshPortDir in_dir) const;
    p_tm_com_que_t wr_dat_rsp_q(TmMeshPortDir in_dir) const;

    p_tm_com_que_t queue_for_class(TmMeshPortDir in_dir,
                                   uint32_t traffic_class) const;
    p_tm_pld_t front_packet(TmMeshPortDir in_dir, uint32_t traffic_class) const;
    void pop_packet(TmMeshPortDir in_dir, uint32_t traffic_class);

    /*
     * 给某个输出方向做一次 RR 仲裁。
     * eligible_mask 由 fabric 按当前拍可发条件提前筛好。
     */
    bool pick_output_winner(TmMeshPortDir out_dir,
                            const std::vector<uint8_t>& eligible_mask,
                            uint32_t& winner);
    const OutputArbDebug& output_arb_debug(TmMeshPortDir out_dir) const;

  private:
    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    /* 每个输入口的 request / data / response 队列。 */
    std::vector<p_tm_com_que_t> req_qs_;
    std::vector<p_tm_com_que_t> wr_dat_qs_;
    std::vector<std::vector<p_tm_com_que_t>> rd_rsp_qs_;
    std::vector<p_tm_com_que_t> wr_req_rsp_qs_;
    std::vector<p_tm_com_que_t> wr_dat_rsp_qs_;

    /* 每个输出口一份粗粒度仲裁调试信息。 */
    std::vector<OutputArbDebug> output_arb_debugs_;
    /* 每个输出口一份 RR 指针。 */
    std::unordered_map<uint32_t, uint32_t> output_rr_ptr_;
};

using tm_mesh_router_t = TmMeshRouter;
using p_tm_mesh_router_t = std::shared_ptr<tm_mesh_router_t>;

inline p_tm_mesh_router_t
tm_make_mesh_router(const std::string& name, tm_engine::p_tm_clk_t clk,
                    p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<TmMeshRouter>(name, clk, cfg);
}

#endif  // _TM_MESH_ROUTER_H_
