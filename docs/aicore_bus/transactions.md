# 事务模型说明

## 1. 文档范围

本文档定义 `TmBusFabric` 的事务视角，主要覆盖：

- 请求类型
- 响应类型
- 事务 key
- 生命周期状态
- 两阶段写事务语义

## 2. 事务类别

当前 fabric 显式建模三类请求：

1. `RD_REQ`
2. `WR_REQ`
3. `WR_DAT`

这是遵循现有 `PemBiu` 协议接口，而不是强行把所有请求压成一个统一 packet。

## 3. 为什么写路径要拆开

写路径被刻意建模成两阶段事务：

1. 发出 `WR_REQ`
2. 收到 `WR_REQ_RSP / grant / DBID`
3. 发出 `WR_DAT`
4. 收到 `WR_DAT_RSP`

这样做非常重要，因为当前 AI Core 侧协议本身就是这么暴露的。

如果过早把它们合并，会失去以下建模能力：

- 命令和数据解耦
- grant 顺序约束
- 命令阶段与数据阶段的独立反压
- 正确的写 slot 生命周期

## 4. 事务标识

当前 fabric 使用：

- `mst_id`
- `gid`

作为主事务 key。

这个 key 能稳定贯穿以下阶段：

1. 上游请求进入 fabric
2. 下游 target 发回响应
3. 响应返回原始 master
4. 查询事务上下文

## 5. 事务上下文

每笔在途事务可以拥有一条上下文记录，用于描述：

- 来源 master port
- 目标 target
- 请求类型
- 当前事务状态
- 请求大小
- 发出时间
- 响应计数信息

这对以下场景尤其关键：

- 读响应被拆成多个分片
- 两阶段写事务
- 资源释放时刻控制

## 6. 生命周期状态

当前状态机包含以下状态：

- `ALLOCATED`
- `IN_INGRESS_FIFO`
- `IN_TARGET_FIFO`
- `WAIT_WR_REQ_RSP`
- `GRANT_READY`
- `WAIT_WR_DAT_RSP`
- `WAIT_RD_RSP`
- `DONE`

这些状态反映的是 **协议进度**，而不是单纯反映“在某个队列里”。

## 7. 读事务生命周期

读路径如下：

1. 上游请求进入 fabric
2. 进入 master ingress FIFO
3. 通过 per-target 仲裁进入 target FIFO
4. 发给 target
5. 一个或多个读响应返回
6. 释放读 slot
7. 事务完成回收

如果下游把一个读请求拆成多个响应分片，则上下文会维护：

- `rsp_expected`
- `rsp_seen`

只有当预期分片数全部收到后，事务才真正 retire。

## 8. 写事务生命周期

写路径如下：

1. `WR_REQ` 进入 ingress FIFO
2. `WR_REQ` 被仲裁并发往 target
3. target 返回 `WR_REQ_RSP`
4. fabric 记录 grant 信息
5. 对应的 `WR_DAT` 获得继续发送资格
6. `WR_DAT` 被仲裁并发往 target
7. target 返回 `WR_DAT_RSP`
8. 释放写 slot
9. 事务完成回收

这样设计能保证命令阶段和数据阶段被视为一个完整的生命周期，而不是两笔无关事务。

## 9. 当前代码映射

- `aicore/tm_bus_types.h`
- `aicore/tm_bus_req.cc`
- `aicore/tm_bus_rsp.cc`
