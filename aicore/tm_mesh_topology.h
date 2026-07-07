#ifndef _TM_MESH_TOPOLOGY_H_
#define _TM_MESH_TOPOLOGY_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "tm_bus_interleave.h"
#include "tm_mesh_types.h"

/*
 * TmMeshTopology:
 * 负责 mesh 版本的地址解码、master_id 绑定、router 网格尺寸计算，
 * 以及确定性坐标路由的下一跳选择。
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
    uint32_t rows() const;
    uint32_t cols() const;

    uint32_t master_node(uint32_t master_port) const;
    uint32_t target_node(uint32_t target_id) const;
    uint32_t compute_next_node(uint32_t cur_node, uint32_t dst_node) const;

  private:
    uint32_t row_of(uint32_t node_id) const;
    uint32_t col_of(uint32_t node_id) const;

  private:
    p_tm_mesh_cfg_t cfg_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> master_id_to_port_;
    std::vector<uint32_t> port_to_master_id_;
    std::vector<p_tm_bus_interleave_t> interleave_rules_;
    uint32_t rows_ = 1;
    uint32_t cols_ = 1;
    uint32_t router_count_ = 1;
};

#endif  // _TM_MESH_TOPOLOGY_H_
