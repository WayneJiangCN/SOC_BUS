#include "tm_mesh_topology.h"

void
TmMeshTopology::config(p_tm_mesh_cfg_t cfg)
{
    cfg_ = cfg;
    interleave_rules_.clear();

    uint32_t endpoint_count = cfg_->num_masters + cfg_->num_targets;
    router_count_ = endpoint_count == 0 ? 1 : endpoint_count;
    master_nodes_.clear();
    target_nodes_.clear();
    master_nodes_.resize(cfg_->num_masters, 0);
    target_nodes_.resize(cfg_->num_targets, 0);

    std::vector<bool> target_slot(router_count_, false);
    if (cfg_->num_targets > 0) {
        for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
            uint32_t node =
                ((target + 1) * router_count_) / cfg_->num_targets - 1;
            target_nodes_[target] = node;
            target_slot[node] = true;
        }
    }

    uint32_t master = 0;
    for (uint32_t node = 0;
         node < router_count_ && master < cfg_->num_masters; ++node) {
        if (target_slot[node]) {
            continue;
        }
        master_nodes_[master++] = node;
    }

    interleave_rules_.resize(cfg_->num_targets);
    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        interleave_rules_[i] =
            tm_make_bus_interleave(cfg_->targets[i]->interleave_type);
    }
}

void
TmMeshTopology::reset(uint32_t num_masters)
{
    master_id_to_port_.clear();
    port_to_master_id_.assign(num_masters, 0);
    for (uint32_t i = 0; i < num_masters; ++i) {
        port_to_master_id_[i] = i;
        master_id_to_port_[i] = i;
    }
}

void
TmMeshTopology::bind_master_id(uint32_t port_id, uint32_t mst_id)
{
    uint32_t old_mst_id = port_to_master_id_[port_id];
    if (old_mst_id != mst_id) {
        master_id_to_port_.erase(old_mst_id);
    }

    port_to_master_id_[port_id] = mst_id;
    master_id_to_port_[mst_id] = port_id;
}

uint32_t
TmMeshTopology::port_master_id(uint32_t port_id) const
{
    return port_to_master_id_[port_id];
}

uint32_t
TmMeshTopology::find_master_port(uint32_t mst_id) const
{
    auto it = master_id_to_port_.find(mst_id);
    return it == master_id_to_port_.end() ? 0 : it->second;
}

uint32_t
TmMeshTopology::decode_target(uint64_t addr) const
{
    uint32_t default_target = 0;
    bool has_default = false;

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

        if (interleave_rules_[i]->matches(addr, target_cfg)) {
            return i;
        }
    }

    return has_default ? default_target : 0;
}

uint32_t
TmMeshTopology::router_count() const
{
    return router_count_;
}

uint32_t
TmMeshTopology::master_node(uint32_t master_port) const
{
    return master_nodes_[master_port];
}

uint32_t
TmMeshTopology::target_node(uint32_t target_id) const
{
    return target_nodes_[target_id];
}

bool
TmMeshTopology::has_neighbor(uint32_t node_id, TmMeshPortDir dir) const
{
    (void)node_id;
    if (dir == TmMeshPortDir::LOCAL) {
        return true;
    }
    return router_count_ > 1 &&
           (dir == TmMeshPortDir::EAST || dir == TmMeshPortDir::WEST);
}

uint32_t
TmMeshTopology::neighbor(uint32_t node_id, TmMeshPortDir dir) const
{
    if (router_count_ <= 1 || dir == TmMeshPortDir::LOCAL) {
        return node_id;
    }
    if (dir == TmMeshPortDir::EAST) {
        return (node_id + 1) % router_count_;
    }
    return (node_id + router_count_ - 1) % router_count_;
}

TmMeshPortDir
TmMeshTopology::route_direction(uint32_t cur_node, uint32_t dst_node) const
{
    if (cur_node == dst_node || router_count_ <= 1) {
        return TmMeshPortDir::LOCAL;
    }

    uint32_t clockwise = (dst_node + router_count_ - cur_node) % router_count_;
    uint32_t counter_clockwise =
        (cur_node + router_count_ - dst_node) % router_count_;

    return clockwise <= counter_clockwise ? TmMeshPortDir::EAST
                                          : TmMeshPortDir::WEST;
}

uint32_t
TmMeshTopology::compute_next_node(uint32_t cur_node, uint32_t dst_node) const
{
    return neighbor(cur_node, route_direction(cur_node, dst_node));
}
