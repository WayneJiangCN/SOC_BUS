#ifndef _TM_BUS_TOPOLOGY_H_
#define _TM_BUS_TOPOLOGY_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "tm_bus_interleave.h"
#include "tm_bus_types.h"

/*
 * TmBusTopology:
 * 负责 ring 版本的地址解码、master_id 映射和 ring 节点编号映射。
 */
class TmBusTopology
{
  public:
    void config(p_tm_bus_cfg_t cfg);
    void reset(uint32_t num_masters);

    void bind_master_id(uint32_t port_id, uint32_t mst_id);
    uint32_t port_master_id(uint32_t port_id) const;
    uint32_t find_master_port(uint32_t mst_id) const;
    uint32_t decode_target(uint64_t addr) const;

    uint32_t ring_node_count() const;
    uint32_t master_node(uint32_t master_port) const;
    uint32_t target_node(uint32_t target_id) const;
    uint32_t next_ring_node(uint32_t node_id) const;

  private:
    p_tm_bus_cfg_t cfg_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> master_id_to_port_;
    std::vector<uint32_t> port_to_master_id_;
    std::vector<p_tm_bus_interleave_t> interleave_rules_;
};

#endif  // _TM_BUS_TOPOLOGY_H_
