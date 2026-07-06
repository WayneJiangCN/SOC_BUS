# 拓扑结构说明

## 1. 文档范围

本文档说明 `TmBusFabric` 的拓扑建模方式，重点覆盖：

- `master -> fabric -> target` 的单跳结构
- 地址解码
- `default target` 回退规则
- `interleave` 的可配置策略

本文档不讨论仲裁、流控和响应时序，这些内容分别见：

- `arbitration.md`
- `flow_control.md`
- `transactions.md`

## 2. 拓扑模型

`TmBusFabric` 在系统视角上是一个单跳事务级互连：

```text
PemBiu[0..N-1] -> TmBusFabric -> Target[0..M-1]
```

它不是“整机只有一根共享总线”的抽象，而是更接近 gem5 `XBar` 的 `per-target` 竞争模型：

- 去不同 `target` 的流量可以并行推进
- 多个 `master` 访问同一个 `target` 时才发生局部竞争
- 地址路由和事务仲裁解耦，便于后续分别演进

## 3. Master 与 Target 视图

### 3.1 Master

每个上游 `PemBiu` 对应一个 `master port`。拓扑层维护两张映射表：

- `port_id -> master_id`
- `master_id -> port_id`

这样请求发出时可以补全 `mst_id`，响应返回时也能根据 `mst_id` 找回原始上游端口。

### 3.2 Target

每个下游端口抽象成一个 `target`，可以代表：

- 一个 `L2 slice`
- 一个 `DDR channel`
- 一个 `MMIO/CFG` 端口
- 一个 ESL 用例里的合成存储端点

拓扑层只回答一个问题：`这个地址应该送到哪一个 target`

## 4. 地址解码规则

V1 采用单级地址解码：

1. 先扫描显式配置的 `target`
2. 按 `contains(addr)` 判断地址是否落在该地址域
3. 如果多个 `target` 共享同一地址域，再交给该 `target` 的 `interleave` 策略判断
4. 如果都不命中，则回退到 `default target`

这套规则和 gem5 `XBar` 的思路一致：先做地址域划分，再在局部范围内决定目标端口。

## 5. Interleave 配置模型

### 5.1 配置字段

`TmBusTargetCfg` 当前支持以下 `interleave` 相关字段：

- `interleave_type`
- `interleave_size`
- `interleave_num`
- `interleave_idx`
- `interleave_hash_shift`
- `interleave_hash_seed`

含义如下：

- `interleave_type`：选择哪一种地址分片算法
- `interleave_size`：条带大小，单位为字节
- `interleave_num`：总共有多少个 slice
- `interleave_idx`：当前 `target` 是第几片
- `interleave_hash_shift`：`XOR_HASH` 模式下的折叠位移
- `interleave_hash_seed`：`XOR_HASH` 模式下的种子

当 `interleave_num <= 1` 或 `interleave_size == 0` 时，视为关闭 `interleave`，只按地址范围命中。

对于共享同一段地址空间的一组 `target`，建议保持以下字段一致：

- `interleave_type`
- `interleave_size`
- `interleave_num`
- `interleave_hash_shift`
- `interleave_hash_seed`

通常只让 `interleave_idx` 在同组 slice 之间变化。

### 5.2 当前支持的策略

当前支持两类策略：

- `LINEAR`
- `XOR_HASH`

两者都通过独立策略类实现，代码位于：

- `aicore/tm_bus_interleave.h`
- `aicore/tm_bus_interleave.cc`

## 6. 线性条带策略

`LINEAR` 是最接近传统 `L2 slice / DDR channel striping` 的规则。

计算过程如下：

```text
stripe_id = (addr - addr_begin) / interleave_size
slice_id  = stripe_id % interleave_num
```

当 `slice_id == interleave_idx` 时，说明该地址命中当前 `target`。

适用场景：

- 规则简单，易于解释
- 更适合作为 bring-up 和基线配置
- 便于和已有 SoC 地址表一一对应

## 7. XOR Hash 策略

`XOR_HASH` 用轻量散列替代纯线性条带，避免某些连续地址模式总是压到固定 slice。

计算过程如下：

```text
stripe_id = (addr - addr_begin) / interleave_size
hashed    = stripe_id ^ (stripe_id >> interleave_hash_shift) ^ interleave_hash_seed
slice_id  = hashed % interleave_num
```

适用场景：

- 地址流量具有明显连续性
- 希望弱化热点偏斜
- 需要做比线性条带更“均匀”的低精度近似

这不是完整 NoC 路由算法，而是事务级 `slice` 选择策略，仍然保持 V1 的轻量建模边界。

## 8. 策略类设计

`interleave` 没有继续写死在 `TmBusTargetCfg::matches()` 里，而是拆成了可扩展策略类：

- `TmBusInterleave`
- `TmBusLinearInterleave`
- `TmBusXorHashInterleave`

`TmBusTopology` 在 `config()` 阶段为每个 `target` 建立一份策略对象，`decode_target()` 只负责：

1. 找到候选地址域
2. 调用对应策略的 `matches()`
3. 返回命中的 `target`

这样做的好处是：

- 拓扑逻辑和算法逻辑分离
- 新增一种 `hash` 规则时，不需要改 `decode_target()` 主流程
- 更符合你们当前“按职责分文件”的代码风格

## 9. 后续扩展方式

如果后面还要增加新的规则，例如：

- `ROTATE_XOR_HASH`
- `PAGE_HASH`
- `BANK_GROUP_HASH`

建议按下面的方式扩展：

1. 在 `tm_bus_types.h` 中新增 `tm_bus_interleave_type_t` 枚举值
2. 在 `tm_bus_interleave.h/.cc` 中新增一个策略子类
3. 在 `tm_make_bus_interleave()` 工厂函数中注册该类型
4. 只在文档中补充新规则说明，不改 `TmBusTopology::decode_target()` 主流程

## 10. 代码映射

- 拓扑入口：`aicore/tm_bus_topology.h`
- 拓扑实现：`aicore/tm_bus_topology.cc`
- 策略接口与子类：`aicore/tm_bus_interleave.h`
- 策略实现：`aicore/tm_bus_interleave.cc`
- 配置定义：`aicore/tm_bus_types.h`
