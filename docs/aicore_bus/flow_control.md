# 流量控制说明

## 1. 文档范围

本文档描述当前 ring 版本 `TmBusFabric` 的流控方式。

当前流控不是单一机制，而是几层叠加：

- FIFO 反压
- target credit
- bandwidth token
- issue/rsp busy time
- ring hop latency

## 2. 当前版本的核心判断

ring 版本里，一个事务能不能继续前进，取决于两个层面：

1. 网络层能不能走
2. 目标端点能不能收

网络层主要看：

- 下一个 ring FIFO 是否有空间
- 当前 node 到下一跳的 hop 时间是否已满足

目标端点主要看：

- target credit 是否够
- bandwidth token 是否够
- target 当前是否处于 busy 时间窗内

## 3. 这不是 Ruby/Garnet 式 credit

当前 ring 版本很重要的一点是：

**它已经有 ring 拓扑，但流控仍然是 target 级为主。**

也就是说，当前 `flow_ctrl` 负责的是：

- 终点 target 的资源准入
- 事务级 outstanding 控制
- 端点带宽限制

而不是：

- router input VC credit
- hop-by-hop flit buffer credit

所以它和 Ruby/Garnet 的区别是：

- 当前版本回答“目标端点还能不能收”
- Ruby/Garnet 回答“下一跳 buffer 还有没有空位能收 flit”

## 4. FIFO 反压

ring 版本里，FIFO 反压已经分布到多层：

- master ingress FIFO
- ring request FIFO
- target local FIFO
- ring response FIFO
- master response FIFO
- per-master grant FIFO

只要下一级 FIFO 满，上一级就不会继续前推。

这意味着当前 ring 已经具备了比较直观的 backpressure 链条。

## 5. Target Credit

当前 target 级 credit 主要包括：

- `acc_slot_credit`
- `rd_slot_credit`
- `wr_slot_credit`

其含义是：

- `acc_slot_credit` 约束总 outstanding 数
- `rd_slot_credit` 约束读事务 outstanding 数
- `wr_slot_credit` 约束写事务 outstanding 数

当前 credit 的粒度是事务级，而不是 flit 级。

## 6. Bandwidth Token

除了 slot credit 以外，当前还维护带宽 token：

- `acc_bw_token`
- `rd_bw_token`
- `wr_bw_token`

它们用于表达：

- 单位时间内目标端点可消耗的总带宽
- 读带宽预算
- 写带宽预算

这些 token 按周期恢复，而不是在 ring 上逐 hop 回传。

## 7. 不同请求的资源消耗

当前资源消耗规则仍然保留旧事务总线时期的语义：

### 7.1 `RD_REQ`

消耗：

- `acc_slot_credit`
- `rd_slot_credit`
- `acc_bw_token`
- `rd_bw_token`

### 7.2 `WR_REQ`

消耗：

- `acc_slot_credit`
- `wr_slot_credit`

它不立即消耗写带宽，因为真正数据还没发。

### 7.3 `WR_DAT`

消耗：

- `acc_bw_token`
- `wr_bw_token`

它复用 `WR_REQ` 已经占住的写事务槽位，不再重复申请写 slot。

## 8. Credit 释放时机

### 8.1 读事务

读事务在响应真正回送到 master 的过程中释放 read slot。

### 8.2 写事务

写事务只在 `WR_DAT_RSP` 完成时释放 write slot。

原因是：

- `WR_REQ_RSP` 只表示写请求阶段被 target 接受
- 并不表示整个写事务已经完成

因此写事务的生命周期必须绑定到 `WR_DAT_RSP`。

## 9. Busy Time

当前版本还对 target 端口占用时间做了显式建模。

主要通过以下量计算：

- `frontend_latency`
- `forward_latency`
- `response_latency`
- `header_latency`
- `width` 与 `size` 推导出的 payload 周期
- `hotspot_penalty`

这部分仍然保持了 gem5 `XBar` 风格的粗粒度时延建模思路。

## 10. Ring Hop Latency

ring 版本新增了一层网络级延迟：

- `ring_link_latency`

每次从一个 ring node 前推到下一跳时，都会受这个 hop 延迟约束。

当前实现方式是：

- 每个 node、每个 traffic class 维护一个“下一次可前推时间”
- 未到时刻时，该 node 本拍不能继续把对应包送往下一跳

这让 ring 版本具备了最基础的逐跳传播时延。

## 11. Outstanding

当前 `target_outstanding` 仍然是 target 级统计量。

它的用途包括：

- 表达 target 压力
- 驱动 `hotspot_penalty`
- 作为性能统计基础

注意：

这不是网络中每跳 buffer occupancy 的完整替代物。

## 12. 当前模型边界

当前流控最重要的边界是：

- 已经有 ring 拓扑
- 但尚未进入 Ruby/Garnet 式 hop-by-hop credit 模型

因此可以把当前版本理解成：

```text
ring 网络拓扑 + target 级事务流控
```

而不是完整网络流控。
