# 代码组织说明

## 1. 文档范围

本文档说明当前 ring 版本 `TmBusFabric` 的源码如何按职责拆分，以及每个文件在整体设计中的位置。

## 2. 当前主文件

当前 ring 版本的主要代码文件包括：

- `BUS/aicore/tm_bus_types.h`
- `BUS/aicore/tm_bus.h`
- `BUS/aicore/tm_bus_core.cc`
- `BUS/aicore/tm_bus_topology.h`
- `BUS/aicore/tm_bus_topology.cc`
- `BUS/aicore/tm_bus_interleave.h`
- `BUS/aicore/tm_bus_interleave.cc`
- `BUS/aicore/tm_bus_req.cc`
- `BUS/aicore/tm_bus_rsp.cc`
- `BUS/aicore/tm_bus_flow_ctrl.h`
- `BUS/aicore/tm_bus_flow_ctrl.cc`
- `BUS/aicore/tm_bus_arbiter.h`
- `BUS/aicore/tm_bus_arbiter.cc`

## 3. 各文件职责

### 3.1 `tm_bus_types.h`

负责集中定义：

- 配置结构
- 事务状态枚举
- interleave 类型
- grant 结构
- `TmBusTxnCtx`

ring 版本中最重要的变化在这里体现为：

- 新增 `IN_REQ_RING`
- 新增 `IN_RSP_RING`
- 新增 `src_node`
- 新增 `dst_node`
- 新增 `ring_req_fifo_depth`
- 新增 `ring_rsp_fifo_depth`
- 新增 `ring_link_latency`

### 3.2 `tm_bus.h`

负责声明 `TmBusFabric` 主类，以及所有主要成员。

ring 版本里，这里最值得关注的是：

- ring request FIFO 成员
- ring response FIFO 成员
- 每类 ring hop time 成员
- `inject_ring_req`
- `advance_ring_req_type`
- `advance_ring_rd_rsps`
- `advance_ring_wr_req_rsps`
- `advance_ring_wr_dat_rsps`

### 3.3 `tm_bus_core.cc`

负责：

- `config / build / reset / idle`
- master/target attach
- ring FIFO 创建
- 主 `tick()` 调度顺序

当前 ring 版本的 `tick()` 顺序是：

1. `update_tokens`
2. `recv_target_rsps`
3. `advance_ring_rsps`
4. `send_master_rsps`
5. `recv_master_reqs`
6. `inject_ring_reqs`
7. `advance_ring_reqs`
8. `send_target_reqs`

这已经很好地体现了“先收回程，再回送，再收新请求，再推进请求”的 ring 调度逻辑。

### 3.4 `tm_bus_topology.h/.cc`

负责：

- `master_id <-> port_id` 映射
- 地址解码
- default target
- target 选择
- ring 节点映射

ring 版本新增的重点接口是：

- `ring_node_count()`
- `master_node()`
- `target_node()`
- `next_ring_node()`

### 3.5 `tm_bus_interleave.h/.cc`

负责 interleave 算法本身。

当前 ring 版本没有改变这些算法，只改变了它们在整体系统中的位置：

- 先用 interleave 选 target
- 再把 target 映射到 ring 节点

### 3.6 `tm_bus_req.cc`

负责请求路径。

当前 ring 版本里，这个文件的职责已经变成四层：

1. 从 master inf 收包
2. 写入 master ingress FIFO
3. 从 source node 注入 request ring
4. 在 ring 上逐跳前推
5. 到达目的 target node 后进入 target FIFO
6. 最终发给 target inf

这就是 ring 版本最核心的请求主路径文件。

### 3.7 `tm_bus_rsp.cc`

负责响应路径。

当前 ring 版本里，这个文件对应：

1. 从 target inf 收响应
2. 从 target node 注入 response ring
3. 在 ring 上逐跳回传
4. 到达 source node 后进入 master response FIFO
5. 最终发回 master inf
6. 完成 credit 释放和事务回收

它和 `tm_bus_req.cc` 一起构成完整的双向事务闭环。

### 3.8 `tm_bus_flow_ctrl.h/.cc`

负责：

- target credit
- bandwidth token
- outstanding
- busy time
- hotspot penalty

这个模块在 ring 版本仍然很重要，因为当前流控还没有切到 Ruby/Garnet 式 hop-by-hop credit。

### 3.9 `tm_bus_arbiter.h/.cc`

当前仍保留在仓库中，但定位已经变化。

它现在更像：

- 兼容旧总线时期的模块拆分
- 未来 router 局部仲裁的保留扩展点

而不是当前 ring 主路径的核心依赖。

## 4. 为什么这样拆分是合理的

当前拆分方式有两个优点：

1. 它保持了你们现有 `tm_*` ESL 工程的直观性。
2. 它又把 ring 拓扑、事务路径、流控、interleave 这些主题分清了。

因此即使后面从 ring 演进到 mesh，也不需要推翻整个文件结构，只需要继续替换和细化某些局部模块。

## 5. 后续演进建议

如果后面继续做更真实的 NoC，建议优先沿着下面方向演进：

1. 把 ring node/router 抽成独立对象。
2. 把 request subnet 和 response subnet 抽成显式类。
3. 把 `tm_bus_arbiter` 挂回 router output arbitration。
4. 在 `tm_bus_flow_ctrl` 之外再增加 hop-by-hop credit 模块。

当前这套代码拆分已经为这些方向留好了空间。
