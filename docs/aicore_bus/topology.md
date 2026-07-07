# 拓扑结构说明

## 1. 文档范围

本文档描述 `TmBusFabric` 当前 ring 版本的拓扑组织方式，重点包括：

- ring 节点组织
- master 节点与 target 节点映射
- 地址解码与目标选择
- interleave 在 ring 版本中的位置

## 2. 总体拓扑

当前 `TmBusFabric` 采用单向 ring 拓扑。

系统视图可以写成：

```text
PemBiu[0..N-1] -> Master Nodes -> Ring -> Target Nodes -> TmMem[0..M-1]
                                           ^
                                           |
                                 响应沿 ring 返回
```

与最早的中心式共享总线相比，当前模型不再是“所有请求都先进入一个中心仲裁点”，而是：

- 每个 master 有自己的注入节点
- 每个 target 有自己的接收节点
- 请求和响应都在 ring 上逐跳前进

## 3. 节点编号

当前节点总数定义为：

```text
ring_node_count = num_masters + num_targets
```

节点映射规则是固定的：

- `master_node(master_port) = master_port`
- `target_node(target_id) = num_masters + target_id`

也就是说：

- 前半段节点属于 master 侧
- 后半段节点属于 target 侧

这套逻辑定义在 `BUS/aicore/tm_bus_topology.cc` 中的：

- `ring_node_count()`
- `master_node()`
- `target_node()`

## 4. 单向 ring

当前 ring 是单向前推的。

下一跳定义为：

```text
next_ring_node(node) = (node + 1) % ring_node_count
```

这意味着：

- 当前版本没有顺时针/逆时针双向选择
- 没有最短路计算
- 所有包都沿统一方向前进

这样做的好处是：

- 实现简单
- 时序行为容易解释
- 更适合 V1/V2 阶段的事务级 bring-up

对应代价是：

- 路径可能不是最短
- 热点更容易在固定方向累积

## 5. 地址解码与目标选择

虽然互连已经变成 ring，但目标选择仍然分两步完成：

1. 先根据地址算出 `target_id`
2. 再把 `target_id` 映射成 `dst_node`

也就是说，当前 `decode_target(addr)` 并不是直接决定“下一跳去哪里”，而是先决定：

```text
这个事务最终要去哪个 target
```

随后再由：

```text
dst_node = target_node(target_id)
```

把它放到 ring 拓扑中。

这是当前 ring 版本非常重要的一点：

- 地址路由仍然是端点级
- ring 路由只是网络级转发

## 6. Default Target

如果一个地址没有命中任何显式 target 范围，当前实现仍然支持回退到 `default target`。

因此 ring 版本并没有改变地址解码策略的基本边界，只是把“到达 target”的过程，从单跳改成了多跳。

## 7. Interleave 在 ring 中的作用

interleave 仍然保留，而且依然是拓扑的一部分。

只是它在 ring 版本中的职责更清楚了：

- interleave 负责在共享地址空间下选择具体 `target_id`
- ring 负责把事务送到这个 `target_id` 对应的 `dst_node`

也就是说，当前层次关系是：

```text
addr
  -> decode_target()
  -> interleave strategy
  -> target_id
  -> target_node(target_id)
  -> ring forwarding
```

这比最早总线版本更容易继续演进到 mesh，因为：

- interleave 仍然只关注端点选择
- ring 只关注网络转发

## 8. 当前支持的 interleave 策略

当前支持两类策略：

- `LINEAR`
- `XOR_HASH`

这两类策略仍定义在：

- `BUS/aicore/tm_bus_interleave.h`
- `BUS/aicore/tm_bus_interleave.cc`

ring 版本没有改变它们的算法本身，只改变了它们在整体系统中的位置。

## 9. 当前拓扑的局限

当前拓扑有几个明确限制：

1. 只支持单向 ring。
2. 不支持多条物理路径并行选择。
3. 不支持 router 级自适应路由。
4. 不支持 mesh 坐标和多维拓扑。
5. 不支持 hop-by-hop credit 控制。

因此它更适合被理解为：

```text
事务级 ring NoC-lite
```

而不是完整 NoC 模型。
