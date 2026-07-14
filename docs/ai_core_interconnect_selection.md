# AI Core 互连方案选型说明

## 1. 文档目的

本文用于在以下三类候选方案之间做选型比较，并给出适合当前 AI Core 多核 SoC 场景的推荐路线：

1. gem5 `classic memory system` 的 `XBar`
2. GPGPU-Sim 的 `LOCAL_XBAR`
3. GPGPU-Sim 的 `INTERSIM / BookSim`

本文重点回答两个问题：

1. 当前 AI Core ESL/CA 级互连模型，主线应该借哪一类设计。
2. 如果不直接照搬现成实现，分别应该借鉴哪些设计思想。

## 2. 当前需求定义

当前要做的不是一个完整的 NoC 研究平台，而是一版可运行、可配置、易集成的 SoC 级事务互连模型。核心需求如下：

1. 面向多 AI Core 场景，支持多个 `PemBiu` 并发访问共享存储系统。
2. 不依赖完整 SoC 环境，能够独立跑 ESL/CA 用例。
3. 需要建模总线延迟、交织、局部瓶颈、关键路径和 backpressure。
4. 需要自然适配当前 `PemBiu -> Bus -> TmMem` 的接口形式。
5. 允许精度略低，但要求开发进度快、参数易校准、模型可解释。

从抽象层级上看，当前需求更偏向：

- transaction-level
- single-hop fabric
- explicit buffering
- configurable arbitration
- configurable bandwidth/latency

而不是：

- flit-level NoC
- multi-hop router pipeline
- VC allocation
- detailed routing research

## 3. 三类候选方案概览

### 3.1 gem5 XBar

gem5 `XBar` 属于事务级互连，主要代码位于：

- [xbar.hh](</C:/Users/wayne/Downloads/gem5-stable/src/mem/xbar.hh>)
- [xbar.cc](</C:/Users/wayne/Downloads/gem5-stable/src/mem/xbar.cc>)
- [noncoherent_xbar.hh](</C:/Users/wayne/Downloads/gem5-stable/src/mem/noncoherent_xbar.hh>)
- [noncoherent_xbar.cc](</C:/Users/wayne/Downloads/gem5-stable/src/mem/noncoherent_xbar.cc>)

核心特征：

1. 按目标端口建模仲裁层，而不是全局单仲裁点。
2. 以 request/response 为基本事务。
3. 内建地址解码、回程路由、busy/retry 反压。
4. 通过 `headerDelay + payloadDelay + width` 做粗粒度时延与带宽建模。

### 3.2 GPGPU-Sim LOCAL_XBAR

GPGPU-Sim 提供了一个轻量交叉开关模式 `LOCAL_XBAR`，相关代码位于：

- [icnt_wrapper.h](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/src/gpgpu-sim/icnt_wrapper.h>)
- [icnt_wrapper.cc](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/src/gpgpu-sim/icnt_wrapper.cc>)
- [local_interconnect.h](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/src/gpgpu-sim/local_interconnect.h>)
- [local_interconnect.cc](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/src/gpgpu-sim/local_interconnect.cc>)

核心特征：

1. 以输入 buffer / 输出 buffer 为主要资源约束。
2. 有请求网和返回网两个 subnet。
3. 仲裁支持 `RR` 和 `iSLIP`。
4. 实现轻量，适合快速估算本地互连冲突和缓冲反压。

### 3.3 GPGPU-Sim INTERSIM / BookSim

GPGPU-Sim 也支持 `INTERSIM` 模式，本质上是 BookSim 风格网络，相关入口包括：

- [icnt_wrapper.h](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/src/gpgpu-sim/icnt_wrapper.h:64>)
- [icnt_wrapper.cc](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/src/gpgpu-sim/icnt_wrapper.cc:163>)
- [README.md](</C:/Users/wayne/Downloads/gpgpu-sim_distribution-dev/README.md:101>)
- `src/intersim2/`

核心特征：

1. 网络级建模而不是总线级建模。
2. 引入 flit、router、buffer、allocator、topology、routing 等细节。
3. 适合做 NoC 研究和网络拥塞分析。

## 4. 总体结论

### 4.1 一句话结论

对于当前 AI Core 多核 SoC 互连模型，推荐路线是：

**以 gem5 XBar 为主线，选择性吸收 GPGPU-Sim LOCAL_XBAR 的 buffer / subnet / arbiter 设计，不把 INTERSIM 作为 V1 主线。**

### 4.2 结论原因

原因主要有五点：

1. 当前上下游接口本身已经是事务级的。  
   `PemBiu` 明确区分 `RD_REQ / WR_REQ / WR_DAT`，`TmMem` 也已经暴露 credit、带宽和延迟语义。

2. 当前问题核心是 SoC fabric，而不是 NoC 研究。  
   现在优先级最高的是地址路由、interleave、队列堆积、局部热点和 backpressure，而不是多跳拓扑和 flit 级拥塞传播。

3. gem5 `XBar` 的 target-centric 竞争模型更贴近需求。  
   它天然适合表达“多个 master 竞争同一个 target”的瓶颈。

4. GPGPU-Sim `LOCAL_XBAR` 的局部机制很有参考价值。  
   尤其是显式 buffer、请求/返回子网分离、仲裁器可替换结构。

5. `INTERSIM` 集成成本和标定成本都过高。  
   在缺乏精确 NoC 参数的情况下，复杂度会显著高于收益。

## 5. 对比表

| 维度 | gem5 XBar | GPGPU-Sim LOCAL_XBAR | GPGPU-Sim INTERSIM |
| --- | --- | --- | --- |
| 抽象层级 | 事务级 | 轻量包级 / 近似 xbar | flit / router / NoC 级 |
| 主要对象 | request/response | packet + buffer | flit + VC + router |
| 拓扑模型 | per-target crossbar layer | 本地 xbar / 双子网 | ring / torus / fat-tree / flatfly 等 |
| 地址解码 | 强 | 弱，更多是 GPU 设备编号 | 弱，更多是网络节点编号 |
| SoC 独立性 | 高 | 中 | 低到中 |
| 与 `PemBiu` 匹配度 | 高 | 中 | 低 |
| 与 `TmMem` 匹配度 | 高 | 中 | 低 |
| FIFO / backpressure 建模 | 强 | 强 | 强 |
| interleave 建模 | 强 | 弱 | 中 |
| 写命令/写数据两阶段事务 | 易支持 | 需要额外补语义 | 需要大量适配 |
| 开发复杂度 | 中 | 低到中 | 高 |
| 参数校准复杂度 | 中 | 低 | 高 |
| 适合当前 V1 | 很适合 | 可借鉴 | 不适合 |

## 6. gem5 XBar 值得借什么

当前最值得借的是 gem5 `BaseXBar / NoncoherentXBar` 的事务级方法论：

1. **按 target 分仲裁**  
   不做全局一把锁，而是把争用局部化。

2. **地址解码与回程路由**  
   请求发出时确定 target，响应回来时由事务上下文找到原始 master。

3. **busy time 抽象**  
   用 `header / payload / width / latency` 折算粗粒度时延。

4. **transaction-level backpressure**  
   通过 timing occupancy 和 retry 语义表达拥塞，而不是上来就做 flit credit。

## 7. LOCAL_XBAR 值得借什么

虽然 `LOCAL_XBAR` 不适合作为主线，但以下几点非常适合吸收到当前实现里：

1. **显式 buffer 组织**  
   输入缓冲、输出缓冲、响应缓冲都清晰可见。

2. **request / reply subnet 分离思想**  
   即使不真的实例化两个物理网络，也应该在逻辑上分开考虑请求路径和返回路径。

3. **仲裁器独立模块化**  
   把 `RR`、`iSLIP` 等策略和主 fabric 解耦。

4. **本地统计项**  
   例如冲突次数、buffer full 次数、平均利用率等，都很适合保留。

## 8. 为什么 INTERSIM 不适合当前 V1

### 8.1 抽象层级太重

`INTERSIM` 的优势在于：

- flit 级传输
- 多跳拓扑
- router buffer
- allocator
- routing
- 网络拥塞传播

这些对 NoC 研究非常重要，但对当前 V1 目标来说过重。

### 8.2 集成成本高

当前接口里最关键的语义包括：

- 地址解码
- `WR_REQ -> WR_REQ_RSP -> WR_DAT -> WR_DAT_RSP`
- `gid / mst_id / chan`
- `TmMem` 侧 credit 和延迟

如果直接走 `INTERSIM`，需要先解决：

1. 如何把事务拆成 packet / flit
2. 如何把网络层回包重新映射回两阶段写事务

这会把大量时间花在协议胶水上。

### 8.3 校准难度高

如果缺少精确的 NoC 参数，例如：

- router pipeline 深度
- VC 数量
- flit size
- routing 算法
- buffer depth
- hop 间链路带宽

那么 `INTERSIM` 很容易变成“更复杂，但不更可信”。

## 9. 当前仓库中的落地路线

当前仓库已经沿这条路线开始落地：

### 9.1 代码侧

当前 `TmBusFabric` 已经拆出如下模块：

- [tm_bus_topology.h](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_topology.h>)
- [tm_bus_topology.cc](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_topology.cc>)
- [tm_bus_flow_ctrl.h](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_flow_ctrl.h>)
- [tm_bus_flow_ctrl.cc](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_flow_ctrl.cc>)
- [tm_bus_arbiter.h](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_arbiter.h>)
- [tm_bus_arbiter.cc](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_arbiter.cc>)
- [tm_bus_req.cc](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_req.cc>)
- [tm_bus_rsp.cc](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_rsp.cc>)
- [tm_bus_core.cc](</C:/Users/wayne/Downloads/gem5-stable/aicore/tm_bus_core.cc>)

这套拆法对应的是：

- gem5 XBar 风格的主骨架
- LOCAL_XBAR 风格的局部 buffer / arbiter 可替换结构

### 9.2 文档侧

当前总线设计文档也已经拆成专题包：

- [docs/aicore_bus/README.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/README.md>)
- [topology.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/topology.md>)
- [transactions.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/transactions.md>)
- [flow_control.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/flow_control.md>)
- [arbitration.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/arbitration.md>)
- [buffers_and_subnets.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/buffers_and_subnets.md>)
- [code_organization.md](</C:/Users/wayne/Downloads/gem5-stable/docs/aicore_bus/code_organization.md>)

这套专题文档正是为了吸收 gem5 / GPGPU-Sim 在“按职责拆设计说明”上的优点。

## 10. 推荐路线

### 10.1 V1

V1 推荐采用：

**gem5 XBar 的事务级方法论 + 当前 `TmBusFabric` 的 ESL 工程风格 + 局部吸收 GPGPU-Sim LOCAL_XBAR 的 buffer / subnet / arbiter 设计**

### 10.2 V2

如果后续发现 V1 误差主要来自以下问题，再考虑向 NoC-lite 演进：

- 多跳传播不可忽略
- 热点扩散不仅是 target 局部堆积
- 路由策略显著影响性能
- 请求路径和返回路径不再适合只做逻辑分离

此时可进一步借：

- multi-hop latency
- per-hop queue
- simplified credit loop
- routing-aware contention

但仍不建议直接把 `intersim2` 整套嵌入当前 ESL 环境。

## 11. 最终结论

对于当前 AI Core 多核 SoC 总线模型，三者推荐顺序如下：

1. **gem5 XBar 设计方法：主线**
2. **GPGPU-Sim LOCAL_XBAR：局部机制参考**
3. **GPGPU-Sim INTERSIM：未来 NoC 演进参考**

因此，当前最合理的工程决策是：

**继续把 `TmBusFabric` 做成 gem5 XBar 风格的 transaction fabric，并有选择地吸收 GPGPU-Sim LOCAL_XBAR 的 buffer / subnet / arbiter 设计。**
