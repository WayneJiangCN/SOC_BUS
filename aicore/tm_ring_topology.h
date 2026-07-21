#ifndef _TM_RING_TOPOLOGY_H_
#define _TM_RING_TOPOLOGY_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "tm_ring_types.h"

/*
 * Ring 拓扑辅助模块。
 *
 * 当前模型是一维双向 Ring：每个 Router 是带 LOCAL 注入/弹出端口的 ring stop；
 * Target 存储分区尽量均匀分布；EAST 为顺时针、WEST 为逆时针；
 * 路由选择跳数更少的方向，距离相等时固定选择 EAST。
 */
class TmRingTopology
{
  public:
    // 根据 Master/Target 数量生成 Ring stop，并尽量均匀放置 Target。
    void config(p_tm_ring_cfg_t cfg);
    // reset 仅恢复 Master ID 映射，不改变已经生成的物理节点布局。
    void reset(uint32_t num_masters);

    void bind_master_id(uint32_t port_id, uint32_t mst_id);
    uint32_t port_master_id(uint32_t port_id) const;
    uint32_t find_master_port(uint32_t mst_id) const;
    // 地址优先匹配非默认 Target；无匹配时回退到配置的默认 Target。
    uint32_t decode_target(uint64_t addr) const;

    uint32_t router_count() const;

    uint32_t master_node(uint32_t master_port) const;
    uint32_t target_node(uint32_t target_id) const;

    bool has_neighbor(uint32_t node_id, TmRingPortDir dir) const;
    uint32_t neighbor(uint32_t node_id, TmRingPortDir dir) const;

    // 返回当前节点的下一跳方向；等距时固定选择 EAST，保证结果确定。
    TmRingPortDir route_direction(uint32_t cur_node, uint32_t dst_node) const;
    uint32_t compute_next_node(uint32_t cur_node, uint32_t dst_node) const;

  private:
    // 地址命中既要满足地址范围，也要满足可选的 interleave slice。
    bool target_matches(uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const;
    uint64_t calc_interleave_stripe(uint64_t addr,
                                    p_tm_ring_target_cfg_t target_cfg) const;
    uint32_t calc_linear_slice(uint64_t addr,
                               p_tm_ring_target_cfg_t target_cfg) const;
    uint32_t calc_xor_hash_slice(uint64_t addr,
                                 p_tm_ring_target_cfg_t target_cfg) const;
    uint32_t calc_interleave_slice(uint64_t addr,
                                   p_tm_ring_target_cfg_t target_cfg) const;

    p_tm_ring_cfg_t cfg_ = nullptr;
    // 双向映射用于把响应中的 mst_id 还原为本地 Master 端口。
    std::unordered_map<uint32_t, uint32_t> master_id_to_port_;
    std::vector<uint32_t> port_to_master_id_;
    std::vector<uint32_t> master_nodes_;
    std::vector<uint32_t> target_nodes_;
    uint32_t router_count_ = 1;
};

#endif  // _TM_RING_TOPOLOGY_H_
