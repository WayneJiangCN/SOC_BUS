#ifndef _TM_MESH_LINK_H_
#define _TM_MESH_LINK_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

class TmMeshLink : public tm_engine::TmModule
{
  public:
    using dst_fifo_lookup_t =
        std::function<p_tm_com_que_t(uint32_t, TmMeshPortDir, uint32_t,
                                     p_tm_pld_t)>;

    struct Transit
    {
        p_tm_pld_t pld = nullptr;
        uint32_t traffic_class = 0;
    };
    using transit_queue_t = TmQue<Transit>;
    using p_transit_queue_t = std::shared_ptr<transit_queue_t>;

    TmMeshLink();
    TmMeshLink(const std::string& name, tm_engine::p_tm_clk_t clk,
               uint32_t latency, uint32_t dst_router, TmMeshPortDir dst_dir);
    ~TmMeshLink();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                uint32_t latency, uint32_t dst_router, TmMeshPortDir dst_dir);
    void reset();
    bool idle() const;

    bool can_send(tm_engine::tm_time_t now) const;
    void enqueue(p_tm_pld_t pld, uint32_t traffic_class,
                 tm_engine::tm_time_t now);
    void attach(dst_fifo_lookup_t dst_fifo_lookup);
    uint32_t dst_router() const;
    TmMeshPortDir dst_dir() const;

  private:
    void drain_ready_packets();

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    uint32_t latency_ = 1;
    uint32_t dst_router_ = 0;
    TmMeshPortDir dst_dir_ = TmMeshPortDir::LOCAL;
    tm_engine::tm_time_t next_send_time_ = 0;
    p_transit_queue_t ready_packets_ = nullptr;
    dst_fifo_lookup_t dst_fifo_lookup_;
};

using tm_mesh_link_t = TmMeshLink;
using p_tm_mesh_link_t = std::shared_ptr<tm_mesh_link_t>;

inline p_tm_mesh_link_t
tm_make_mesh_link(const std::string& name, tm_engine::p_tm_clk_t clk,
                  uint32_t latency, uint32_t dst_router, TmMeshPortDir dst_dir)
{
    return std::make_shared<TmMeshLink>(name, clk, latency, dst_router,
                                        dst_dir);
}

#endif  // _TM_MESH_LINK_H_
