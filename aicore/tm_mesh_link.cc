#include "tm_mesh_link.h"

using namespace tm_engine;

TmMeshLink::TmMeshLink()
{
}

TmMeshLink::TmMeshLink(const std::string& name, uint32_t latency,
                       uint32_t src_router, TmMeshPortDir src_dir,
                       uint32_t dst_router, TmMeshPortDir dst_dir)
{
    config(name, latency, src_router, src_dir, dst_router, dst_dir);
}

TmMeshLink::~TmMeshLink()
{
}

void
TmMeshLink::config(const std::string& name, uint32_t latency,
                   uint32_t src_router, TmMeshPortDir src_dir,
                   uint32_t dst_router, TmMeshPortDir dst_dir)
{
    name_ = name;
    latency_ = latency;
    src_router_ = src_router;
    src_dir_ = src_dir;
    dst_router_ = dst_router;
    dst_dir_ = dst_dir;
    reset();
}

void
TmMeshLink::reset()
{
    next_send_time_ = 0;
    inflight_packets_.clear();
}

bool
TmMeshLink::idle() const
{
    return inflight_packets_.empty();
}

tm_time_t&
TmMeshLink::next_send_time()
{
    return next_send_time_;
}

bool
TmMeshLink::can_send(tm_time_t now) const
{
    return now >= next_send_time_;
}

void
TmMeshLink::enqueue(p_tm_pld_t pld, uint32_t traffic_class, tm_time_t now)
{
    // 包一旦进入链路，就按 now + latency_ 计算到达时间。
    Transit transit;
    transit.pld = pld;
    transit.traffic_class = traffic_class;
    transit.ready_time = now + latency_;
    inflight_packets_.push_back(transit);
    // 当前模型里链路是“一拍一个单位”，所以下一拍才能再发。
    next_send_time_ = now + 1;
}

const TmMeshLink::Transit*
TmMeshLink::peek_ready_packet(tm_time_t now) const
{
    if (inflight_packets_.empty() || inflight_packets_.front().ready_time > now) {
        return nullptr;
    }
    return &inflight_packets_.front();
}

void
TmMeshLink::pop_ready_packet()
{
    if (!inflight_packets_.empty()) {
        inflight_packets_.pop_front();
    }
}

uint32_t
TmMeshLink::latency() const
{
    return latency_;
}

uint32_t
TmMeshLink::src_router() const
{
    return src_router_;
}

uint32_t
TmMeshLink::dst_router() const
{
    return dst_router_;
}

TmMeshPortDir
TmMeshLink::src_dir() const
{
    return src_dir_;
}

TmMeshPortDir
TmMeshLink::dst_dir() const
{
    return dst_dir_;
}
