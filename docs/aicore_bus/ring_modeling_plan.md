# Ring 建模方案

当前 `tm_ring_*` 实现是一版 **message-level bidirectional Ring NoC-lite**。它借鉴 gem5 SimpleNetwork 的 message buffer、逐跳 switch/link、反压和 link throttle 思路，但不下沉到 Garnet/BookSim 那种 flit、VC、credit allocator 级别。

## 1. 建模目标

目标是服务 AI Core SoC 级 ESL/CA 互连建模：

- 多 master 并发访问。
- 多 target / memory partition。
- 地址交织和热点访问。
- Ring hop latency。
- Link 序列化和 in-flight 限制。
- Target credit/token/outstanding。
- Master outstanding。
- 请求/响应分离。
- 关键路径 debug log。

非目标：

- flit 拆分。
- virtual channel。
- VC allocator / switch allocator。
- cache coherence / snoop。
- RTL AXI 五通道逐拍行为。

## 2. 当前拓扑

Ring 是一维双向结构：

```text
       EAST / clockwise
  R0 ---------------- R1
  |                  |
  |                  |
  R3 ---------------- R2
       WEST / counter-clockwise
```

每个 router 是一个 ring stop，包含三个逻辑方向：

- `LOCAL`：本地 master/target 注入和弹出。
- `EAST`：顺时针方向。
- `WEST`：逆时针方向。

路由规则：

- 当前 node 等于目的 node：走 `LOCAL`。
- 顺时针更近：走 `EAST`。
- 逆时针更近：走 `WEST`。
- 等距：走 `EAST`。

## 3. 数据通路

### 3.1 请求路径

```text
BIU/API
  -> TmRingInf
  -> source TmRingRouter LOCAL
  -> TmRingLink
  -> one or more intermediate TmRingRouter
  -> target TmRingRouter LOCAL
  -> TmRingTargetPort
  -> TmMem / target
```

`RD`、`WR`、`WR_DAT` 都走 `REQ subnet`。其中 `WR_DAT` 必须在收到 `WR_RSP` grant 后才能进入 `wr_data_` 队列再注入 Ring。

### 3.2 响应路径

```text
TmMem / target
  -> TmRingTargetPort
  -> target TmRingRouter LOCAL
  -> TmRingLink
  -> one or more intermediate TmRingRouter
  -> master TmRingRouter LOCAL
  -> TmRingInf
  -> BIU/API
```

`RD_RSP`、`WR_RSP`、`RSP` 都走 `RSP subnet`。`RD_RSP` 支持多 lane，lane 信息保存在 `pld->ring_rsp_lane`。

## 4. 模块分工

详细资源和连接关系见 [ring_module_resources.md](./ring_module_resources.md)。这里给出简表：

| 模块 | 主要职责 | 主要资源 |
| --- | --- | --- |
| `TmRingFabric` | 创建和连接整张 Ring | NIU、Router、Link、TargetPort、Topology、FlowCtrl |
| `TmRingInf` | Master 侧 NIU | `bus_inf_`、`router_inf_`、本地命令队列、OSD、completion 表 |
| `TmRingRouter` | Ring stop 转发 | `LOCAL/EAST/WEST` 端口、输出接口、RR 指针 |
| `TmRingLink` | 有向链路 | `src_inf_`、`dst_inf_`、in-flight FIFO、序列化状态 |
| `TmRingTargetPort` | Target 侧 NIU | `ring_inf_`、memory `inf_`、target request queue、response timing |
| `TmRingTopology` | 节点和路由 | master/target node 映射、interleave、方向计算 |
| `TmBusFlowCtrl` | Target 流控 | credit、token、outstanding、busy cycle |

## 5. 缓存放在哪里

当前设计不把所有缓存都放到 Router：

- `TmRingInf`：保存 master 本地命令、写数据、API completion。
- `TmRingRouter`：只保留端口短缓存，用于一跳转发和仲裁。
- `TmRingLink`：保存传播延迟中的 in-flight packet。
- `TmRingTargetPort`：保存 target 本地 request queue 和 response issue timing。

这样做的边界更清楚：

- NIU 负责 master-side 状态。
- Router 负责当前 hop 的转发。
- Link 负责链路延迟和吞吐。
- TargetPort 负责 memory-side 状态。

## 6. Link 建模

`TmRingLink` 同时表达两件事：

- propagation latency：通过 `inflight_packets_` 的 `TmQue` 延迟表达。
- serialization bandwidth：通过 `next_send_time_` 和 `ring_link_width_bytes` 表达。

packet byte 计算遵循：

- `RD` / `WR`：只算请求头。
- `WR_DAT`：算写数据大小。
- `RD_RSP`：算读响应数据大小。
- `WR_RSP` / `RSP`：只算响应头。

## 7. Flow control

模型有多层反压：

- `TmInf::send()` 失败：下游端口满。
- `TmQue::full()`：内部队列满。
- Master OSD：限制每个 master 在途读写事务。
- Link OSD：限制每条 link 每个 subnet 的 in-flight packet。
- Target credit/token/outstanding：限制 target 侧可接收和可发射能力。

统一规则是：下游不接收时，不 pop 上游队头。

## 8. 与 gem5 SimpleNetwork 的对应关系

| gem5 SimpleNetwork 概念 | 当前 Ring 对应 |
| --- | --- |
| MessageBuffer | `TmInf` / `TmQue` |
| Switch | `TmRingRouter` |
| Throttle / Link | `TmRingLink` |
| Endpoint queue | `TmRingInf` / `TmRingTargetPort` |
| Logical network | `REQ subnet` / `RSP subnet` |

当前模型更像 SimpleNetwork，而不是 Garnet。

## 9. Debug 和可观测性

当前 Ring 主模块都创建独立 log：

- Fabric：网络创建和绑定。
- Inf：master 请求、写 grant、写数据、completion。
- Router：route commit。
- Link：enqueue、drain、dst full。
- TargetPort：request 到 memory、response 回 Ring。

这些 log 可以用于追踪一笔事务从 master 到 target 再返回的完整路径。
