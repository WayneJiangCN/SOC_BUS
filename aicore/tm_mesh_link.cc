#include "tm_mesh_link.h"

#include <algorithm>
#include <utility>

using namespace tm_engine;

TmMeshLink::TmMeshLink()
{
}

TmMeshLink::TmMeshLink(const std::string& name, p_tm_clk_t clk,
                       uint32_t latency, uint32_t dst_router,
                       TmMeshPortDir dst_dir)
    : TmModule(name)
{
    config(name, clk, latency, dst_router, dst_dir);
}

TmMeshLink::~TmMeshLink()
{
}

void
TmMeshLink::config(const std::string& name, p_tm_clk_t clk, uint32_t latency,
                   uint32_t dst_router, TmMeshPortDir dst_dir)
{
    name_ = name;
    this->name(name_);
    clk_ = clk;
    latency_ = latency;
    dst_router_ = dst_router;
    dst_dir_ = dst_dir;

    ready_packets_.clear();
    next_send_time_.assign(tm_ring_subnet_count(), 0);

    auto drain_proc = TM_MAKE_CPROC(&TmMeshLink::drain_ready_packets);
    ready_packets_.push_back(tm_make_que<Transit>(
        clk_, name_ + "_req_ready_packets", latency_ + 1, latency_));
    ready_packets_.push_back(tm_make_que<Transit>(
        clk_, name_ + "_rsp_ready_packets", latency_ + 1, latency_));
    for (auto& q : ready_packets_) {
        tm_sensitive(drain_proc, q->vld);
    }

    reset();
}

void
TmMeshLink::reset()
{
    std::fill(next_send_time_.begin(), next_send_time_.end(), 0);
    for (auto& q : ready_packets_) {
        q->clear();
    }
}

bool
TmMeshLink::idle() const
{
    bool ret = true;
    for (auto& q : ready_packets_) {
        ret = ret && q->empty();
    }
    return ret;
}

bool
TmMeshLink::can_send(TmRingSubnet subnet, tm_time_t now) const
{
    uint32_t idx = tm_ring_subnet_index(subnet);
    return now >= next_send_time_[idx] && !ready_packets_[idx]->full();
}

void
TmMeshLink::enqueue(TmRingSubnet subnet, p_tm_pld_t pld,
                    uint32_t traffic_class, tm_time_t now)
{
    uint32_t idx = tm_ring_subnet_index(subnet);
    Transit transit;
    transit.pld = pld;
    transit.traffic_class = traffic_class;
    ready_packets_[idx]->push_back(transit);
    next_send_time_[idx] = now + 1;
}

void
TmMeshLink::attach(dst_fifo_lookup_t dst_fifo_lookup)
{
    dst_fifo_lookup_ = std::move(dst_fifo_lookup);
}

void
TmMeshLink::drain_ready_packets()
{
    for (auto& q : ready_packets_) {
        while (q->valid() && !q->empty()) {
            auto transit = q->front();
            if (transit.pld == nullptr) {
                q->pop_front();
                continue;
            }

            auto dst_fifo = dst_fifo_lookup_(
                dst_router_, dst_dir_, transit.traffic_class, transit.pld);
            if (dst_fifo == nullptr) {
                q->pop_front();
                continue;
            }
            if (dst_fifo->full()) {
                break;
            }

            dst_fifo->push_back(transit.pld);
            q->pop_front();
        }
    }
}

uint32_t
TmMeshLink::dst_router() const
{
    return dst_router_;
}

TmMeshPortDir
TmMeshLink::dst_dir() const
{
    return dst_dir_;
}
