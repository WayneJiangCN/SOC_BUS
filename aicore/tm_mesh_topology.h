#ifndef _TM_MESH_TOPOLOGY_H_
#define _TM_MESH_TOPOLOGY_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "tm_bus_interleave.h"
#include "tm_mesh_types.h"

/*
 * Ring topology helper.
 *
 * The historical class name is kept to avoid touching every caller. Internally
 * this is now a 1-D bidirectional ring:
 * 1. every router is a ring stop with a LOCAL injection/ejection port.
 * 2. target memory partitions are spread evenly around the ring.
 * 3. EAST means clockwise, WEST means counter-clockwise.
 * 4. routing picks the shorter ring distance and breaks ties clockwise.
 */
class TmMeshTopology
{
  public:
    void config(p_tm_mesh_cfg_t cfg);
    void reset(uint32_t num_masters);

    void bind_master_id(uint32_t port_id, uint32_t mst_id);
    uint32_t port_master_id(uint32_t port_id) const;
    uint32_t find_master_port(uint32_t mst_id) const;
    uint32_t decode_target(uint64_t addr) const;

    uint32_t router_count() const;

    uint32_t master_node(uint32_t master_port) const;
    uint32_t target_node(uint32_t target_id) const;

    bool has_neighbor(uint32_t node_id, TmMeshPortDir dir) const;
    uint32_t neighbor(uint32_t node_id, TmMeshPortDir dir) const;

    TmMeshPortDir route_direction(uint32_t cur_node, uint32_t dst_node) const;
    uint32_t compute_next_node(uint32_t cur_node, uint32_t dst_node) const;

  private:
    p_tm_mesh_cfg_t cfg_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> master_id_to_port_;
    std::vector<uint32_t> port_to_master_id_;
    std::vector<uint32_t> master_nodes_;
    std::vector<uint32_t> target_nodes_;
    std::vector<p_tm_bus_interleave_t> interleave_rules_;
    uint32_t router_count_ = 1;
};

#endif  // _TM_MESH_TOPOLOGY_H_
