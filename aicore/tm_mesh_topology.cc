#include "tm_mesh_topology.h"

namespace
{

uint32_t
ceil_div_u32(uint32_t x, uint32_t y)
{
    return y == 0 ? x : (x + y - 1) / y;
}

}  // namespace

void
TmMeshTopology::config(p_tm_mesh_cfg_t cfg)
{
    cfg_ = cfg;
    interleave_rules_.clear();

    // 网格必须至少能容纳所有 master node 和 target node。
    uint32_t endpoint_count = cfg_->num_masters + cfg_->num_targets;
    rows_ = cfg_->mesh_rows == 0 ? 1 : cfg_->mesh_rows;
    cols_ = cfg_->mesh_cols;
    if (cols_ == 0) {
        cols_ = ceil_div_u32(endpoint_count, rows_);
    }
    if (rows_ * cols_ < endpoint_count) {
        cols_ = ceil_div_u32(endpoint_count, rows_);
    }
    if (cols_ == 0) {
        cols_ = 1;
    }
    router_count_ = rows_ * cols_;

    interleave_rules_.resize(cfg_->num_targets);
    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        interleave_rules_[i] =
            tm_make_bus_interleave(cfg_->targets[i]->interleave_type);
    }
}

void
TmMeshTopology::reset(uint32_t num_masters)
{
    // 默认绑定关系：port_id == mst_id。
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
TmMeshTopology::rows() const
{
    return rows_;
}

uint32_t
TmMeshTopology::cols() const
{
    return cols_;
}

uint32_t
TmMeshTopology::master_node(uint32_t master_port) const
{
    return master_port;
}

uint32_t
TmMeshTopology::target_node(uint32_t target_id) const
{
    return cfg_->num_masters + target_id;
}

bool
TmMeshTopology::has_neighbor(uint32_t node_id, TmMeshPortDir dir) const
{
    uint32_t row = row_of(node_id);
    uint32_t col = col_of(node_id);

    switch (dir) {
      case TmMeshPortDir::NORTH:
        return row > 0;
      case TmMeshPortDir::SOUTH:
        return row + 1 < rows_;
      case TmMeshPortDir::EAST:
        return col + 1 < cols_;
      case TmMeshPortDir::WEST:
        return col > 0;
      case TmMeshPortDir::LOCAL:
      default:
        return true;
    }
}

uint32_t
TmMeshTopology::neighbor(uint32_t node_id, TmMeshPortDir dir) const
{
    switch (dir) {
      case TmMeshPortDir::NORTH:
        return node_id - cols_;
      case TmMeshPortDir::SOUTH:
        return node_id + cols_;
      case TmMeshPortDir::EAST:
        return node_id + 1;
      case TmMeshPortDir::WEST:
        return node_id - 1;
      case TmMeshPortDir::LOCAL:
      default:
        return node_id;
    }
}

TmMeshPortDir
TmMeshTopology::route_direction(uint32_t cur_node, uint32_t dst_node) const
{
    // 当前支持最简单的确定性路由：X-first 或 Y-first。
    if (cur_node == dst_node) {
        return TmMeshPortDir::LOCAL;
    }

    uint32_t cur_row = row_of(cur_node);
    uint32_t cur_col = col_of(cur_node);
    uint32_t dst_row = row_of(dst_node);
    uint32_t dst_col = col_of(dst_node);

    if (cfg_->mesh_x_first) {
        if (cur_col < dst_col) {
            return TmMeshPortDir::EAST;
        }
        if (cur_col > dst_col) {
            return TmMeshPortDir::WEST;
        }
        if (cur_row < dst_row) {
            return TmMeshPortDir::SOUTH;
        }
        return TmMeshPortDir::NORTH;
    }

    if (cur_row < dst_row) {
        return TmMeshPortDir::SOUTH;
    }
    if (cur_row > dst_row) {
        return TmMeshPortDir::NORTH;
    }
    if (cur_col < dst_col) {
        return TmMeshPortDir::EAST;
    }
    return TmMeshPortDir::WEST;
}

uint32_t
TmMeshTopology::compute_next_node(uint32_t cur_node, uint32_t dst_node) const
{
    return neighbor(cur_node, route_direction(cur_node, dst_node));
}

uint32_t
TmMeshTopology::row_of(uint32_t node_id) const
{
    return cols_ == 0 ? 0 : node_id / cols_;
}

uint32_t
TmMeshTopology::col_of(uint32_t node_id) const
{
    return cols_ == 0 ? 0 : node_id % cols_;
}
