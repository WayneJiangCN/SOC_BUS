# 代码组织说明

## 1. 范围

本文档说明当前 `BUS/aicore/` 下 mesh 主线代码的职责划分。

当前主线结构是：

- `Tm_mesh_inf`
- `TmMeshRouter`
- `TmMeshLink`
- `TmMeshTargetPort`
- `TmMeshFabric`
- `TmMeshTopology`
- `TmBusFlowCtrl`

这条主线已经完成一轮精简：

- 不再使用 fabric 级 `arbiter_`
- 不再保留旧的 `advance_mesh_reqs()` / `advance_mesh_rsps()` 兼容壳
- 不再保留按 request/data/response 拆开的旧 hop-ready 接口

## 2. 核心文件

### 2.1 类型与拓扑

- `tm_mesh_types.h`
  - mesh 配置结构
  - `TmMeshTxnCtx`
  - `TmMeshGrant`
  - mesh 事务状态
- `tm_mesh_topology.h/.cc`
  - `rows / cols / router_count`
  - `master_node(master_port)`
  - `target_node(target_id)`
  - `compute_next_node()`
  - `decode_target()`
  - `bind_master_id()`

### 2.2 左侧 endpoint

- `tm_mesh_inf.h/.cc`
  - master-side NIU
  - `bus_inf_`
  - `req_pending_q_`
  - `wr_dat_pending_q_`
  - `wr_grant_fifo_`
  - `bus_req_list_ / api_req_map_`

### 2.3 中间网络节点

- `tm_mesh_router.h/.cc`
  - 每个 router 自己持有本地 message queues
  - request queue
  - write-data queue
  - read-response queues
  - write-response queues
  - per-output RR 仲裁状态

- `tm_mesh_link.h/.cc`
  - 相邻 router 之间的有向物理链路
  - `latency`
  - 共享 `next_ready_time()`

### 2.4 右侧 endpoint

- `tm_mesh_target_port.h/.cc`
  - target-side ingress port
  - target-local request queues
  - 下游 `inf()` 接口

### 2.5 顶层共享容器

- `tm_mesh.h`
  - `TmMeshFabric` 类声明
  - 成员组织与 helper 声明

- `tm_mesh_core.cc`
  - `config / reset / idle / tick`
  - `attach_master / attach_target`
  - API 风格 `send_rd_req / send_wr_req`
  - link 创建和 helper

- `tm_mesh_req.cc`
  - 从 NIU 吸收请求
  - request/data 注入
  - 统一的 `advance_mesh_routers()`
  - Router -> Link -> Router 前推
  - 到达 target 后进入 `TargetPort` local queue

- `tm_mesh_rsp.cc`
  - 从 `TargetPort` 收响应
  - 将 response 注入目标 router 的 response queues

## 3. 各层职责

### 3.1 `Tm_mesh_inf`

只负责端点本地行为：

- 上游接口
- 本地 pending
- grant
- completion

它不负责：

- 多跳路由
- mesh 内部 queue
- target flow control

### 3.2 `TmMeshRouter`

负责本节点的粗粒度 message-level router 行为：

- 持有本地 request/data/response queues
- 为同一输出口上的多个候选流量做 RR 选择

它不负责：

- flit
- VC
- credit
- router pipeline

### 3.3 `TmMeshLink`

只负责相邻节点之间的 hop 资源：

- hop latency
- 共享输出节流时间

它不负责：

- queue
- credit
- flit

### 3.4 `TmMeshTargetPort`

只负责 target 侧 endpoint：

- 接住 Router 到达目标节点后的 request
- 再转交给 target / TmMem
- 把 target 响应交回 Fabric

### 3.5 `TmMeshFabric`

是共享容器和调度骨架，负责：

- 持有 NIU / Router / Link / TargetPort
- 维护共享 `txn_ctx_`
- 安排 tick 顺序
- 调用 topology / flow control

## 4. tick 主顺序

当前 `TmMeshFabric::tick()` 的主顺序是：

1. `NIU.tick()`
2. `flow_ctrl_.update_tokens()`
3. `recv_target_rsps()`
4. `recv_master_reqs()`
5. `inject_mesh_reqs()`
6. `advance_mesh_routers()`
7. `send_target_reqs()`

这里：

- 先收 target 回包
- 再吸收新的 master 请求
- 再统一推进 router / link / target path

## 5. request / data / response 的落点

- request/data 前向路径：`tm_mesh_req.cc`
- response 注入路径：`tm_mesh_rsp.cc`
- endpoint 本地收发：`tm_mesh_inf.cc`
- target 侧端口：`tm_mesh_target_port.cc`
- router 本地 queues 与仲裁：`tm_mesh_router.cc`
- link hop 状态：`tm_mesh_link.cc`

## 6. 仍在复用的公共模块

- `tm_bus_flow_ctrl.h/.cc`
  - 当前 mesh 继续复用它做 target-level flow control

- `tm_bus_interleave.h/.cc`
  - 地址到 target/channel 的分流逻辑

`tm_bus_arbiter.h/.cc` 仍然保留在工程里，但已经不再是 mesh 主路径依赖。

## 7. 一句话理解

```text
Tm_mesh_inf
  负责端点本地行为

TmMeshRouter + TmMeshLink
  负责网内逐跳行为

TmMeshTargetPort
  负责目标端口行为

TmMeshFabric
  负责把它们组织和调度起来
```
