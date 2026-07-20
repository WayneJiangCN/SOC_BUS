# AI Core Ring-Lite 总线模型方案与架构

## 1. 模型定位

当前总线模型是一套面向多 AI Core SoC 的事务级双向 Ring-Lite 互连，用于连接多个上游 Master 与多个存储或外设 Target。模型重点表达系统级性能中的主要结构性因素，而不是复刻 RTL 级 NoC 的全部微架构细节。

模型主要解决以下问题：

- 多个 Master 并发访问共享存储系统时的仲裁与资源竞争；
- 地址到 Target 的解码、条带化与负载分布；
- 不同 Ring 节点之间的路径长度和链路共享；
- 链路带宽、传播延迟、序列化及最大在途数量；
- FIFO 堆积以及从 Target 向 Master 逐级传播的 backpressure；
- Master 和 Target 两侧的 outstanding、slot credit 与 bandwidth token 限制；
- 读写事务的端到端生命周期、响应归属和资源释放；
- 系统吞吐、事务延迟、热点、阻塞来源和 Master 公平性。

因此，这套模型适合用于 CA/ESL 级架构分析、互连方案比较和系统瓶颈定位。

## 2. 总体架构

![AI Core Ring-Lite 总线模型总体架构](./aicore_bus/images/tm_ring_architecture-v2.png)

**图 1：AI Core Ring-Lite 总线模型总体架构。** Master 通过本地 NIU 注入事务；Router 在本地、顺时针和逆时针端口之间完成路由与仲裁；Link 建模每跳延迟、序列化和在途容量；TargetPort 连接 Ring 与 L2、DDR 或 MMIO 等下游端点。Topology 和 FlowControl 构成控制平面，分别负责地址/路径决策与容量约束。

从数据通路看，模型可以概括为：

```text
Master / BIU
    -> Master NIU
    -> Source Router
    -> Bidirectional Ring Links
    -> Destination Router
    -> Target Port
    -> L2 / DDR / MMIO

Response
    -> Target Port
    -> Response Subnet
    -> Master NIU
    -> Master / BIU
```

`TmRingFabric` 是顶层组织者，负责创建、持有和连接整张网络。细粒度队列、仲裁和传输状态由各子模块分别维护，避免把所有数据通路状态集中到一个全局对象中。

## 3. 架构分层与模块职责

### 3.1 Fabric：网络组织层

`TmRingFabric` 负责：

- 根据配置创建 Master NIU、Router、Link 和 TargetPort；
- 建立双向 Ring 的节点与有向链路；
- 将 Master 和 Target 映射到对应 Ring 节点；
- 连接各模块的 `TmInf` 接口；
- 持有共享的 Topology 与 FlowControl 对象；
- 提供 attach、reset、idle 以及事务发送/完成查询接口；
- 汇总全局 outstanding、Target 资源和 Ring Link 阻塞统计。

Fabric 本身不直接保存 Router 内部 FIFO，也不执行逐包仲裁。它更接近系统结构的装配器和统一管理入口。

### 3.2 Master NIU：主设备接入层

`TmRingInf` 是 Master 侧网络接口，主要职责包括：

- 接收来自 BIU 或上层 API 的读写事务；
- 将 `RD`、`WR` 和 `WR_DAT` 分别缓存到本地队列；
- 根据地址补充 Target、源节点和目的节点等 Ring metadata；
- 维护每个 Master 的读写 outstanding；
- 将请求注入本地 Router；
- 接收返回包并根据 `mst_id` 与 `gid` 恢复事务归属；
- 管理写请求、写授权和写数据之间的两阶段关联；
- 将最终读写响应返回上游。

NIU 将上游接口协议与 Ring 内部传输语义解耦，使 Router 和 Link 只处理统一的事务包。

### 3.3 Router：逐跳路由与输出仲裁层

`TmRingRouter` 是每个 Ring stop 上的轻量转发节点，具有三类端口：

- `LOCAL`：连接本节点的 Master NIU 或 TargetPort；
- `EAST`：顺时针方向；
- `WEST`：逆时针方向。

Router 对输入事务执行以下处理：

1. 根据 payload 中的源节点和目的节点判断请求或响应的目标方向；
2. 若目标就在本节点，则转发到 `LOCAL` 端口；
3. 否则根据 Topology 选择 `EAST` 或 `WEST`；
4. 多个输入竞争同一输出方向时执行轮询仲裁；
5. 只有下游 Link 或本地端口能够接收时才提交转发。

仲裁以事务包为粒度。模型保留了输出端口竞争和方向热点，但不进一步拆分 flit 或 Virtual Channel。

### 3.4 Link：链路传输层

`TmRingLink` 表示一个方向上的有向链路。双向 Ring 由相邻节点间的两条反向 Link 组成。

每条 Link 独立建模：

- 固定传播延迟 `ring_link_latency`；
- 每周期序列化宽度 `ring_link_width_bytes`；
- Request/Response 子网的在途 FIFO；
- Request/Response 子网各自的最大 inflight 数量；
- 下一个允许发送事务的时间；
- 包数量、字节数、忙周期、峰值在途数量和阻塞次数。

事务占用链路的序列化周期为：

```text
serialization_cycles = ceil(packet_bytes / link_width_bytes)
```

链路延迟决定包何时到达下一跳，序列化周期决定链路何时可以接收下一次发送。两者分开建模，可以表达流水化链路的带宽与传播时延差异。

### 3.5 TargetPort：目标设备接入层

`TmRingTargetPort` 位于目的 Router 与下游 Target 之间，负责：

- 接收 Ring 上到达本节点的 `RD`、`WR` 和 `WR_DAT`；
- 将不同事务类型放入独立的 Target 请求 FIFO；
- 查询 FlowControl，判断 Target 是否具备 slot、token 和发射带宽；
- 按目标端口宽度、头部延迟和响应延迟计算发射节奏；
- 将请求发送给 `TmMem` 或其他 Target；
- 接收 Target 响应并转换为 Ring Response 包；
- 选择读响应 lane，并将响应重新注入目的 Router。

TargetPort 是互连容量模型与下游设备时序模型之间的边界。

### 3.6 Topology：地址与路径控制层

`TmRingTopology` 负责两类决策。

第一类是端点布局：

- Router 数量由 Master 数量和 Target 数量共同确定；
- Target 节点沿 Ring 分散布置；
- Master 被放置到剩余节点；
- Master ID 与 Master Port 之间维护双向映射。

第二类是路由决策：

- 将访问地址解码成 `target_id`；
- 将 Master 和 Target 转换为 Ring 节点号；
- 比较顺时针和逆时针跳数；
- 选择最短方向，等距时固定选择 `EAST`，保证决策确定性。

### 3.7 FlowControl：容量与带宽控制层

`TmBusFlowCtrl` 统一管理 Target 侧资源：

- 全局事务 outstanding；
- Target 访问 slot；
- Target 读 slot 和写 slot；
- 读、写和总访问 bandwidth token；
- Token 更新周期和补充数量；
- Target 当前 outstanding；
- 目标端口的请求与响应 busy time；
- 高并发热点下的附加延迟。

FlowControl 只负责判断资源是否允许以及资源的消耗/释放，不负责事务在 Ring 上如何逐跳传输。

## 4. 地址解码与数据分布

地址解码按以下顺序执行：

1. 检查地址是否落入 Target 的地址范围；
2. 对共享地址空间应用 interleave 策略；
3. 选择 interleave slice 对应的 Target；
4. 没有显式命中时回退到 default Target。

当前支持两种 interleave：

- `LINEAR`：按照固定 stripe 大小在多个 Target 之间轮转；
- `XOR_HASH`：对 stripe 编号进行异或散列，降低规则步长访问产生集中热点的概率。

线性映射可表示为：

```text
stripe_id = (address - address_begin) / interleave_size
target_id = stripe_id mod interleave_num
```

XOR Hash 在 `stripe_id` 基础上加入移位异或和 seed，再对 Target 数量取模。

地址映射直接决定 Target 负载分布，也间接决定 Ring 路径、链路热点和返回流量的竞争位置。

## 5. 事务与子网模型

Ring 逻辑上划分为独立的 Request 和 Response 子网。二者共享相同拓扑，但使用独立通道、队列和链路状态。

| 子网 | 承载事务 | 主要作用 |
|---|---|---|
| Request | `RD`、`WR`、`WR_DAT` | 将命令和写数据从 Master 送到 Target |
| Response | `RD_RSP`、`WR_RSP`、`RSP` | 将读数据、写授权和写完成响应送回 Master |

### 5.1 读事务

读事务的生命周期为：

```text
RD
 -> Master NIU
 -> Request Subnet
 -> TargetPort
 -> Target
 -> RD_RSP
 -> Response Subnet
 -> Master NIU
```

读请求成功注入后占用 Master 读 outstanding 和 Target 读/访问资源。最终读响应返回时，根据响应数量和事务状态释放资源并完成事务。

### 5.2 写事务

写事务保留两阶段写协议：

```text
WR
 -> WR_RSP / Grant
 -> WR_DAT
 -> RSP / Write Completion
```

写请求首先申请 Target 写资源。Target 返回授权后，Master NIU 生成对应的 `WR_DAT`。写数据完成响应返回时，模型释放写 slot、全局 outstanding 以及事务上下文。

`gid` 在整个请求、授权、写数据和完成响应阶段保持稳定；`mst_id` 标识发起 Master。二者共同保证多 Master、多笔在途事务下的正确回程。

## 6. 仲裁与 Backpressure

Router 的竞争按输出方向局部化，而不是把整张网络抽象成一根全局串行总线。同一周期内，不同 Link 和不同方向可以并行推进。

模型采用 `valid-ready` 风格的事件驱动语义：

- 队列或接口存在有效数据时，由 `vld` 事件触发处理；
- 下游可接收时，事务发送成功并从上游队列弹出；
- 下游 FIFO、接口或 Link 满时，上游队头保持不动；
- 只要 `vld` 保持，调度机制就会继续尝试推进；
- 不需要为每个阻塞点额外创建周期性 retry event。

反压可以沿以下方向逐级传播：

```text
Target resource limit
 -> TargetPort FIFO
 -> Destination Router
 -> Ring Link
 -> Upstream Router
 -> Master NIU
 -> Master
```

这种建模方式能够表达局部拥塞向源端扩散以及多个流量在共享路径上相互影响的过程。

## 7. 性能模型

### 7.1 主要配置维度

| 类别 | 关键参数 | 建模含义 |
|---|---|---|
| 拓扑 | Master/Target 数量、节点映射 | Ring 规模和端点距离 |
| 路由 | 地址范围、interleave 类型与粒度 | 负载分布与路径热点 |
| Router | 输入深度、响应 lane 数 | 短时流量吸收和并发返回能力 |
| Link | 宽度、延迟、FIFO、max inflight | 每跳带宽、传播时间和链路容量 |
| Master | 读写 FIFO、读写 OSD | 注入能力和并发事务上限 |
| Target | 请求 FIFO、slot credit | 目标端排队和并发处理能力 |
| Bandwidth | token 上限、补充量、更新周期 | 长期读写带宽上限 |
| Timing | frontend/header/response latency | Target 端口发射与返回节奏 |

### 7.2 可以观察的性能现象

模型能够用于分析：

- 多 Master 同时注入时的源端竞争；
- 多个输入竞争同一 Router 输出方向时的排队；
- 近端和远端 Target 的路径延迟差异；
- 多跳事务在共享 Link 上形成的热点；
- Request 与 Response 流量各自的链路占用；
- Target slot、全局 OSD 或 bandwidth token 耗尽；
- Target 数量及 interleave 策略带来的负载均衡收益；
- 读写混合流量下两个阶段之间的相互压制；
- 多 Master 的吞吐公平性。

### 7.3 性能统计

建议从以下维度评估总线：

- 完成事务数及有效 payload bytes；
- 首次请求到最后响应之间的有效完成周期；
- 读写 payload bytes/cycle；
- 在给定时钟频率下换算的有效带宽；
- 读写平均、最小和最大响应延迟；
- Master 侧发送阻塞及响应阻塞；
- Global OSD、Target slot、bandwidth token 和 Ring Link 阻塞；
- 各 Master 吞吐以及 Jain fairness index；
- 各 Link 的包数、字节数、忙周期和 inflight 峰值。

阻塞统计表示一次被拒绝的发送或资源申请。多个端口可能在同一周期分别产生阻塞，因此阻塞次数用于判断瓶颈来源，不应直接等同于全局停顿周期。

## 8. 正确性与可观测性

总线级正确性应覆盖以下不变量：

- 每个已接受请求最终都能找到对应事务上下文；
- 响应能够根据 `mst_id` 和 `gid` 返回正确 Master；
- 写授权、写数据和写完成属于同一事务；
- Target 解码结果与地址映射策略一致；
- outstanding、slot、credit 和 token 不发生非法释放或永久泄漏；
- 网络进入 idle 时，NIU、Router、Link 和 TargetPort 均不存在遗留事务；
- 阻塞期间事务保留在队头，不丢包、不重复提交。

各模块保留独立日志，用于重建事务经过的节点、链路和资源状态。统计信息用于判断系统吞吐和瓶颈，日志用于定位单笔事务的异常路径。

## 9. 模型精度边界

当前模型保留了以下与系统性能高度相关的行为：

- 多 Master、多 Target 和多事务并发；
- 双向多跳路径；
- Router 输出竞争；
- Link 延迟、序列化和有限容量；
- Target FIFO、OSD、credit、token 与 backpressure；
- 两阶段写事务和独立响应子网。

当前不显式建模：

- flit 拆分与逐 flit 交错；
- Virtual Channel 和 VC allocator；
- 物理链路级 credit return；
- Router 内部多级 pipeline；
- RTL 总线协议的逐拍信号行为；
- cache coherence、snoop 和一致性目录。

因此，模型适合得出以下结论：

- 不同拓扑、映射和资源配置的相对优劣；
- 主要瓶颈位于 Master、Link 还是 Target；
- Target 扩展或 interleave 调整是否带来有效收益；
- 读写混合比例变化对吞吐和延迟的影响。

模型不适合直接给出 RTL 级绝对周期、flit 级 HOL 行为或微小流水优化的最终收益。

## 10. 代码架构映射

| 模块 | 代码位置 | 核心职责 |
|---|---|---|
| Fabric | `tm_ring.h`、`tm_ring_core.cc` | 创建、连接和管理整张 Ring |
| Master NIU | `tm_ring_inf.h/.cc` | 上游接入、事务跟踪、请求注入和响应完成 |
| Router | `tm_ring_router.h/.cc` | LOCAL/EAST/WEST 路由与输出仲裁 |
| Link | `tm_ring_link.h/.cc` | 有向链路延迟、序列化、inflight 和反压 |
| TargetPort | `tm_ring_target_port.h/.cc` | Target 请求排队、发射和响应回注 |
| Topology | `tm_ring_topology.h/.cc` | 节点映射、地址解码和最短路径选择 |
| FlowControl | `tm_bus_flow_ctrl.h/.cc` | OSD、slot、token、busy time 和热点约束 |
| Types | `tm_ring_types.h`、`tm_bus_types.h` | 配置、端口、子网和 Target 参数定义 |
| Payload | `tm_pld.h/.cc` | 事务标识、地址、数据与 Ring metadata |

## 11. 演进方向

后续可以在不改变当前分层结构的前提下继续扩展：

1. 增加按 Link、Router 输出端口和 Target 的周期利用率统计；
2. 引入可配置仲裁策略和 QoS/优先级；
3. 增加更丰富的地址 Hash 和端点布局策略；
4. 增加流量类别隔离或轻量 Virtual Channel；
5. 对关键链路引入更细粒度的 beat/flit 近似；
6. 根据 RTL 或性能计数器校准延迟、带宽和资源上限。

总体而言，当前架构定位为结构上接近真实 NoC、性能上覆盖主要瓶颈、时序上保留必要竞争的事务级 Ring-Lite 模型。
