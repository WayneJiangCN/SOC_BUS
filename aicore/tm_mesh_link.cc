#include "tm_mesh_link.h"

using namespace tm_engine;

TmMeshLink::TmMeshLink()
{
}

TmMeshLink::TmMeshLink(const std::string& name, uint32_t latency)
{
    config(name, latency);
}

TmMeshLink::~TmMeshLink()
{
}

void
TmMeshLink::config(const std::string& name, uint32_t latency)
{
    name_ = name;
    latency_ = latency;
    reset();
}

void
TmMeshLink::reset()
{
    next_ready_time_ = 0;
}

tm_time_t&
TmMeshLink::next_ready_time()
{
    return next_ready_time_;
}

uint32_t
TmMeshLink::latency() const
{
    return latency_;
}
