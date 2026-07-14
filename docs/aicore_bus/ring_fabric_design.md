# Fabric 设计

## 顶层定位

`TmRingFabric` 不是一个“大而全”的 bus 对象，而是整张 ring-lite 网络的统一调度器。它自己不再保存每个 router 的内部 FIFO，而是把节点内状态下放到 `TmRingRouter`，把边上的时延状态下放到 `TmRingLink`。

## Fabric 负责什么

- 持有 `master_nius_`
- 持有 `routers_`
- 持有 `links_`
- 持有 `target_ports_`
- 持有 `topology_`
- 持有 `flow_ctrl_`
- 持有共享事务表 `txn_ctx_`

## Fabric 不负责什么

- 不直接管理 router 内部请求/响应队列
- 不直接做 bus 风格全局仲裁
- 不做 flit、VC、credit 级调度

## 共享事务表

`txn_ctx_` 是 fabric 里最重要的共享状态。它记录：

- 请求来自哪个 master
- 目标 target 是谁
- source node 和 destination node
- 当前事务处于哪个阶段
- 响应是否已经收齐
- target 侧 slot 是否已经释放

它的作用不是替代包本身，而是把包之外的生命周期状态集中到一张表里，方便：

- 写事务两阶段配对
- 回包归属判断
- 目标侧 credit 释放
- 完成态查询

## 关键函数边界

- `recv_master_reqs()`
  从 `bus_inf_` 吸收上游请求到 NIU 本地 pending。
- `inject_ring_reqs()`
  把 NIU 本地 pending 请求送到 source router 的 `LOCAL` 输入口。
- `advance_ring_routers()`
  推进整张网络内部的 router、output port 和 link。
- `send_target_reqs()`
  把目标 router 已经送到 target 本地队列的包真正发给 target。
- `recv_target_rsps()`
  从 target 接口收回响应，再注入目标 router。

## 当前精度边界

当前模型保留了：

- 多跳路径差异
- 端口级竞争
- 链路逐拍发射
- target 端瓶颈
- 读/写/回包之间的共享竞争

当前模型没有做：

- flit 拆分
- VC
- credit return
- 细粒度 crossbar pipeline

这使它更适合 SoC 级性能趋势建模，而不是 RTL 级互连复刻。
