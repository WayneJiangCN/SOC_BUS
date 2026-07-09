#ifndef _TM_MESH_LINK_H_
#define _TM_MESH_LINK_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>

#include "tm_engine.h"
#include "tm_mesh_types.h"

/*
 * TmMeshLink
 *
 * router 之间的一条有向物理链路：
 *   Router(src).src_dir -> Router(dst).dst_dir
 *
 * 当前抽象是“每拍最多发送一个单位”的链路模型：
 * - 源输出口每拍最多送一个包进入链路
 * - 包经过 latency_ 拍后，才会在目标输入口可见
 * - 链路内部只记录在途包和下一次允许发送的时间
 */
class TmMeshLink
{
  public:
    struct Transit
    {
        /* 正在链路中飞行的包。 */
        p_tm_pld_t pld = nullptr;
        /* 保留 traffic class，便于到达目标口后灌回正确队列。 */
        uint32_t traffic_class = 0;
        /* 到达目标输入口的时间。 */
        tm_engine::tm_time_t ready_time = 0;
    };

    TmMeshLink();
    TmMeshLink(const std::string& name, uint32_t latency, uint32_t src_router,
               TmMeshPortDir src_dir, uint32_t dst_router,
               TmMeshPortDir dst_dir);
    ~TmMeshLink();

    void config(const std::string& name, uint32_t latency, uint32_t src_router,
                TmMeshPortDir src_dir, uint32_t dst_router,
                TmMeshPortDir dst_dir);
    void reset();
    bool idle() const;

    /* 下一次允许源输出口发包进链路的时间。 */
    tm_engine::tm_time_t& next_send_time();
    bool can_send(tm_engine::tm_time_t now) const;
    /* 在当前拍将一个包送进链路。 */
    void enqueue(p_tm_pld_t pld, uint32_t traffic_class,
                 tm_engine::tm_time_t now);
    /* 查看是否已有包在本拍到达目标端。 */
    const Transit* peek_ready_packet(tm_engine::tm_time_t now) const;
    void pop_ready_packet();
    uint32_t latency() const;
    uint32_t src_router() const;
    uint32_t dst_router() const;
    TmMeshPortDir src_dir() const;
    TmMeshPortDir dst_dir() const;

  private:
    std::string name_;
    uint32_t latency_ = 1;
    uint32_t src_router_ = 0;
    uint32_t dst_router_ = 0;
    TmMeshPortDir src_dir_ = TmMeshPortDir::LOCAL;
    TmMeshPortDir dst_dir_ = TmMeshPortDir::LOCAL;
    /* 一条链路每拍只能发一个单位，因此这里只需要一个发送节流时间。 */
    tm_engine::tm_time_t next_send_time_ = 0;
    /* 链路里的在途包队列。 */
    std::deque<Transit> inflight_packets_;
};

using tm_mesh_link_t = TmMeshLink;
using p_tm_mesh_link_t = std::shared_ptr<tm_mesh_link_t>;

inline p_tm_mesh_link_t
tm_make_mesh_link(const std::string& name, uint32_t latency, uint32_t src_router,
                  TmMeshPortDir src_dir, uint32_t dst_router,
                  TmMeshPortDir dst_dir)
{
    return std::make_shared<TmMeshLink>(name, latency, src_router, src_dir,
                                        dst_router, dst_dir);
}

#endif  // _TM_MESH_LINK_H_
