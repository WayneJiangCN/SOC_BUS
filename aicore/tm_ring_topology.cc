#include "tm_ring_topology.h"

void
TmRingTopology::config(p_tm_ring_cfg_t cfg)
{
    cfg_ = cfg;

    // 当前每个 endpoint 分配一个 ring stop，至少保留一个 Router 节点。
    uint32_t endpoint_count = cfg_->num_masters + cfg_->num_targets;
    router_count_ = endpoint_count == 0 ? 1 : endpoint_count;
    master_nodes_.clear();
    target_nodes_.clear();
    master_nodes_.resize(cfg_->num_masters, 0);
    target_nodes_.resize(cfg_->num_targets, 0);

    // Target 按比例分散到 Ring 上，降低所有存储分区集中在一侧造成的热点。
    std::vector<bool> target_slot(router_count_, false);
    if (cfg_->num_targets > 0) {
        for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
            uint32_t node =
                ((target + 1) * router_count_) / cfg_->num_targets - 1;
            target_nodes_[target] = node;
            target_slot[node] = true;
        }
    }

    // Master 顺序填入未被 Target 占用的节点。
    uint32_t master = 0;
    for (uint32_t node = 0;
         node < router_count_ && master < cfg_->num_masters; ++node) {
        if (target_slot[node]) {
            continue;
        }
        master_nodes_[master++] = node;
    }

}

void
TmRingTopology::reset(uint32_t num_masters)
{
    // 默认 mst_id 与端口号一致，外部可通过 bind_master_id() 覆盖。
    master_id_to_port_.clear();
    port_to_master_id_.assign(num_masters, 0);
    for (uint32_t i = 0; i < num_masters; ++i) {
        port_to_master_id_[i] = i;
        master_id_to_port_[i] = i;
    }
}

void
TmRingTopology::bind_master_id(uint32_t port_id, uint32_t mst_id)
{
    // 改绑前先删除旧反向映射，避免同一端口残留两个 mst_id。
    uint32_t old_mst_id = port_to_master_id_[port_id];
    if (old_mst_id != mst_id) {
        master_id_to_port_.erase(old_mst_id);
    }

    port_to_master_id_[port_id] = mst_id;
    master_id_to_port_[mst_id] = port_id;
}

uint32_t
TmRingTopology::port_master_id(uint32_t port_id) const
{
    return port_to_master_id_[port_id];
}

uint32_t
TmRingTopology::find_master_port(uint32_t mst_id) const
{
    // UINT32_MAX 明确表示未找到，调用方不能默认把响应送到 Master 0。
    auto it = master_id_to_port_.find(mst_id);
    return it == master_id_to_port_.end() ? UINT32_MAX : it->second;
}

uint32_t
TmRingTopology::decode_target(uint64_t addr) const
{
    // 默认 Target 只做兜底，优先返回地址范围和 interleave 都匹配的普通 Target。
    uint32_t default_target = 0;
    bool has_default = false;

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];

        if (target_cfg->is_default) {
            default_target = i;
            has_default = true;
            continue;
        }
        if (target_matches(addr, target_cfg)) {
            return i;
        }
    }

    return has_default ? default_target : 0;
}

bool
TmRingTopology::target_matches(uint64_t addr,
                               p_tm_ring_target_cfg_t target_cfg) const
{
    // 先做地址窗口过滤，再做更细粒度的交织切片判断。
    if (!target_cfg->contains(addr)) {
        return false;
    }
    if (!target_cfg->interleave_enabled()) {
        return true;
    }
    return calc_interleave_slice(addr, target_cfg) ==
           target_cfg->interleave_idx;
}

uint64_t
TmRingTopology::calc_interleave_stripe(
    uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const
{
    // stripe_id 表示地址位于该 Target 地址窗口内的第几个交织粒度块。
    return (addr - target_cfg->addr_begin) / target_cfg->interleave_size;
}

uint32_t
TmRingTopology::calc_linear_slice(uint64_t addr,
                                  p_tm_ring_target_cfg_t target_cfg) const
{
    uint64_t stripe_id = calc_interleave_stripe(addr, target_cfg);
    return static_cast<uint32_t>(stripe_id % target_cfg->interleave_num);
}

uint32_t
TmRingTopology::calc_xor_hash_slice(uint64_t addr,
                                    p_tm_ring_target_cfg_t target_cfg) const
{
    // XOR_HASH 混合高低位，降低固定 stride 访问持续命中同一 Target 的概率。
    uint64_t stripe_id = calc_interleave_stripe(addr, target_cfg);
    uint64_t hashed = stripe_id ^
                      (stripe_id >> target_cfg->interleave_hash_shift) ^
                      target_cfg->interleave_hash_seed;
    return static_cast<uint32_t>(hashed % target_cfg->interleave_num);
}

uint32_t
TmRingTopology::calc_interleave_slice(
    uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const
{
    if (tm_ring_is_xor_hash_interleave(target_cfg->interleave_type)) {
        return calc_xor_hash_slice(addr, target_cfg);
    }
    return calc_linear_slice(addr, target_cfg);
}

uint32_t
TmRingTopology::router_count() const
{
    return router_count_;
}

uint32_t
TmRingTopology::master_node(uint32_t master_port) const
{
    return master_nodes_[master_port];
}

uint32_t
TmRingTopology::target_node(uint32_t target_id) const
{
    return target_nodes_[target_id];
}

bool
TmRingTopology::has_neighbor(uint32_t node_id, TmRingPortDir dir) const
{
    (void)node_id;
    // 单节点 Ring 没有 EAST/WEST Link，但 LOCAL 端口始终存在。
    if (dir == TmRingPortDir::LOCAL) {
        return true;
    }
    return router_count_ > 1 &&
           (dir == TmRingPortDir::EAST || dir == TmRingPortDir::WEST);
}

uint32_t
TmRingTopology::neighbor(uint32_t node_id, TmRingPortDir dir) const
{
    if (router_count_ <= 1 || dir == TmRingPortDir::LOCAL) {
        return node_id;
    }
    // EAST 节点号递增，WEST 递减；取模实现首尾相连。
    if (dir == TmRingPortDir::EAST) {
        return (node_id + 1) % router_count_;
    }
    return (node_id + router_count_ - 1) % router_count_;
}

TmRingPortDir
TmRingTopology::route_direction(uint32_t cur_node, uint32_t dst_node) const
{
    if (cur_node == dst_node || router_count_ <= 1) {
        return TmRingPortDir::LOCAL;
    }

    // 分别计算顺/逆时针跳数，距离相等时选择 EAST 以获得确定性结果。
    uint32_t clockwise = (dst_node + router_count_ - cur_node) % router_count_;
    uint32_t counter_clockwise =
        (cur_node + router_count_ - dst_node) % router_count_;

    return clockwise <= counter_clockwise ? TmRingPortDir::EAST
                                          : TmRingPortDir::WEST;
}

uint32_t
TmRingTopology::compute_next_node(uint32_t cur_node, uint32_t dst_node) const
{
    return neighbor(cur_node, route_direction(cur_node, dst_node));
}
