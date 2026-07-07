#include "tm_bus_topology.h"

/*
 * tm_bus_topology.cc
 *
 * 负责 ring 版本的：
 * - 配置期地址范围与 interleave 规则准备
 * - 运行期 master_id 映射
 * - addr -> target_id 解码
 * - master/target -> ring node 映射
 */

namespace
{

bool
ranges_overlap(const p_tm_bus_target_cfg_t& lhs, const p_tm_bus_target_cfg_t& rhs)
{
    if (lhs->size == 0 || rhs->size == 0) {
        return false;
    }

    uint64_t lhs_end = lhs->addr_begin + lhs->size;
    uint64_t rhs_end = rhs->addr_begin + rhs->size;
    return lhs->addr_begin < rhs_end && rhs->addr_begin < lhs_end;
}

bool
same_range(const p_tm_bus_target_cfg_t& lhs, const p_tm_bus_target_cfg_t& rhs)
{
    return lhs->addr_begin == rhs->addr_begin && lhs->size == rhs->size;
}

bool
same_interleave_group(const p_tm_bus_target_cfg_t& lhs,
                      const p_tm_bus_target_cfg_t& rhs)
{
    return lhs->interleave_type == rhs->interleave_type &&
           lhs->interleave_size == rhs->interleave_size &&
           lhs->interleave_num == rhs->interleave_num &&
           lhs->interleave_hash_shift == rhs->interleave_hash_shift &&
           lhs->interleave_hash_seed == rhs->interleave_hash_seed;
}

void
validate_target_cfg(const p_tm_bus_target_cfg_t& cfg)
{

    if (cfg->is_default) {
        return;
    }


    if (cfg->interleave_enabled()) {
    } else {
    }
}

void
validate_cfg(const p_tm_bus_cfg_t& cfg)
{

    uint32_t default_count = 0;
    for (uint32_t i = 0; i < cfg->num_targets; ++i) {
        auto target_cfg = cfg->targets[i];
        validate_target_cfg(target_cfg);
        if (target_cfg->is_default) {
            default_count++;
        }
    }

    for (uint32_t i = 0; i < cfg->num_targets; ++i) {
        auto lhs = cfg->targets[i];
        if (lhs->is_default) {
            continue;
        }

        for (uint32_t j = i + 1; j < cfg->num_targets; ++j) {
            auto rhs = cfg->targets[j];
            if (rhs->is_default) {
                continue;
            }

            if (!ranges_overlap(lhs, rhs)) {
                continue;
            }

        }
    }

    for (uint32_t i = 0; i < cfg->num_targets; ++i) {
        auto lhs = cfg->targets[i];
        if (lhs->is_default || !lhs->interleave_enabled()) {
            continue;
        }

        std::vector<uint8_t> seen(lhs->interleave_num, 0);
        uint32_t slice_count = 0;
        for (uint32_t j = 0; j < cfg->num_targets; ++j) {
            auto rhs = cfg->targets[j];
            if (rhs->is_default) {
                continue;
            }
            if (!same_range(lhs, rhs) || !same_interleave_group(lhs, rhs)) {
                continue;
            }

            seen[rhs->interleave_idx] = 1;
            slice_count++;
        }

    }
}

}  // namespace

void
TmBusTopology::config(p_tm_bus_cfg_t cfg)
{
    /* 配置阶段只做静态规则准备，不推进运行时状态。 */
    cfg_ = cfg;
    interleave_rules_.clear();

    validate_cfg(cfg_);

    interleave_rules_.resize(cfg_->num_targets);
    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        interleave_rules_[i] =
            tm_make_bus_interleave(cfg_->targets[i]->interleave_type);
    }
}

void
TmBusTopology::reset(uint32_t num_masters)
{
    /* 缺省情况下，port_id 和 mst_id 采用 1:1 映射。 */
    master_id_to_port_.clear();
    port_to_master_id_.assign(num_masters, 0);
    for (uint32_t i = 0; i < num_masters; ++i) {
        port_to_master_id_[i] = i;
        master_id_to_port_[i] = i;
    }
}

void
TmBusTopology::bind_master_id(uint32_t port_id, uint32_t mst_id)
{
    /* 显式绑定后，后续响应回程会按 mst_id 找回 master 端口。 */
    auto it = master_id_to_port_.find(mst_id);

    uint32_t old_mst_id = port_to_master_id_[port_id];
    if (old_mst_id != mst_id) {
        master_id_to_port_.erase(old_mst_id);
    }

    port_to_master_id_[port_id] = mst_id;
    master_id_to_port_[mst_id] = port_id;
}

uint32_t
TmBusTopology::port_master_id(uint32_t port_id) const
{
    return port_to_master_id_[port_id];
}

uint32_t
TmBusTopology::find_master_port(uint32_t mst_id) const
{
    auto it = master_id_to_port_.find(mst_id);
    return it->second;
}

uint32_t
TmBusTopology::decode_target(uint64_t addr) const
{
    /*
     * decode 的层次：
     * 1. 先按地址范围筛 target
     * 2. 对共享地址域再按 interleave/hash 选 slice
     * 3. 若无显式命中，再回落到 default target
     */
    uint32_t default_target = 0;
    bool has_default = false;
    bool hit_explicit_range = false;

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];

        if (target_cfg->is_default) {
            default_target = i;
            has_default = true;
            continue;
        }
        if (!target_cfg->contains(addr)) {
            continue;
        }

        hit_explicit_range = true;
        if (interleave_rules_[i]->matches(addr, target_cfg)) {
            return i;
        }
    }

    if (hit_explicit_range) {
    }

    return default_target;
}

uint32_t
TmBusTopology::ring_node_count() const
{
    return cfg_->num_masters + cfg_->num_targets;
}

uint32_t
TmBusTopology::master_node(uint32_t master_port) const
{
    return master_port;
}

uint32_t
TmBusTopology::target_node(uint32_t target_id) const
{
    return cfg_->num_masters + target_id;
}

uint32_t
TmBusTopology::next_ring_node(uint32_t node_id) const
{
    uint32_t num_nodes = ring_node_count();
    return (node_id + 1) % num_nodes;
}
