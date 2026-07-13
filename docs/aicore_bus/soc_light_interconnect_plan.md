# SoC 级轻量总线/Ring 模型技术方案

## 1. 目标

本方案面向一类 **SoC 级、可在 CA/ESL 环境下独立运行的多 core 互连模型**。

目标不是复刻完整 NoC 微结构，而是在较低复杂度下覆盖主要性能趋势，满足：

1. 多 core 环境适配，不依赖完整 SoC 平台也能跑 ESL 用例。
2. 能建模总线/ring 延迟、交织方案、关键瓶颈。
3. 总线延迟、交织、关键路径 OSD、带宽和 busy time 都可配。
4. 目标精度以系统级趋势为主，追求大约 80% 左右，而不是 router 微结构逐周期吻合。

## 2. 当前方案是否合适

结论是：**合适，而且已经基本够用。**

当前 `BUS/aicore` 这套代码，不宜再把目标描述成“传统 bus”；更准确的说法是：

**SoC 级轻量 interconnect / ring-lite 模型**

原因：

- 你的目标已经超过了单共享总线的能力边界。
- 你关心的是多 core、多 target/channel、交织、OSD、热点、瓶颈。
- 这些问题用轻量 interconnect 比用一根大总线更合理。

## 3. 推荐抽象层级

推荐继续沿着 **gem5 SimpleNetwork 的抽象层级** 走，而不是向 Garnet 的 flit/VC/credit 级 NoC 下沉。

原因：

- 你需要的是 message/transaction 级性能模型。
- 你要保留 AI Core 自己的协议语义。
- 你要能独立跑 ESL 用例。
- 你要控制复杂度，让模型更易配、更易校准。

不推荐当前继续做：

- flit 拆分
- VC
- credit link
- router pipeline
- 细粒度 crossbar / allocator 微结构

## 4. 推荐结构

当前推荐结构如下：

```text
Core / BIU
  -> Tm_mesh_inf
  -> TmMeshRouter
  -> TmMeshLink
  -> TmMeshRouter
  -> TmMeshTargetPort
  -> TmMem / target
```

职责划分：

- `Tm_mesh_inf`
  message-buffer endpoint / NIU，负责本地 pending、grant、completion、上下游接口。
- `TmMeshRouter`
  粗粒度 router，负责本地 message queues 和每个输出口的 RR 选择。
- `TmMeshLink`
  轻量链路，负责共享输出节流和 hop latency。
- `TmMeshTargetPort`
  target 前 ingress port，负责 target-local queues 和下游接口。
- `TmMeshFabric`
  共享调度容器，负责路由、共享事务上下文和主 tick。

## 5. 协议语义

建议保留并冻结下面这些 AI Core 事务语义：

- `RD_REQ`
- `WR_REQ`
- `WR_REQ_RSP`
- `WR_DAT`
- `WR_DAT_RSP`

关键原因：

- `WR_REQ -> WR_REQ_RSP -> WR_DAT -> WR_DAT_RSP` 是当前协议价值所在。
- 这部分如果被 NoC 微结构抽象冲散，模型会更像通用网络，反而不像你的 AI Core 系统。

## 6. 关键瓶颈如何建模

当前最值得保留的是 **target-level transaction flow control**。

也就是继续建模：

- `rd_slot_credit`
- `wr_slot_credit`
- `acc_slot_credit`
- `rd_bw_token`
- `wr_bw_token`
- `acc_bw_token`
- `busy_time`
- `hotspot_penalty`

这套更适合表达：

- 多 core 打同一个 target 的竞争
- memory channel 压力
- outstanding 上限
- 带宽上限
- 热点目标的排队和回压

## 7. 交织和地址映射

交织方案是这类模型的核心能力之一，建议明确保留：

1. `addr -> target_id`
2. `target_id -> dst_node`

这样可以把：

- 地址到 channel/target 的映射
- 网络路由

明确解耦。

建议保留可配项：

- base / size
- interleave type
- interleave size
- interleave number
- 可选 hash/shift

## 8. 当前是否过度设计

结论是：**还没有到“不能接受的过度设计”，但已经到“应该收住”的位置。**

### 8.1 该保留的

- `Tm_mesh_inf`
- `TmMeshTopology`
- `TmBusFlowCtrl`
- `txn_ctx_`
- `TmMeshTargetPort`

这些都直接支撑你的目标。

### 8.2 可以保留，但不要继续细化的

- `TmMeshRouter`
- `TmMeshLink`

保留它们是合理的，因为：

- 结构清晰
- 便于扩到多 hop、多节点
- 已经能表达 queue、仲裁、节流

但建议停在 **粗粒度 transaction/message 级**。

### 8.3 不建议再做的

- flit
- VC
- credit
- Garnet 风格 InputUnit / OutputUnit / SwitchAllocator / CrossbarSwitch 全套
- router pipeline 细节

这些会明显超过你“80% 精度、SoC 级模型”的目标。

## 9. 推荐冻结边界

建议把当前模型冻结在下面这个层级：

- endpoint：message-buffer 风格
- router：粗粒度 per-output RR 仲裁
- link：共享 throttle + hop latency
- flow control：target-level transaction gating

一句话：

**做到能表达多 core 竞争、交织分流、target 压力、热点和多跳延迟，就够了。**

## 10. 推荐参数面

### 10.1 拓扑参数

- `num_masters`
- `num_targets`

### 10.2 endpoint 参数

- `master_inf_depth`
- `target_inf_depth`
- `master_rd_req_fifo_depth`
- `master_wr_req_fifo_depth`
- `master_wr_dat_fifo_depth`
- `master_wr_grant_fifo_depth`

### 10.3 router/link 参数

- `ring_req_fifo_depth`
- `ring_rsp_fifo_depth`
- `ring_link_latency`

### 10.4 target 参数

- `rd_slot_credit_max`
- `wr_slot_credit_max`
- `acc_slot_credit_max`
- `rd_bw_token_max`
- `wr_bw_token_max`
- `acc_bw_token_max`
- `token_update_period`
- `frontend_latency`
- `forward_latency`
- `response_latency`
- `width`
- `hotspot_threshold`
- `hotspot_penalty`

## 11. 精度边界

当前方案适合覆盖：

- 多 core 扩展趋势
- 多 channel 交织收益
- 热点 target 瓶颈
- request/data/response 相互影响
- outstanding/bandwidth/busy time 造成的性能差异

当前方案不适合直接覆盖：

- flit 级拥塞传播
- VC 竞争
- credit deadlock
- crossbar 微结构时序
- router pipeline 周期级细节

因此这里说的“80% 精度”，更合理的理解是：

- 趋势对
- 热点位置对
- 多 core 扩展曲线大致对
- 主要瓶颈来源对

## 12. 最终建议

对你的目标，我建议把当前方案正式定格为：

**SoC 级轻量 interconnect / ring-lite 模型**

并且后续开发只做两类事情：

1. 补参数、统计、校准能力。
2. 优化粗粒度 router/link 行为，但不下沉到 Garnet 级细节。

这会比继续向重型 NoC 演进更符合你的项目目标。
