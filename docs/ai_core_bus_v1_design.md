# AI Core Ring-Lite 总线模型方案与架构

## 架构评审摘要

本方案选择事务级双向 Ring-Lite，而不是单层共享 XBar 或完整 flit/VC NoC。核心判断是：当前阶段需要保留多跳路径、方向竞争、链路序列化、Target 瓶颈和端到端反压，同时避免过早引入 VC allocator、switch allocator 和逐 flit credit 等高复杂度微架构。

| 评审项 | 当前决策 | 架构含义 |
|---|---|---|
| 拓扑 | 双向 Ring，每个 Master/Target 独占一个节点 | 布局确定、布线规则，平均跳数随端点数增长 |
| 路由 | 确定性最短路径，等距走 `EAST` | 易复现、易分析，但等距流量可能形成方向偏置 |
| 交换粒度 | 事务包级 | 保留端口竞争，省略 flit 交错与 VC 行为 |
| 子网 | Request/Response 逻辑分离 | 降低协议依赖耦合；若硬件共享物理链路，需要重新折算带宽 |
| 流控 | FIFO + Master OSD + Target slot + bandwidth token | 同时覆盖瞬时排队、并发上限和长期带宽上限 |
| 写协议 | `WR -> Grant -> WR_DAT -> Completion` | 保留两阶段写资源依赖和 DBID/grant 语义 |
| 精度目标 | CA/ESL 级趋势与瓶颈相关性 | 不承诺与 RTL 逐周期一致 |

从评审角度，当前模型的主要价值不是给出单笔事务的绝对时延，而是回答以下架构问题：拓扑是否足够、链路或 Target 谁先饱和、OSD 是否匹配 RTT、interleave 是否均衡，以及增加端点或带宽能否形成可兑现的系统收益。

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
4. 在每个输入端口内，按 traffic class 和读响应 lane 维护轮询指针；
5. 只有下游 Link 或本地端口能够接收时才提交转发。

仲裁以事务包为粒度。当前实现的输入端 channel/lane 选择具有轮询语义；多个输入同时竞争同一输出时，由 Link 可接受状态和事件调度顺序决定实际提交者。`output_rr_ptr_` 已记录提交历史，但尚未参与统一的跨输入 winner 选择，因此当前版本不能宣称具备严格的输出端 bounded fairness。模型能够形成输出端口竞争和方向热点，但公平性结论必须结合各 Master 的实际吞吐统计判断。

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

当前 Request 和 Response 子网分别维护 `next_send_time`、FIFO 和 inflight 计数，相当于两套独立的逻辑链路资源。如果目标硬件只提供一套由请求和响应时分复用的物理通道，当前模型会高估双向合计吞吐，校准时必须合并带宽预算或引入共享仲裁。

当前链路字节记账中，`RD/WR` 使用请求头大小，`WR_DAT` 和 `RD_RSP` 主要按 payload 大小计费，写响应按响应头大小计费。协议头是否与数据同拍、是否额外占用 beat 属于硬件相关假设，需要在与 RTL/协议规格对齐时统一。

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

### 5.3 资源记账时点

资源不是在同一个位置统一预留，而是随着事务跨越 NIU、Ring 和 TargetPort 分阶段记账。

| 事务阶段 | Master OSD | Global/Target outstanding | Slot | Bandwidth token | 释放时点 |
|---|---|---|---|---|---|
| `RD` 注入 Ring | 占用 read OSD | 尚未占用 | 尚未占用 | 尚未消耗 | 最后一拍/最后一个读响应完成后释放 Master OSD |
| `RD` 发给 Target | 已占用 | 各加一 | 占用 access + read slot | 消耗 access + read token | 第一个读响应完成时释放 Target 资源 |
| `WR` 注入 Ring | 占用 write OSD | 尚未占用 | 尚未占用 | 不消耗 | 最终写完成响应后释放 Master OSD |
| `WR` 发给 Target | 已占用 | 各加一 | 占用 access + write slot | 请求阶段不消耗写数据 token | Grant 不释放资源 |
| `WR_DAT` 发给 Target | 已占用 | 不重复增加 | 保持原写 slot | 消耗 access + write token | 最终写完成响应后释放 Target/Global 资源 |

这一实现有两个必须在性能解读中保留的语义：

- `global_osd` 限制的是已经被 Target 接受的事务，而不是所有已进入 Ring 的事务；Ring 内仍可能排队更多请求。
- 多响应读事务在第一个响应到达时释放 Target slot，在最后一个响应完成时释放 Master read OSD。该策略提高 Target 资源周转率，但需要与目标硬件的 credit 释放定义对齐。

### 5.4 顺序性与事务隔离

当前模型提供事务标识和确定性路由，但不提供全局内存顺序模型：

- 同一接口、同一 channel 内的 FIFO 顺序在队列中保持；
- 不同 traffic class、不同读响应 lane、不同 Target 或不同路径之间允许重排；
- Response 依靠 `mst_id + gid` 回到正确 Master，不依靠返回顺序匹配；
- 模型没有 same-address hazard、barrier、fence 或跨 Master ordering point；
- 模型不包含 cache coherence、snoop 或一致性目录。

如果系统要求 AXI ID ordering、同地址写后读约束或软件可见的强序语义，应由上游协议适配层、Target 或新增 ordering module 明确实现，不能从当前 Ring FIFO 行为推导得到。

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

### 7.4 架构性能上界

对给定流量，端到端有效带宽的上界可写成：

```text
BW_effective <= min(
    BW_master_injection,
    BW_ring_min_cut,
    sum(BW_target),
    OSD * payload_bytes / average_RTT
)
```

其中：

- `BW_master_injection` 由 Master FIFO、Master OSD 和注入端口能力决定；
- `BW_ring_min_cut` 由热点路径上最窄的 Link 集合决定，不能简单使用所有 Link 带宽之和；
- `sum(BW_target)` 只有在地址映射均匀且 Target 真正并行时才成立；
- `OSD * payload / RTT` 表示延迟隐藏能力，OSD 小于带宽时延积时，即使链路未满也无法达到峰值。

单条 Link 的包启动间隔由以下公式限制：

```text
Tserialize = ceil(packet_bytes / ring_link_width_bytes)
```

Target 请求和响应发射间隔分别近似为：

```text
Ttarget_req = frontend_latency + forward_latency
              + header_latency + ceil(payload / target_width)
              + hotspot_penalty

Ttarget_rsp = response_latency + header_latency
              + ceil(response_payload / target_width)
              + hotspot_penalty
```

单事务无竞争时延至少包含 NIU/接口延迟、逐跳 Link 传播延迟、Target 固定延迟和响应回程；负载升高后的增量主要来自 Router/Link/Target FIFO 排队。评审时应分别报告 zero-load latency 和 saturation throughput，避免用单一平均延迟掩盖拥塞拐点。

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

## 10. 硬件映射与模型校准

模型参数必须有明确的硬件来源。仅靠调参得到目标带宽，无法保证瓶颈位置和拥塞行为正确。

| 模型参数 | 推荐硬件来源 | 校准观察量 |
|---|---|---|
| 节点布局与方向 | Floorplan、端点连接图 | 平均/最大跳数、热点 Link |
| Link width/latency | NoC 规格、时序流水定义 | zero-load hop latency、链路饱和带宽 |
| Link/FIFO/inflight | RTL 参数、buffer allocation | 首次 backpressure 点、突发吸收能力 |
| Master OSD | BIU/NIU credit 定义 | OSD sweep 下的带宽时延积拐点 |
| Target slot | Memory slice/HBM controller queue | Target outstanding 峰值和排队延迟 |
| Token replenishment | 带宽配置寄存器、QoS 规格 | 长窗口稳定带宽 |
| Target latency | RTL 仿真或硅后计数器 | 无竞争读写 RTT 分布 |
| Interleave/hash | 地址映射规格 | 各 Target 流量占比和热点偏差 |
| 仲裁 | Router/NIU RTL | 多 Master 公平性和最坏等待时间 |

推荐采用分层校准流程：

1. **功能对齐**：验证地址解码、事务回程、写两阶段关联和资源守恒；
2. **零负载对齐**：使用单事务测量每跳延迟、Target 固定延迟和返回路径；
3. **单瓶颈对齐**：分别限制 Master、Link 和 Target，确认模型能定位正确瓶颈；
4. **饱和曲线对齐**：扫描 OSD、FIFO 和注入率，对比吞吐拐点及排队增长；
5. **混合流量对齐**：对比读写比例、地址分布和多 Master 公平性；
6. **相关性签核**：以趋势误差、峰值带宽误差和关键延迟分位数作为签核指标，而不是只比较单个平均值。

## 11. 代码架构映射

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

## 12. 演进方向

后续可以在不改变当前分层结构的前提下继续扩展：

1. 增加按 Link、Router 输出端口和 Target 的周期利用率统计；
2. 引入可配置仲裁策略和 QoS/优先级；
3. 增加更丰富的地址 Hash 和端点布局策略；
4. 增加流量类别隔离或轻量 Virtual Channel；
5. 对关键链路引入更细粒度的 beat/flit 近似；
6. 根据 RTL 或性能计数器校准延迟、带宽和资源上限。

总体而言，当前架构定位为结构上接近真实 NoC、性能上覆盖主要瓶颈、时序上保留必要竞争的事务级 Ring-Lite 模型。

## 13. 已知风险与待评审项

以下项目需要在架构冻结或与 RTL 相关性签核前给出明确结论：

1. **跨输入输出仲裁**：`output_rr_ptr_` 尚未参与统一 winner 选择，当前实现没有严格的 bounded fairness 保证。
2. **REQ/RSP 物理资源关系**：模型将两个子网作为独立链路资源；必须确认硬件是物理隔离、部分共享还是完全时分复用。
3. **Global OSD 的定义**：当前在 Target 接受请求时记账，不包含仍在 Ring 内排队的请求；需确认这是否匹配硬件 credit 域。
4. **多响应读的 slot 释放**：Target slot 在首个响应后释放，Master OSD 在最后响应后释放；需确认硬件是否采用相同策略。
5. **协议字节记账**：数据包的 header、payload 和 response header 是否共享 beat，需要与实际链路协议统一。
6. **死锁证明**：确定性最短路径和 REQ/RSP 分离降低协议相关死锁风险，但当前没有基于 channel dependency graph 的形式化无死锁证明。
7. **响应 lane 含义**：`rd_rsp_port_num` 是逻辑并发返回能力，不应直接解释为同数量的物理独立链路。
8. **端点布局假设**：当前 Router 数量等于 Master 与 Target 总数，端点不共置；若实际 floorplan 共置 NIU/TargetPort，需要调整跳数模型。
9. **Hotspot penalty**：当前是经验性附加延迟，不代表具体 RTL 结构，必须通过相关性数据标定或关闭。

建议优先关闭第 1、2、3、4 项，因为它们会直接改变公平性、峰值带宽和资源占用曲线；其余项目主要影响协议精度和与具体硬件实现的映射方式。
