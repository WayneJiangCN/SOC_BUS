#include "tm_bus_interleave.h"

/* interleave 只解决共享地址域如何切片，不直接负责完整地址解码。 */

bool
TmBusInterleave::matches(uint64_t addr, p_tm_bus_target_cfg_t cfg) const
{
    if (!cfg->contains(addr)) {
        return false;
    }
    if (!cfg->interleave_enabled()) {
        return true;
    }
    return calc_slice(addr, cfg) == cfg->interleave_idx;
}

uint64_t
TmBusInterleave::calc_stripe_id(uint64_t addr, p_tm_bus_target_cfg_t cfg) const
{
    return (addr - cfg->addr_begin) / cfg->interleave_size;
}

uint32_t
TmBusLinearInterleave::calc_slice(uint64_t addr,
                                  p_tm_bus_target_cfg_t cfg) const
{
    /* 线性条带：stripe_id 轮转到不同 slice。 */
    uint64_t stripe_id = calc_stripe_id(addr, cfg);
    return static_cast<uint32_t>(stripe_id % cfg->interleave_num);
}

uint32_t
TmBusXorHashInterleave::calc_slice(uint64_t addr,
                                   p_tm_bus_target_cfg_t cfg) const
{
    /* XOR hash：把连续地址打散，减轻热点固定压到同一片。 */
    uint64_t stripe_id = calc_stripe_id(addr, cfg);
    uint64_t hashed = stripe_id ^
                      (stripe_id >> cfg->interleave_hash_shift) ^
                      cfg->interleave_hash_seed;
    return static_cast<uint32_t>(hashed % cfg->interleave_num);
}

p_tm_bus_interleave_t
tm_make_bus_interleave(tm_bus_interleave_type_t type)
{
    /* 工厂函数：根据配置构造具体的 interleave 策略对象。 */
    switch (type) {
      case tm_bus_interleave_type_t::XOR_HASH:
        return std::make_shared<TmBusXorHashInterleave>();
      case tm_bus_interleave_type_t::LINEAR:
      default:
        return std::make_shared<TmBusLinearInterleave>();
    }
}
