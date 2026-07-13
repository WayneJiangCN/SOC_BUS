#ifndef _TM_MESH_PLD_H_
#define _TM_MESH_PLD_H_

#include "tm_mesh_types.h"

inline uint64_t
tm_mesh_pld_key(p_tm_pld_t pld)
{
    return (static_cast<uint64_t>(pld->mst_id) << 32) | pld->gid;
}

inline void
tm_mesh_pld_set_route(p_tm_pld_t pld, uint32_t target_id, uint32_t src_node,
                      uint32_t dst_node, aic_req_type_t req_type,
                      tm_engine::tm_time_t now)
{
    pld->type_id = static_cast<uint32_t>(req_type);
    pld->slv_id = target_id;
    pld->mst_addr = src_node;
    pld->slv_addr = dst_node;
    pld->ts = now;
}

inline aic_req_type_t
tm_mesh_pld_req_type(p_tm_pld_t pld)
{
    return static_cast<aic_req_type_t>(pld->type_id);
}

inline uint32_t
tm_mesh_pld_target_id(p_tm_pld_t pld)
{
    return pld->slv_id;
}

inline uint32_t
tm_mesh_pld_src_node(p_tm_pld_t pld)
{
    return static_cast<uint32_t>(pld->mst_addr);
}

inline uint32_t
tm_mesh_pld_dst_node(p_tm_pld_t pld)
{
    return static_cast<uint32_t>(pld->slv_addr);
}

#endif  // _TM_MESH_PLD_H_
