# Mesh NoC-lite 建模技术方案

## 1. 文档定位

本文档面向当前 `BUS/aicore/tm_mesh_*` 这一版 mesh 互连实现，给出一份更偏工程落地的建模方案说明。

它回答的不是“代码里现在有什么”，而是：

- 这版 mesh 模型为什么这样设计
- 它借鉴了 gem5 mesh 的哪些思想
- 它当前建模到哪一层
- 它的关键技术点、边界和后续演进方向是什么

建议把本文档理解为：

- `mesh_fabric_design.md` 的方案版补充
- `tm_mesh_*` 实现的技术说明书

## 2. 建模目标

这版 mesh 的目标是构建一个适合 AI Core SoC/ESL 场景的事务级多跳互连模型，重点覆盖以下能力：

- 多 AI Core 到多 memory target 的多跳访问路径
- mesh 拓扑带来的 hop latency 差异
- 地址到 target/channel 的分流与交织
- request/response 分离后的基本拥塞行为
- target 侧 outstanding、带宽和热点瓶颈

当前版本不覆盖以下能力：

- flit 级拆包
- VC 分配
- router/link 级 hop-by-hop credit
- 复杂自适应路由
- 基于 link weight 的路由表构造

因此它的定位应当明确为：

**transaction-level Mesh NoC-lite**

而不是：

**Garnet 风格的 flit/VC 级 NoC**

## 3. 参考来源

这版 mesh 主要借鉴 gem5 的 mesh 拓扑和确定性路由思想，重点参考：

- [Mesh_XY.py](</C:/Users/wayne/Downloads/gem5-stable/configs/topologies/Mesh_XY.py>)
- [Mesh_westfirst.py](</C:/Users/wayne/Downloads/gem5-stable/configs/topologies/Mesh_westfirst.py>)
- [Topology.cc](</C:/Users/wayne/Downloads/gem5-stable/src/mem/ruby/network/Topology.cc>)
- [SimpleNetwork.hh](</C:/Users/wayne/Downloads/gem5-stable/src/mem/ruby/network/simple/SimpleNetwork.hh>)
- [Router.hh](</C:/Users/wayne/Downloads/gem5-stable/src/mem/ruby/network/garnet/Router.hh>)
- [README.txt](</C:/Users/wayne/Downloads/gem5-stable/src/mem/ruby/network/garnet/README.txt>)

借鉴点主要有三类：

1. 用 `rows / cols` 描述二维 mesh 网格。
2. 用 endpoint 和 network fabric 分离的方式组织系统。
3. 用确定性坐标路由表达多跳传播。

没有直接照搬的内容主要是：

- `InputUnit / OutputUnit`
- `SwitchAllocator`
- `CreditLink`
- flit / VC / vnet 细节

## 4. 总体方案

### 4.1 设计结论

建议将当前 mesh 版定义为：

- 端点接口保持 `PemBiu / TmMem / tm_*` 风格
- 内部 fabric 采用 router-grid 多跳前推
- 请求和响应走独立逻辑子网
- target 侧继续保留事务级 credit/token 流控
- 网络内部先采用 FIFO + hop latency，而不是 hop-by-hop credit

### 4.2 模块关系图

```text
                 +----------------------+
                 |      PemBiu[0..N]    |
                 |  RD_REQ/WR_REQ/WR_DAT|
                 +----------+-----------+
                            |
                            v
                 +----------------------+
                 |     TmMeshFabric     |
                 |  master ingress FIFO |
                 +----------+-----------+
                            |
                            v
         +---------------------------------------------+
         |              TmMeshTopology                 |
         | addr -> target_id -> dst_node -> next_node  |
         +---------------------------------------------+
                            |
                            v
         +---------------------------------------------+
         |             Mesh Router Grid                |
         | mesh_*_req_fifo / mesh_*_rsp_fifo           |
         | next_mesh_*_hop_time / mesh_link_latency    |
         +---------------------------------------------+
                            |
                            v
                 +----------------------+
                 |   target local FIFO  |
                 +----------+-----------+
                            |
                            v
                 +----------------------+
                 |   TmBusFlowCtrl      |
                 | slot/token/busy/hot  |
                 +----------+-----------+
                            |
                            v
                 +----------------------+
                 |     TmMem[0..M]      |
                 +----------------------+
```

### 4.3 代码模块关系

```text
TmMeshFabric
  |- TmMeshTopology
  |    |- rows/cols/router_count
  |    |- master_node / target_node
  |    `- compute_next_node
  |
  |- TmBusFlowCtrl
  |    |- target slot credit
  |    |- target bandwidth token
  |    `- target busy / hotspot
  |
  |- tm_mesh_req.cc
  |    |- recv_master_req
  |    |- inject_mesh_req
  |    |- advance_mesh_req_type
  |    `- send_target_req
  |
  `- tm_mesh_rsp.cc
       |- recv_target_*_rsp
       |- advance_mesh_*_rsps
       `- send_master_*_rsp
```

## 5. 拓扑建模方案

### 5.1 Router 网格

当前 mesh 使用：

- `mesh_rows`
- `mesh_cols`

来描述二维 router 网格。

其中：

- `mesh_rows` 由配置给出
- `mesh_cols` 可以显式给出
- 若 `mesh_cols == 0`，则根据 endpoint 数自动补齐

router 总数为：

```text
router_count = mesh_rows * mesh_cols
```

对应实现见：

- [tm_mesh_types.h](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_types.h>)
- [tm_mesh_topology.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_topology.cc>)

### 5.2 Endpoint 到 Router 的映射

当前版本使用最简单的线性映射：

- `master_node(master_port) = master_port`
- `target_node(target_id) = num_masters + target_id`

优点：

- 与 ring 版本语义一致
- 易调试
- 易和 `target_id` 对齐

不足：

- 未体现 cluster/locality
- 没有把 core 和 memory channel 做空间亲和摆放

因此它适合作为 V1 映射，不建议当作最终版物理拓扑。

### 5.3 建议的 memory 建模方式

如果系统是：

- 8 个 core
- 1 个 DRAM 子系统
- 4 个 channel

建议建成：

- 8 个 core endpoint
- 4 个 memory-channel endpoint

而不是：

- 8 个 core endpoint
- 1 个大 DRAM endpoint

原因是 channel 级并行性、带宽和 outstanding 必须显式可见。

## 6. 路由建模方案

### 6.1 两段式路由

建议始终保持两段式路由：

1. `addr -> target_id`
2. `target_id -> dst_node`

即：

- 地址空间和交织规则负责决定访问哪个 target/channel
- mesh 负责把事务送到对应 target 所在 router

这能保证地址解码层和 NoC 层解耦。

### 6.2 确定性坐标路由

当前版本建议只支持：

- `X-first`
- `Y-first`

当 `mesh_x_first = true` 时：

1. 先比较列坐标
2. 列对齐前先做左右移动
3. 列对齐后再做上下移动

当 `mesh_x_first = false` 时：

1. 先比较行坐标
2. 行对齐后再做左右移动

这是一种事务级的 XY/YX 路由简化表达。

优点：

- 易实现
- 易验证
- 易解释
- 足够支撑 V1 多跳趋势分析

## 7. 事务路径建模

### 7.1 请求路径

请求路径建议固定为：

```text
PemBiu
  -> master ingress FIFO
  -> source router 注入
  -> mesh 逐跳前推
  -> destination router
  -> target local FIFO
  -> target interface
  -> TmMem
```

当前代码入口：

- [tm_mesh_req.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_req.cc>)

主流程函数：

- `recv_master_req()`
- `inject_mesh_req()`
- `advance_mesh_req_type()`
- `send_target_req()`

### 7.2 响应路径

响应路径建议固定为：

```text
TmMem
  -> target response receive
  -> destination router 注入响应子网
  -> mesh 逐跳返回 source router
  -> master response FIFO
  -> PemBiu
```

当前代码入口：

- [tm_mesh_rsp.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_rsp.cc>)

主流程函数：

- `recv_target_rd_rsp()`
- `recv_target_wr_req_rsp()`
- `recv_target_wr_dat_rsp()`
- `advance_mesh_rd_rsps()`
- `advance_mesh_wr_req_rsps()`
- `advance_mesh_wr_dat_rsps()`
- `send_master_rd_rsp()`
- `send_master_wr_req_rsp()`
- `send_master_wr_dat_rsp()`

### 7.3 写事务必须保留两阶段

写事务建议继续保留以下语义：

```text
WR_REQ -> WR_REQ_RSP(grant) -> WR_DAT -> WR_DAT_RSP
```

原因：

- 与 `PemBiu` 当前接口完全一致
- 可表达 target 写 buffer / grant 约束
- 便于后续演进到更复杂 NoC

当前配套状态包括：

- `TmMeshGrant`
- `m_wr_grant_fifo_`
- `txn_ctx_`

## 8. 流控建模方案

### 8.1 当前采用的流控层次

当前 mesh 使用的是：

**mesh 拓扑 + target 级事务流控**

而不是：

**mesh 拓扑 + router/link 级 credit**

也就是说，当前流控重点仍在 target 侧：

- target slot credit
- target bandwidth token
- target busy time
- target outstanding
- hotspot penalty

对应实现：

- [tm_bus_flow_ctrl.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_bus_flow_ctrl.cc>)

### 8.2 网络内部反压如何产生

虽然还没有 hop-by-hop credit，但当前 mesh 内部已经有两类反压：

1. router 本地 FIFO 满
2. 下一跳 hop 时间未到

这使模型已经能表达：

- 多跳路径越长，完成时间越长
- 某些 router 局部堆积
- target 忙时回压到网络注入侧

### 8.3 当前方案的优缺点

优点：

- 与现有 `PemBiu / TmMem` 接口天然对齐
- 参数少，校准容易
- 足够表达多跳和多 channel 的主要趋势
- 代码复杂度明显低于 Garnet

不足：

- 无法精确表达中间 router/link 的 credit 泡沫传播
- 无法表达 VC 竞争
- 无法表达细粒度 router buffer 占用

## 9. 事务上下文建模

`txn_ctx_` 是 mesh 版的关键状态表，不只是辅助缓存。

建议至少跟踪：

- `master_port`
- `target_id`
- `src_node`
- `dst_node`
- `req_type`
- `state`
- `size`
- `issue_time`
- `rsp_expected`
- `rsp_seen`
- `slot_released`

对应定义：

- [tm_mesh_types.h](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_types.h>)

它的作用是：

- 跟踪请求是否已注入网络
- 跟踪是否已到达 target
- 跟踪是否已注入响应子网
- 跟踪是否已经释放 target slot

## 10. 关键技术要点

### 10.1 保持地址解码与 NoC 解耦

不要让 mesh 直接理解地址交织细节。  
mesh 只理解：

- 当前事务的 `dst_node` 是谁

地址到 target 的映射应继续放在 topology/interleave 层。

### 10.2 请求和响应必须分网

哪怕仍然是事务级，也建议继续保持：

- request path
- response path

逻辑分离。

这是避免 `WR_DAT` 阻塞 `RD_REQ` 和读响应的关键。

### 10.3 多 channel 必须建成多 target

channel 如果不拆开，mesh 的大部分意义会丢失，因为：

- hop 路径差异看不出来
- target 并行性看不出来
- interleave 也失去建模价值

### 10.4 当前最适合做趋势分析

这版 mesh 更擅长：

- core 数变化对延迟/吞吐的影响
- 多 channel 分流收益
- path length 差异
- target 热点问题

不适合拿来做：

- 细粒度路由器微结构研究
- VC 数量优化
- link-level deadlock/credit 分析

## 11. 配置建议

建议 mesh V1 的核心配置面如下：

**拓扑参数**

- `mesh_rows`
- `mesh_cols`
- `mesh_x_first`

**网络缓存参数**

- `mesh_req_fifo_depth`
- `mesh_rsp_fifo_depth`

**链路参数**

- `mesh_link_latency`

**端点能力参数**

每个 target 保留：

- `rd_slot_credit_max`
- `wr_slot_credit_max`
- `acc_slot_credit_max`
- `rd_bw_token_max`
- `wr_bw_token_max`
- `acc_bw_token_max`
- `token_update_period`
- `frontend_latency`
- `forward_latency`
- `response_latency`
- `width`
- `hotspot_threshold`
- `hotspot_penalty`

## 12. 后续演进路线

建议按以下阶段演进：

### V1

- transaction-level mesh
- router FIFO
- hop latency
- target credit/token

### V1.5

- 新增 router/link credit 扩展接口
- 每个 router 输入方向独立状态
- 每条 link 独立前向/反向状态

### V2

- 引入 packet-level network flow control
- 真正的 hop-by-hop credit
- router 输出仲裁对象化

### V3

- 视需求再决定是否上 flit/VC

## 13. 与 ring 版本的关系

ring 版和 mesh 版的关系建议理解成：

- ring 是从共享总线向 NoC 过渡的第一步
- mesh 是从 ring 向更真实 NoC 继续推进的一步

因此 mesh 版不是推翻 ring，而是：

**在保持事务级接口不变的前提下，把拓扑从一维环扩展到二维网格。**

## 14. 结论

当前 `tm_mesh_*` 这版最合适的定义是：

**一个借鉴 gem5 Mesh_XY 拓扑与确定性坐标路由思想、保持 `PemBiu / TmMem / tm_*` ESL 风格、采用 request/response 分离和 target-credit 驱动的 transaction-level Mesh NoC-lite 模型。**

它的价值在于：

- 比 ring 更接近训练类 AI 芯片的片上互连形态
- 比 Garnet 更容易接入当前工程
- 已经能支撑多核、多 channel、多跳的趋势分析
- 后续可以平滑升级到 router/link credit 版本
