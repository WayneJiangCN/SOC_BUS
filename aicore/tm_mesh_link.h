#ifndef _TM_MESH_LINK_H_
#define _TM_MESH_LINK_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "tm_engine.h"

/*
 * TmMeshLink
 *
 * Coarse directed physical link:
 *   Router(src) -> Router(dst)
 *
 * The link keeps one shared next-ready timestamp so request, data, and
 * response traffic compete for the same output resource.
 */
class TmMeshLink
{
  public:
    TmMeshLink();
    TmMeshLink(const std::string& name, uint32_t latency);
    ~TmMeshLink();

    void config(const std::string& name, uint32_t latency);
    void reset();

    tm_engine::tm_time_t& next_ready_time();
    uint32_t latency() const;

  private:
    std::string name_;
    uint32_t latency_ = 1;
    tm_engine::tm_time_t next_ready_time_ = 0;
};

using tm_mesh_link_t = TmMeshLink;
using p_tm_mesh_link_t = std::shared_ptr<tm_mesh_link_t>;

inline p_tm_mesh_link_t
tm_make_mesh_link(const std::string& name, uint32_t latency)
{
    return std::make_shared<TmMeshLink>(name, latency);
}

#endif  // _TM_MESH_LINK_H_
