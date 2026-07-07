#ifndef _TM_BUS_INTERLEAVE_H_
#define _TM_BUS_INTERLEAVE_H_

#include <stdint.h>

#include <memory>

#include "tm_bus_types.h"

/*
 * TmBusInterleave:
 * 在共享地址域下决定“这个地址应该落到哪一个 slice/target”。
 */
class TmBusInterleave
{
  public:
    virtual ~TmBusInterleave() = default;

    /*
     * 根据地址计算目标 interleave slice。
     */
    virtual uint32_t calc_slice(uint64_t addr,
                                p_tm_bus_target_cfg_t cfg) const = 0;

    /*
     * 统一封装“该地址是否命中当前 target”这层判断。
     */
    bool matches(uint64_t addr, p_tm_bus_target_cfg_t cfg) const;

  protected:
    uint64_t calc_stripe_id(uint64_t addr, p_tm_bus_target_cfg_t cfg) const;
};

using tm_bus_interleave_t = TmBusInterleave;
using p_tm_bus_interleave_t = std::shared_ptr<tm_bus_interleave_t>;

class TmBusLinearInterleave : public TmBusInterleave
{
  public:
    virtual uint32_t calc_slice(uint64_t addr,
                                p_tm_bus_target_cfg_t cfg) const override;
};

class TmBusXorHashInterleave : public TmBusInterleave
{
  public:
    virtual uint32_t calc_slice(uint64_t addr,
                                p_tm_bus_target_cfg_t cfg) const override;
};

p_tm_bus_interleave_t tm_make_bus_interleave(tm_bus_interleave_type_t type);

#endif  // _TM_BUS_INTERLEAVE_H_
