# Mesh Fabric 设计说明

## 1. 文档目标

本文档说明 `BUS/aicore/tm_mesh_*` 这一组代码的设计目标、实现边界和与 gem5 mesh 的关系。

当前这版 mesh 的定位是：

- 事务级 mesh NoC-lite
- 保留 `PemBiu / TmMem / tm_*` 接口风格
- 借鉴 gem5 `Mesh_XY` 的网格拓扑与确定性路由思路
- 不直接进入 Ruby/Garnet 的 flit / VC / router-credit 复杂度

## 2. 参考来源

这版 mesh 主要借鉴了 gem5 里的三类思路。

### 2.1 网格拓扑组织

主要参考：

- `configs/topologies/Mesh_XY.py`
- `configs/topologies/Mesh_westfirst.py`

借鉴点是：

- 用 `rows / cols` 描述 2D mesh
- 每个 endpoint 挂接到某个 router
- router 之间按坐标关系连接

### 2.2 确定性路由

主要参考：

- `Mesh_XY.py`
- `src/mem/ruby/network/garnet/RoutingUnit.hh`

借鉴点是：

- 采用确定性坐标路由
- 当前默认是 `X-first`
- 配置项 `mesh_x_first` 可以切到先走 Y 再走 X

### 2.3 多跳网络与端点分离

主要参考：

- `src/mem/ruby/network/Topology.cc`
- `src/mem/ruby/network/simple/SimpleNetwork.cc`

借鉴点是：

- endpoint 与 network fabric 分离
- 内部由多跳拓扑负责前推
- 端点只关心注入和弹出

## 3. 没有直接照搬的部分

当前 mesh 版本没有直接照搬 Ruby/Garnet 的这些机制：

- flit 切分
- VC
- input unit / output unit
- switch allocator
- hop-by-hop credit link
- link weight 驱动的 routing table 构造

原因是当前 AI Core 项目仍然优先追求：

- CA/ESL 易接入
- 与现有 `PemBiu` 协议兼容
- 较低实现成本
- 容易做统计和解释

## 4. 总体架构

当前 mesh 版本的系统视图如下：

```text
PemBiu[0..N-1]
  -> master ingress FIFO
  -> mesh router nodes
  -> target local FIFO
  -> TmMem[0..M-1]

Rsp:
TmMem
  -> mesh router nodes
  -> master response FIFO
  -> PemBiu
```

它和 ring 版本的主要区别是：

- ring 版本只有一维循环节点
- mesh 版本是二维网格节点
- ring 版本 `next hop` 是固定单向
- mesh 版本 `next hop` 由坐标关系决定

## 5. 拓扑组织

### 5.1 Router 数量

当前 mesh 版本的 router 总数定义在 [tm_mesh_topology.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_topology.cc>) 里。

核心规则是：

- endpoint 数量 = `num_masters + num_targets`
- `mesh_rows` 由配置给出
- `mesh_cols` 如果未显式给出，则自动按 endpoint 数和 `mesh_rows` 计算
- `router_count = rows * cols`

这意味着：

- 可以允许网格上存在未挂 endpoint 的空 router
- 后续从简单 mesh 继续扩到更大网格会更自然

### 5.2 Endpoint 到 Router 的映射

当前版本采用最简单的线性映射：

- `master_node(master_port) = master_port`
- `target_node(target_id) = num_masters + target_id`

也就是说：

- 前一段 router 直接挂 masters
- 后一段 router 直接挂 targets

这和 ring 版本保持了一致的 endpoint 编号语义，方便横向比较。

## 6. 路由方式

当前 mesh 路由核心在 [tm_mesh_topology.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_topology.cc>) 的 `compute_next_node()`。

### 6.1 X-first

默认配置 `mesh_x_first = true` 时：

1. 先比较列坐标
2. 如果列还没对齐，先左右走
3. 列对齐后，再比较行坐标上下走

这相当于事务级的 XY 路由简化版。

### 6.2 Y-first

如果 `mesh_x_first = false`：

1. 先比较行坐标
2. 行对齐后再比较列坐标

这样可以很方便做两种简单确定性路由的对比。

## 7. 请求路径

请求路径入口在：

- [tm_mesh_req.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_req.cc>)

主流程是：

1. `recv_master_req()`
   从 `PemBiu` 收包，进入 per-master ingress FIFO。
2. `inject_mesh_req()`
   从 source router 把事务注入 mesh。
3. `advance_mesh_req_type()`
   根据 `compute_next_node()` 逐 hop 前推。
4. 到达 `dst_node` 后，进入 target local FIFO。
5. `send_target_req()`
   在 target credit 和时延允许时真正发给下游 `TmMem`。

请求状态机沿用了 ring 版本的思路，只是把：

- `IN_REQ_RING`

换成了：

- `IN_REQ_MESH`

## 8. 响应路径

响应路径入口在：

- [tm_mesh_rsp.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_rsp.cc>)

主流程是：

1. target 收到响应后，先从 target 节点注入 mesh。
2. `advance_mesh_*_rsps()` 按坐标逐跳回 source router。
3. 回到 source router 后，进入 master response FIFO。
4. `send_master_*_rsp()` 再送回 `PemBiu`。

读响应、写请求响应、写数据响应三条回程路径仍然分开。

## 9. 流控方式

当前 mesh 版本复用了 [tm_bus_flow_ctrl.h](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_bus_flow_ctrl.h>) 和 [tm_bus_flow_ctrl.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_bus_flow_ctrl.cc>) 的 target 级流控能力。

这意味着当前 mesh 的流控重点仍是：

- target slot credit
- target bandwidth token
- target busy time
- target outstanding

同时 mesh 自身增加了一层：

- per-router FIFO 容量限制
- `mesh_link_latency` 驱动的 hop 时间

所以当前版本属于：

```text
mesh 拓扑 + target 级事务流控
```

而不是：

```text
mesh 拓扑 + router/link 级 credit NoC
```

## 10. Buffer 组织

当前 mesh 版本的 FIFO 分为五层：

1. master ingress FIFO
2. mesh request FIFO
3. target local FIFO
4. mesh response FIFO
5. master response FIFO

此外仍保留：

- `m_wr_grant_fifo_`

用于写事务 grant/DBID 跟踪。

## 11. 与 ring 版本的差异

最关键差异可以概括成下面几条。

### 11.1 相同点

- 都是 transaction-level
- 都保留 `RD_REQ / WR_REQ / WR_DAT`
- 都保留独立响应回程
- 都使用 target credit/token
- 都和 `PemBiu / TmMem` 接口兼容

### 11.2 不同点

- ring 用一维环前推
- mesh 用二维坐标前推
- ring 只有固定方向 `next node`
- mesh 通过 `compute_next_node()` 做 X/Y 路由
- ring 只有 `ring_link_latency`
- mesh 通过 router 网格结构体现路径长度差异

## 12. 当前代码文件

当前 mesh 版本核心文件包括：

- [tm_mesh_types.h](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_types.h>)
- [tm_mesh_topology.h](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_topology.h>)
- [tm_mesh_topology.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_topology.cc>)
- [tm_mesh.h](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh.h>)
- [tm_mesh_core.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_core.cc>)
- [tm_mesh_req.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_req.cc>)
- [tm_mesh_rsp.cc](</C:/Users/wayne/Downloads/gem5-stable/BUS/aicore/tm_mesh_rsp.cc>)

## 13. 当前局限

这版 mesh 还有几个明确局限：

1. 没有独立 router 对象类。
2. 没有 per-port input buffer / output arbitration。
3. 没有 hop-by-hop credit。
4. 没有 flit/VC。
5. endpoint 到 router 的映射还比较简单。

因此它更适合被理解成：

**从 ring 向真正 NoC 继续演进的一版 mesh 过渡模型。**
