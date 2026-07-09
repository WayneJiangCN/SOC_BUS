#ifndef _TM_MESH_TARGET_PORT_H_
#define _TM_MESH_TARGET_PORT_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

/*
 * TmMeshTargetPort
 *
 * target-side endpoint / ingress port。
 *
 * 它对应 mesh 右侧的 endpoint：
 * - Router 到达目的节点后，先把 request 落到 target-local queues
 * - TargetPort 再按 target 侧 ready / credit / busy time 把 request 送给下游
 * - 下游 target / TmMem 产生的响应也通过这里返回 Fabric
 *
 * 这样右侧结构就从“裸 target 接口”收成了：
 *   Router -> TargetPort -> TmMem
 */
class TmMeshTargetPort
{
  public:
    TmMeshTargetPort();
    TmMeshTargetPort(const std::string& name, tm_engine::p_tm_clk_t clk,
                     p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                     uint32_t inf_depth);
    ~TmMeshTargetPort();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                uint32_t inf_depth);
    void reset();
    bool idle() const;

    void attach_downstream(p_tm_com_inf_t inf);
    void attach_downstream(p_tm_mem_t mem);

    p_tm_com_inf_t inf() const;
    p_tm_com_que_t req_q(aic_req_type_t req_type) const;

  private:
    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_target_cfg_t cfg_ = nullptr;
    /* 与下游 target/TmMem 相连的接口 */
    p_tm_com_inf_t inf_ = nullptr;

    /* target-local queues */
    p_tm_com_que_t rd_req_q_ = nullptr;
    p_tm_com_que_t wr_req_q_ = nullptr;
    p_tm_com_que_t wr_dat_q_ = nullptr;
};

using tm_mesh_target_port_t = TmMeshTargetPort;
using p_tm_mesh_target_port_t = std::shared_ptr<tm_mesh_target_port_t>;

inline p_tm_mesh_target_port_t
tm_make_mesh_target_port(const std::string& name, tm_engine::p_tm_clk_t clk,
                         p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t inf_depth)
{
    return std::make_shared<TmMeshTargetPort>(name, clk, cfg, rd_rsp_port_num,
                                              inf_depth);
}

#endif  // _TM_MESH_TARGET_PORT_H_
