# 流量控制说明

## 1. 文档范围

本文档定义 `TmBusFabric` 如何建模流控，重点覆盖：

- slot credit
- bandwidth token
- outstanding depth
- busy time
- hotspot penalty

## 2. 基本原则

当前 fabric 使用的是 **transaction-level flow control**，而不是 flit-level credit loop。

它更接近：

- gem5 `XBar` 的 timing occupancy 思路
- `TmMem` 现有的 credit / bandwidth 语义

而不是：

- BookSim 风格的 router input/output credit

## 3. 资源类型

### 3.1 Slot Credit

slot credit 表示 target 还可以接收多少在途事务。

当前模型区分三类限制：

- aggregate transaction 数量
- read transaction 数量
- write transaction 数量

这样 target 可以同时表达：

1. 总体占用压力
2. 读路径压力
3. 写路径压力

### 3.2 Bandwidth Token

bandwidth token 表示单位时间内可持续消耗的字节带宽额度。

当前分别维护：

- aggregate token
- read token
- write token

这些 token 按周期恢复，而不是每个字节都建模成 flit 级传输事件。

## 4. 资源消耗规则

### 4.1 读请求

`RD_REQ` 会消耗：

- aggregate slot credit
- read slot credit
- aggregate bandwidth token
- read bandwidth token

### 4.2 写命令

`WR_REQ` 会消耗：

- aggregate slot credit
- write slot credit

它**不会**立即消耗写带宽 token，因为真正的数据字节发生在 `WR_DAT` 阶段。

### 4.3 写数据

`WR_DAT` 会消耗：

- aggregate bandwidth token
- write bandwidth token

它不会重新申请新的写 slot，而是复用 `WR_REQ` 阶段已经占住的写事务槽位。

## 5. 资源释放规则

### 5.1 读事务释放

读 slot 在读响应真正开始回送到 master 时释放。

对 V1 来说，这是一个合理的 transaction-level 近似，因为此时 target 已经开始真正完成这笔读事务。

### 5.2 写事务释放

写 slot 只在 `WR_DAT_RSP` 时释放。

原因是：

- 仅收到 `WR_REQ_RSP` 只表示写命令阶段结束
- 并不表示写数据已经完成

只有把 slot 生命周期绑定到 `WR_DAT_RSP`，写事务建模才完整。

## 6. Busy Time

当前 fabric 通过以下几个量统一折算 target 侧 busy time：

- frontend latency
- forward latency
- response latency
- header latency
- `size / width` 对应的 payload cycles
- hotspot penalty

这部分是当前实现里最接近 gem5 `XBar` 的时延抽象。

## 7. Outstanding Depth

每个 target 都维护 outstanding depth。

它的作用包括：

- 粗粒度拥塞信号
- hotspot penalty 输入
- 调试和性能统计指标

这刻意保持为粗粒度近似，不扩展成完整 NoC 排队网络。

## 8. 当前代码映射

- `aicore/tm_bus_flow_ctrl.h`
- `aicore/tm_bus_flow_ctrl.cc`
- `aicore/tm_bus_types.h`
