# 仲裁说明

## 1. 文档范围

本文档说明当前 mesh 主线里的仲裁逻辑放在哪里，以及 `tm_bus_arbiter` 为什么已经不在主路径上。

## 2. 当前 mesh 主路径的仲裁位置

当前主路径已经不是早期“大 fabric 扫描一堆 FIFO”的模式，而是：

```text
NIU
  -> Router local queues
  -> Router per-output RR
  -> Link shared throttle
  -> 下一跳 Router / TargetPort / NIU
```

因此当前真正生效的仲裁点在：

- `TmMeshRouter::pick_output_winner()`
- `TmMeshLink::next_ready_time()`

前者负责：

- 同一个 router 节点里
- 多个 traffic class 同时请求同一个输出口时
- 选择这拍谁先走

后者负责：

- 这一拍链路是否可用
- 输出资源是否需要节流

## 3. 仲裁粒度

当前是粗粒度 message/transaction 级仲裁，不是 Garnet 那种 flit/VC 级仲裁。

当前粒度大致是：

- request subnet：`RD_REQ + WR_REQ`
- data subnet：`WR_DAT`
- response subnet：
  - `RD_RSP`
  - `WR_REQ_RSP`
  - `WR_DAT_RSP`

Router 会从这些本地 queue 里收集候选，再按输出口做 RR 选择。

## 4. 为什么不再用 fabric 级 `arbiter_`

早期实现里，fabric 顶层保留过：

- `TmBusArbiter arbiter_`

但当前 mesh 主路径已经完成精简：

- fabric 不再做单独的中心式仲裁
- router 自己负责本节点的输出选择
- link 自己负责共享节流

因此 fabric 级 `arbiter_` 已经删除，不再是主路径依赖。

## 5. `tm_bus_arbiter` 当前定位

`tm_bus_arbiter.h/.cc` 仍然保留在工程中，但当前定位已经变成：

- 历史遗留的公共模块
- 未来如果要做更复杂 router arbitration 的扩展点

它当前不是 mesh 主路径核心。

## 6. 当前不是在做什么

当前 mesh 主线没有做：

- InputUnit / OutputUnit
- SwitchAllocator
- CrossbarSwitch
- credit-based arbitration
- flit/VC 级调度

这些都是更接近 Garnet 的 NoC 微结构，不属于当前 SoC 级轻量 mesh-lite 模型的目标范围。

## 7. 当前仲裁模型的一句话总结

当前 mesh 仲裁可以概括成：

```text
Router 负责本地 per-output RR
Link 负责共享输出节流
TargetPort 负责目标端口准入
```

这套机制足够支撑：

- 多 core 并发
- 多 target / 多 channel
- 交织与热点瓶颈分析
- SoC 级 CA/ESL 用例
