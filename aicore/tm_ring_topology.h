#ifndef _TM_RING_TOPOLOGY_H_
#define _TM_RING_TOPOLOGY_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "tm_ring_types.h"

/*
 * Ring topology helper.
 *
 * This is a 1-D bidirectional ring:
 * 1. every router is a ring stop with a LOCAL injection/ejection port.
 * 2. target memory partitions are spread evenly around the ring.
 * 3. EAST means clockwise, WEST means counter-clockwise.
 * 4. routing picks the shorter ring distance and breaks ties clockwise.
 */
class TmRingTopology
{
  public:
    void config(p_tm_ring_cfg_t cfg);
    void reset(uint32_t num_masters);

    void bind_master_id(uint32_t port_id, uint32_t mst_id);
    uint32_t port_master_id(uint32_t port_id) const;
    uint32_t find_master_port(uint32_t mst_id) const;
    uint32_t decode_target(uint64_t addr) const;

    uint32_t router_count() const;

    uint32_t master_node(uint32_t master_port) const;
    uint32_t target_node(uint32_t target_id) const;

    bool has_neighbor(uint32_t node_id, TmRingPortDir dir) const;
    uint32_t neighbor(uint32_t node_id, TmRingPortDir dir) const;

    TmRingPortDir route_direction(uint32_t cur_node, uint32_t dst_node) const;
    uint32_t compute_next_node(uint32_t cur_node, uint32_t dst_node) const;

  private:
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
    std::unordered_map<uint32_t, uint32_t> master_id_to_port_;
    std::vector<uint32_t> port_to_master_id_;
    std::vector<uint32_t> master_nodes_;
    std::vector<uint32_t> target_nodes_;
    uint32_t router_count_ = 1;
};

#endif  // _TM_RING_TOPOLOGY_H_
