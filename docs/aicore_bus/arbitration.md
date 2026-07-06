# 仲裁说明

## 1. 文档范围

本文档定义 `TmBusFabric` 的仲裁方式，主要覆盖：

- 仲裁粒度
- 仲裁状态组织
- 当前 round-robin 行为
- 后续更强仲裁器的扩展点

## 2. 基本设计选择

当前 fabric 采用 **per-target**、**per-traffic-class** 的仲裁方式。

这也是借鉴 gem5 `XBar` 的关键点。

换句话说，fabric 不是“一次只允许系统里一个请求前进”，而是：

- 每个 target 有自己的仲裁域
- 每类事务有自己的前进节奏

## 3. 仲裁域划分

当前仲裁同时按以下两个维度拆开：

1. `target_id`
2. 请求类型

请求类型包括：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`

这意味着每个 target 会分别维护：

- 读命令的仲裁状态
- 写命令的仲裁状态
- 写数据的仲裁状态

## 4. 当前仲裁模块

仲裁器已经被抽成独立模块：

- `aicore/tm_bus_arbiter.h`
- `aicore/tm_bus_arbiter.cc`

这样 `TmBusFabric` 主类可以更专注于：

- 队列搬移
- 事务状态机
- 响应闭环

而不必继续把仲裁状态和仲裁策略都堆在主类内部。

## 5. 当前策略

默认策略是 target-local round-robin。

仲裁器为以下每种组合维护一个轮转指针：

- target
- 请求类型

它的优点是：

1. 简单
2. 易调试
3. 可解释性强
4. 适合 V1 bring-up

## 6. 从 LOCAL_XBAR 借来的点

GPGPU-Sim `LOCAL_XBAR` 对当前仲裁层的影响主要有两点：

1. 把仲裁器视为可替换的独立策略块
2. 预留更强输出侧 grant 状态的扩展点

当前代码里已经预留：

- `tm_bus_arbiter_type_t`
- `RR`
- `ISLIP_LIKE`

其中 `ISLIP_LIKE` 当前仍是 V1 占位实现：

- 不实现完整 request / grant / accept 多轮握手
- 只是保留“按输出维度维护 grant 状态”的结构扩展点

这样能在保持 ESL 简洁性的前提下，为后续增强仲裁器留接口。

## 7. 仲裁资格条件

一个 master 只有在满足以下条件时，才有资格参与某个 target 的仲裁：

1. 对应 master FIFO 非空
2. 队头事务解码后确实要去这个 target
3. 如果是 `WR_DAT`，则 grant 队列头存在且匹配

仲裁阶段本身不检查：

- credit
- bandwidth token
- target busy time

这些限制是在后续 `send_target_req()` 阶段检查的。

这样可以把“选谁先上车”和“现在能不能开车”分开表达。

## 8. 当前代码映射

- `aicore/tm_bus_arbiter.h`
- `aicore/tm_bus_arbiter.cc`
- `aicore/tm_bus_req.cc`
- `aicore/tm_bus_types.h`
