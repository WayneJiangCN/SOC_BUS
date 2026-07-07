#include "tm_mesh_inf.h"

#include <algorithm>
#include <vector>

using namespace std;
using namespace tm_engine;

/*
 * 这份实现文件专注于“单个 master 的本地接入单元”：
 * - 请求先进入 NIU 本地 FIFO；
 * - fabric 再从这些 FIFO 取包并注入 mesh；
 * - mesh 回来的响应先落到 NIU 本地 FIFO；
 * - NIU 再把响应送回上游接口。
 *
 * 可以把它理解成：
 * master endpoint <-> Tm_mesh_inf <-> TmMeshFabric
 */

Tm_mesh_inf::Tm_mesh_inf(const std::string& name, p_tm_clk_t clk,
                         uint32_t inf_id, p_tm_mesh_cfg_t cfg)
    : TmModule(name), clk_(clk), cfg_(cfg), inf_id_(inf_id)
{
    config();
}

Tm_mesh_inf::~Tm_mesh_inf()
{
}

void
Tm_mesh_inf::config()
{
    /*
     * 通道号约定沿用当前事务协议：
     * - chan0: WR_REQ / WR_REQ_RSP
     * - chan1: WR_DAT / WR_DAT_RSP
     * - chan2.. : RD_REQ / RD_RSP(lane)
     */
    uint32_t chan_num = static_cast<uint32_t>(aic_req_type_t::RD_REQ) +
                        cfg_->rd_rsp_port_num;
    bus_inf_ = tm_make_com_inf(clk_, this->name() + "_bus_inf",
                               cfg_->master_inf_depth);
    bus_inf_->set_chan_num(chan_num);

    /* 本地请求 FIFO：负责把 master 的请求和 mesh 的注入过程解耦。 */
    rd_req_fifo_ = tm_make_com_que(clk_, this->name() + "_rd_req_fifo",
                                   cfg_->master_rd_req_fifo_depth);
    wr_req_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_req_fifo",
                                   cfg_->master_wr_req_fifo_depth);
    wr_dat_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_dat_fifo",
                                   cfg_->master_wr_dat_fifo_depth);

    /* 读响应支持多 lane，因此本地也按 lane 分离缓存。 */
    rd_rsp_fifo_.clear();
    next_rd_rsp_issue_time_.assign(cfg_->rd_rsp_port_num, 0);
    for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
        rd_rsp_fifo_.push_back(
            tm_make_com_que(clk_, this->name() + "_rd_rsp_fifo_" +
                                      std::to_string(lane),
                            cfg_->master_rd_rsp_fifo_depth));
    }

    wr_req_rsp_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_req_rsp_fifo",
                                       cfg_->master_wr_req_rsp_fifo_depth);
    wr_dat_rsp_fifo_ = tm_make_com_que(clk_, this->name() + "_wr_dat_rsp_fifo",
                                       cfg_->master_wr_dat_rsp_fifo_depth);

    /* config 结束后直接走一次 reset，确保所有队列和计时状态归零。 */
    reset();
}

void
Tm_mesh_inf::reset()
{
    /* reset 同时清空请求、本地回包、grant 和 API 完成记录。 */
    bus_inf_->reset();
    rd_req_fifo_->clear();
    wr_req_fifo_->clear();
    wr_dat_fifo_->clear();
    for (auto& q : rd_rsp_fifo_) {
        q->clear();
    }
    wr_req_rsp_fifo_->clear();
    wr_dat_rsp_fifo_->clear();
    wr_grant_fifo_.clear();
    api_req_map_.clear();
    req_id_ = 0;
    std::fill(next_rd_rsp_issue_time_.begin(), next_rd_rsp_issue_time_.end(), 0);
    next_wr_req_rsp_issue_time_ = 0;
    next_wr_dat_rsp_issue_time_ = 0;
}

bool
Tm_mesh_inf::idle()
{
    /*
     * idle 的含义是：
     * 这个 NIU 当前没有待注入的请求，也没有待发回上游的响应，
     * 也没有悬挂的 grant 或 API 请求。
     */
    bool ret = bus_inf_->idle() && rd_req_fifo_->empty() && wr_req_fifo_->empty() &&
               wr_dat_fifo_->empty() && wr_req_rsp_fifo_->empty() &&
               wr_dat_rsp_fifo_->empty() && wr_grant_fifo_.empty() &&
               api_req_map_.empty();
    for (auto& q : rd_rsp_fifo_) {
        ret = ret && q->empty();
    }
    return ret;
}

void
Tm_mesh_inf::connect_upstream(p_tm_com_inf_t inf)
{
    /* 允许上游 master/BIU 直接把自己的 com_inf 接到 NIU 上。 */
    bus_inf_->connect(inf);
}

void
Tm_mesh_inf::bind_master_id(uint32_t mst_id)
{
    /* master_id 可能由 fabric 统一分配或重绑定，因此单独暴露接口。 */
    inf_id_ = mst_id;
}

uint32_t
Tm_mesh_inf::send_rd_req(uint64_t address, uint32_t size)
{
    /*
     * 本地 API 模式：
     * 直接在 NIU 内部创建 pld，并压入 rd_req_fifo_。
     * 后续由 fabric 像处理普通 master 请求一样取走并注入 mesh。
     */
    auto req = tm_make_pld(pld_cmd_t::RD, address, size);
    req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
    req->data = pld_data_t((req->buf_u32)->data());
    req->mst_id = inf_id_;

    uint32_t cur_req_id = req_id_++;
    req->gid = cur_req_id;
    api_req_map_[cur_req_id].req_type = aic_req_type_t::RD_REQ;
    rd_req_fifo_->push_back(req);
    return cur_req_id;
}

uint32_t
Tm_mesh_inf::send_wr_req(uint64_t address, uint32_t size)
{
    /*
     * 写事务 API 模式只生成 WR_REQ。
     * 真正的 WR_DAT 要等后续 WR_REQ_RSP/grant 到达后再继续走。
     */
    auto req = tm_make_pld(pld_cmd_t::WR, address, size);
    req->buf_u32 = make_shared<vector<uint32_t>>(size, 0);
    req->data = pld_data_t((req->buf_u32)->data());
    req->mst_id = inf_id_;

    uint32_t cur_req_id = req_id_++;
    req->gid = cur_req_id;
    api_req_map_[cur_req_id].req_type = aic_req_type_t::WR_REQ;
    wr_req_fifo_->push_back(req);
    return cur_req_id;
}

bool
Tm_mesh_inf::completed(uint32_t req_id)
{
    /* 只对本地 API 请求生效：不在 map 里就表示已经完成。 */
    return api_req_map_.find(req_id) == api_req_map_.end();
}

bool
Tm_mesh_inf::canSendRdReq()
{
    /* 本地 API 是否还能继续塞读请求。 */
    return !rd_req_fifo_->full();
}

bool
Tm_mesh_inf::canSendWrReq()
{
    /* 本地 API 是否还能继续塞写请求头。 */
    return !wr_req_fifo_->full();
}

void
Tm_mesh_inf::ingest_upstream_reqs(uint32_t master_port,
                                  const TmMeshTopology& topology,
                                  unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
                                  tm_time_t now)
{
    /*
     * 这是“上游接口模式”的入口：
     * 从 bus_inf_ 吸收 RD_REQ / WR_REQ / WR_DAT，
     * 转成 NIU 本地 FIFO 中的排队请求。
     *
     * 对 RD_REQ / WR_REQ：
     *   在这里为共享 fabric 建立 txn_ctx。
     * 对 WR_DAT：
     *   不新建事务，只复用之前 WR_REQ 创建的上下文。
     */
    const aic_req_type_t req_order[] = {
        aic_req_type_t::WR_REQ, aic_req_type_t::WR_DAT, aic_req_type_t::RD_REQ};

    for (auto req_type : req_order) {
        uint32_t chan = static_cast<uint32_t>(req_type);
        auto fifo = req_fifo(req_type);

        while (bus_inf_->valid(chan) && !fifo->full()) {
            auto pld = bus_inf_->get_pld(chan);
            if (pld->mst_id == 0) {
                pld->mst_id = inf_id_;
            }

            auto key = make_txn_key(pld);
            auto it = txn_ctx.find(key);
            if (req_type == aic_req_type_t::WR_DAT) {
                /*
                 * WR_DAT 必须依附在已有 WR_REQ 事务上。
                 * 如果连对应上下文都没有，说明上游时序不对，当前包先不吃掉。
                 */
                if (it == txn_ctx.end()) {
                    break;
                }
                it->second.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
            } else {
                /* 新的读请求/写请求头在这里登记共享事务上下文。 */
                TmMeshTxnCtx ctx;
                ctx.master_port = master_port;
                ctx.target_id = topology.decode_target(pld->addr);
                ctx.src_node = topology.master_node(ctx.master_port);
                ctx.dst_node = topology.target_node(ctx.target_id);
                ctx.req_type = req_type;
                ctx.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
                ctx.size = pld->size;
                ctx.issue_time = now;
                txn_ctx.emplace(key, ctx);
            }

            fifo->push_back(pld);
            bus_inf_->pop_pld(chan);
        }
    }
}

bool
Tm_mesh_inf::has_req(aic_req_type_t req_type) const
{
    /* fabric 用它判断这个 NIU 某类请求是否还有待注入项。 */
    return !req_fifo(req_type)->empty();
}

p_tm_pld_t
Tm_mesh_inf::front_req(aic_req_type_t req_type) const
{
    /* fabric 只看队头，不直接操作内部 FIFO。 */
    auto fifo = req_fifo(req_type);
    return fifo->empty() ? nullptr : fifo->front();
}

void
Tm_mesh_inf::pop_req(aic_req_type_t req_type)
{
    /* fabric 在成功注入 mesh 后，再通知 NIU 弹出本地请求队头。 */
    auto fifo = req_fifo(req_type);
    if (!fifo->empty()) {
        fifo->pop_front();
    }
}

bool
Tm_mesh_inf::has_grant() const
{
    /* WR_DAT 是否已经拿到至少一个可消费的 grant。 */
    return !wr_grant_fifo_.empty();
}

const TmMeshGrant&
Tm_mesh_inf::front_grant() const
{
    /* fabric 只读取 grant 队头，匹配成功后再 pop。 */
    return wr_grant_fifo_.front();
}

void
Tm_mesh_inf::pop_grant()
{
    /* WR_DAT 真正发给 target 后，对应 grant 才出队。 */
    if (!wr_grant_fifo_.empty()) {
        wr_grant_fifo_.pop_front();
    }
}

bool
Tm_mesh_inf::can_accept_rd_rsp(uint32_t lane) const
{
    /* 某个读响应 lane 是否还能接收一笔回包。 */
    return lane < rd_rsp_fifo_.size() && !rd_rsp_fifo_[lane]->full();
}

bool
Tm_mesh_inf::can_accept_wr_req_rsp() const
{
    /* 是否还能缓存一笔 WR_REQ_RSP。 */
    return !wr_req_rsp_fifo_->full();
}

bool
Tm_mesh_inf::can_accept_wr_dat_rsp() const
{
    /* 是否还能缓存一笔 WR_DAT_RSP。 */
    return !wr_dat_rsp_fifo_->full();
}

bool
Tm_mesh_inf::can_accept_grant() const
{
    /* 本地 grant FIFO 是否还有空位。 */
    return wr_grant_fifo_.size() < cfg_->master_wr_grant_fifo_depth;
}

bool
Tm_mesh_inf::accept_rd_rsp(p_tm_pld_t rsp, uint32_t lane)
{
    /*
     * 读响应到达 source NIU 后分两类处理：
     * 1. 本地 API 请求：直接在这里更新完成计数，不再向上游转发；
     * 2. 普通上游接口请求：先进入本地 rd_rsp_fifo_，后续再由 bus_inf_ 发回。
     */
    auto it = api_req_map_.find(rsp->gid);
    if (it != api_req_map_.end() && it->second.req_type == aic_req_type_t::RD_REQ) {
        if (it->second.rsp_expected == 1 && rsp->latency > 1) {
            it->second.rsp_expected = rsp->latency;
        }
        it->second.rsp_seen++;
        if (it->second.rsp_seen >= it->second.rsp_expected) {
            api_req_map_.erase(it);
        }
        return true;
    }

    if (!can_accept_rd_rsp(lane)) {
        return false;
    }
    rd_rsp_fifo_[lane]->push_back(rsp);
    return true;
}

bool
Tm_mesh_inf::accept_wr_req_rsp(p_tm_pld_t rsp, const TmMeshGrant& grant)
{
    /*
     * WR_REQ_RSP 同时承担两件事：
     * 1. 生成可供 WR_DAT 消费的 grant；
     * 2. 作为响应回给上游，或者驱动本地 API 写事务继续前进。
     */
    auto it = api_req_map_.find(rsp->gid);
    bool is_api_write =
        it != api_req_map_.end() && it->second.req_type == aic_req_type_t::WR_REQ;

    if (!can_accept_grant() || (is_api_write && wr_dat_fifo_->full())) {
        return false;
    }
    wr_grant_fifo_.push_back(grant);

    if (is_api_write) {
        /*
         * 本地 API 写事务没有上游 WR_REQ_RSP 消费者，
         * 因此直接把这笔包转成后续待发的 WR_DAT。
         */
        wr_dat_fifo_->push_back(rsp);
        return true;
    }

    /* 普通接口模式则把 WR_REQ_RSP 暂存，等待之后回送上游。 */
    if (!can_accept_wr_req_rsp()) {
        wr_grant_fifo_.pop_back();
        return false;
    }
    wr_req_rsp_fifo_->push_back(rsp);
    return true;
}

bool
Tm_mesh_inf::accept_wr_dat_rsp(p_tm_pld_t rsp)
{
    /*
     * WR_DAT_RSP 是写事务真正完成点。
     * 对 API 写请求，直接在这里 retire；
     * 对普通接口模式，则排进本地 wr_dat_rsp_fifo_ 再回送上游。
     */
    if (retire_api_req(rsp)) {
        return true;
    }

    if (!can_accept_wr_dat_rsp()) {
        return false;
    }
    wr_dat_rsp_fifo_->push_back(rsp);
    return true;
}

void
Tm_mesh_inf::service_rsp_outputs(const TmBusFlowCtrl& flow_ctrl,
                                 const unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
                                 tm_time_t now)
{
    /*
     * 共享 mesh 事务在 NIU 边界就算“网络部分完成”，
     * 但 NIU 仍需按本地输出节拍把响应送回上游接口。
     *
     * 这里不再修改 txn_ctx，只利用它查询 target_id，
     * 以便沿用 target 侧响应 busy-cycle 模型。
     */
    for (uint32_t lane = 0; lane < rd_rsp_fifo_.size(); ++lane) {
        auto fifo = rd_rsp_fifo_[lane];
        if (fifo->empty() || now < next_rd_rsp_issue_time_[lane]) {
            continue;
        }

        auto rsp = fifo->front();
        uint32_t chan = static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
        if (bus_inf_->send(chan, rsp)) {
            fifo->pop_front();
            auto ctx_it = txn_ctx.find(make_txn_key(rsp));
            if (ctx_it != txn_ctx.end()) {
                next_rd_rsp_issue_time_[lane] =
                    now + flow_ctrl.calc_rsp_busy_cycles(ctx_it->second.target_id, rsp);
            }
        }
    }

    if (!wr_req_rsp_fifo_->empty() && now >= next_wr_req_rsp_issue_time_) {
        auto rsp = wr_req_rsp_fifo_->front();
        if (bus_inf_->send(static_cast<uint32_t>(aic_req_type_t::WR_REQ), rsp)) {
            wr_req_rsp_fifo_->pop_front();
            auto ctx_it = txn_ctx.find(make_txn_key(rsp));
            if (ctx_it != txn_ctx.end()) {
                next_wr_req_rsp_issue_time_ =
                    now + flow_ctrl.calc_rsp_busy_cycles(ctx_it->second.target_id, rsp);
            }
        }
    }

    if (!wr_dat_rsp_fifo_->empty() && now >= next_wr_dat_rsp_issue_time_) {
        auto rsp = wr_dat_rsp_fifo_->front();
        if (bus_inf_->send(static_cast<uint32_t>(aic_req_type_t::WR_DAT), rsp)) {
            wr_dat_rsp_fifo_->pop_front();
            auto ctx_it = txn_ctx.find(make_txn_key(rsp));
            if (ctx_it != txn_ctx.end()) {
                next_wr_dat_rsp_issue_time_ =
                    now + flow_ctrl.calc_rsp_busy_cycles(ctx_it->second.target_id, rsp);
            }
        }
    }
}

p_tm_com_que_t
Tm_mesh_inf::req_fifo(aic_req_type_t req_type) const
{
    /* 把三类本地请求 FIFO 收口到一个小 helper，避免调用点重复判断。 */
    if (req_type == aic_req_type_t::RD_REQ) {
        return rd_req_fifo_;
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return wr_req_fifo_;
    }
    return wr_dat_fifo_;
}

uint64_t
Tm_mesh_inf::make_txn_key(uint32_t mst_id, uint32_t gid) const
{
    /* 和 fabric 保持一致：用 mst_id + gid 组合成唯一事务键。 */
    return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t
Tm_mesh_inf::make_txn_key(p_tm_pld_t pld) const
{
    return make_txn_key(pld->mst_id, pld->gid);
}

bool
Tm_mesh_inf::retire_api_req(p_tm_pld_t rsp)
{
    /*
     * 仅服务本地 API 写事务：
     * 当 WR_DAT_RSP 回来时，把对应 req_id 从 api_req_map_ 删除，
     * completed(req_id) 之后就会返回 true。
     */
    auto it = api_req_map_.find(rsp->gid);
    if (it == api_req_map_.end()) {
        return false;
    }
    if (it->second.req_type != aic_req_type_t::WR_REQ) {
        return false;
    }

    api_req_map_.erase(it);
    return true;
}
