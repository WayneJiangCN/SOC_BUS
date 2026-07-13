#include "tm_mesh_link.h"

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

    ready_packets_ = tm_make_que<Transit>(clk_, name_ + "_ready_packets",
                                          latency_+1, latency_);
    tm_sensitive(TM_MAKE_CPROC(&TmMeshLink::drain_ready_packets),
                 ready_packets_->vld);

    reset();
}

void
TmMeshLink::reset()
{
    next_send_time_ = 0;
    ready_packets_->clear();
}

bool
TmMeshLink::idle() const
{
    return ready_packets_->empty();
}

bool
TmMeshLink::can_send(tm_time_t now) const
{
    return now >= next_send_time_;
}

void
TmMeshLink::enqueue(p_tm_pld_t pld, uint32_t traffic_class, tm_time_t now)
{
    Transit transit;
    transit.pld = pld;
    transit.traffic_class = traffic_class;
    ready_packets_->push_back(transit);
    next_send_time_ = now + 1;
}

void
TmMeshLink::attach(dst_fifo_lookup_t dst_fifo_lookup)
{
    dst_fifo_lookup_ = std::move(dst_fifo_lookup);
}

void
TmMeshLink::drain_ready_packets()
{
    while (ready_packets_->valid() && !ready_packets_->empty()) {
        auto transit = ready_packets_->front();
        if (transit.pld == nullptr) {
            ready_packets_->pop_front();
            continue;
        }

        auto dst_fifo = dst_fifo_lookup_(
            dst_router_, dst_dir_, transit.traffic_class, transit.pld);
        if (dst_fifo == nullptr) {
            ready_packets_->pop_front();
            continue;
        }
        if (dst_fifo->full()) {
            break;
        }

        dst_fifo->push_back(transit.pld);
        ready_packets_->pop_front();
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
