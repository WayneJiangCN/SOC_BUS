# Mesh Fabric 设计说明

## 1. 设计目标

当前 `tm_mesh_*` 的设计目标是：

- 保留现有 `PemBiu / TmMem / tm_*` 接口风格
- 在 transaction/message 级建模多跳 mesh
- 保留 AI Core 风格的请求/响应协议
- 让代码结构逐步接近 `Core - NI - Router - Link - Router - NI - Mem`

因此当前实现不是“大 fabric 管一切”的结构，而是下面五类对象协作：

- `Tm_mesh_inf`
- `TmMeshRouter`
- `TmMeshLink`
- `TmMeshTargetPort`
- `TmMeshFabric`

## 2. 当前结构

```text
Core / BIU
  -> Tm_mesh_inf
  -> TmMeshRouter
  -> TmMeshLink
  -> TmMeshRouter
  -> TmMeshTargetPort
  -> TmMem / target
```

## 3. 模块职责

### 3.1 `Tm_mesh_inf`

负责：

- 上游 `bus_inf_`
- 本地 request pending
- 本地 grant
- 本地 completion 跟踪
- response 回送

### 3.2 `TmMeshRouter`

负责：

- 本节点 request queue
- 本节点 write-data queue
- 本节点 response queues
- 同一输出口上的本地 RR 选择

当前 router 是 **粗粒度 message-level switch**，不是 Garnet 式 router 微结构。

### 3.3 `TmMeshLink`

负责：

- hop latency
- 共享输出节流

当前 link 是轻量链路模型，不做 credit。

### 3.4 `TmMeshTargetPort`

负责：

- target-local request queues
- 下游 `inf()` 接口
- Router 到 target 的最后一跳交接

### 3.5 `TmMeshFabric`

负责：

- 组织 NIU / Router / Link / TargetPort
- 维护共享 `txn_ctx_`
- 驱动 tick 调度
- 调用 topology、route、flow control

## 4. 请求路径

```text
upstream / API
  -> Tm_mesh_inf pending
  -> Fabric 注入
  -> Router
  -> Link
  -> Router
  -> TargetPort queue
  -> target / TmMem
```

当前前向子网：

- request subnet：`RD_REQ + WR_REQ`
- data subnet：`WR_DAT`

## 5. 响应路径

```text
target / TmMem
  -> TmMeshTargetPort
  -> Router response queues
  -> Link
  -> Router
  -> Tm_mesh_inf
  -> upstream
```

当前回程类型：

- `RD_RSP`
- `WR_REQ_RSP`
- `WR_DAT_RSP`

其中：

- `WR_REQ_RSP` 回到 `Tm_mesh_inf` 后会生成本地 grant
- `WR_DAT_RSP` 代表写事务最终完成

## 6. 当前流控边界

当前仍然以 **target-level transaction flow control** 为主：

- target slot credit
- target bandwidth token
- target busy time
- hotspot penalty

Router / Link 当前只显式建模：

- queue depth
- 本地 RR 仲裁
- shared output throttle
- hop latency

## 7. 为什么这样设计合适

这版结构适合当前项目，是因为它同时满足：

- 比单跳总线更接近训练类 AI 芯片的片上互连形态
- 比 Garnet 轻，能和现有 `PemBiu / TmMem / tm_*` 对齐
- 保留 AI Core 特有的写事务分阶段语义
- 能独立跑 CA/ESL 多 core 用例

## 8. 设计收敛建议

当前结构已经足够支撑目标，建议在这里收住：

- endpoint 保持 message-buffer 思路
- router 保持粗粒度 RR + queue
- link 保持 shared throttle + latency
- flow control 继续以 target-level 为主

不建议继续向下做：

- flit
- VC
- credit
- router pipeline
- 细 crossbar

这会让模型走向 NoC 微结构研究，而不是 SoC 级轻量互连模型。
