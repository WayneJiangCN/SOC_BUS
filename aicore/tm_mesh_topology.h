#ifndef _TM_MESH_TOPOLOGY_H_
#define _TM_MESH_TOPOLOGY_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "tm_bus_interleave.h"
#include "tm_mesh_types.h"

/*
 * TmMeshTopology
 *
 * 负责整张 mesh 的静态拓扑和地址解码：
 * 1. 根据 rows / cols 生成 router 网格规模。
 * 2. 管理 master_port <-> master_id 的绑定。
 * 3. 将地址 decode 到目标 target。
 * 4. 给出某个节点的邻居，以及当前节点到目标节点的下一跳方向。
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

    /* master/target 在网格中的本地接入节点。 */
    uint32_t master_node(uint32_t master_port) const;
    uint32_t target_node(uint32_t target_id) const;

    /* 查询某个节点在某个方向上是否存在邻居，以及邻居是谁。 */
    bool has_neighbor(uint32_t node_id, TmMeshPortDir dir) const;
    uint32_t neighbor(uint32_t node_id, TmMeshPortDir dir) const;

    /* 从当前节点到目标节点的路由方向和下一跳节点。 */
    TmMeshPortDir route_direction(uint32_t cur_node, uint32_t dst_node) const;
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
