# 仲裁说明

## 1. 文档范围

本文档描述 ring 版本 `TmBusFabric` 中“仲裁”这一概念当前是如何变化的。

这里最重要的结论是：

**ring 版本已经不再采用旧总线版本那种中心式 `per-target` 仲裁作为主路径核心。**

## 2. 从旧总线版本到 ring 版本的变化

旧版本更像：

```text
master ingress
  -> per-target 选人
  -> target FIFO
  -> target
```

其核心矛盾是：

- 多个 master 同时抢同一个 target

而 ring 版本更像：

```text
master ingress
  -> source node 注入 ring
  -> ring 逐跳前推
  -> target node
  -> target FIFO
  -> target
```

它的核心矛盾已经转成：

- 当前节点能不能继续往下一跳送
- 当前目标 FIFO 是否还有空间

## 3. 当前主路径中的“仲裁”

当前 ring 主路径里的仲裁更加弱化，主要表现为以下几种局部顺序规则：

1. 每个 FIFO 先看队头。
2. 每个 node 对某一类 traffic 一次只前推一个队头包。
3. 如果下一跳 FIFO 满，当前 node 本拍就停住。
4. 如果当前 node 已到达目的节点，则优先尝试进入本地 target FIFO 或 master response FIFO。

所以当前版本的“仲裁”本质上更接近：

```text
FIFO 顺序 + 局部可前推判断
```

而不是独立的中心式调度器。

## 4. 为什么会这样

这是 ring 拓扑带来的自然变化。

在中心总线里，问题是“谁先占中心资源”。

在 ring 里，问题变成了：

- 这个包是否已经到达目的节点
- 没到的话下一跳能不能走

因此主路径调度天然从“全局抢占”变成了“分布式逐跳前推”。

## 5. 请求路径中的局部顺序

当前请求路径的主要顺序是：

1. `recv_master_reqs()` 按 `WR_REQ -> WR_DAT -> RD_REQ` 顺序收包。
2. `inject_ring_reqs()` 按 `RD_REQ -> WR_REQ -> WR_DAT` 顺序尝试把本地 FIFO 注入 ring。
3. `advance_ring_reqs()` 分别推进三类请求 ring FIFO。
4. `send_target_reqs()` 再按 `RD_REQ -> WR_REQ -> WR_DAT` 依次尝试发给 target。

这里没有单独的中心仲裁器对象参与主路径选人。

## 6. 响应路径中的局部顺序

响应路径同样采用局部队列顺序：

1. target 先把响应注入目标节点的响应 ring FIFO。
2. ring 再逐跳回传。
3. 到达 source node 后进入 master response FIFO。
4. 最后再送回 `PemBiu`。

其中读响应还会带 lane 维度：

- 每个 lane 独立 FIFO
- 每个 lane 独立回送

## 7. `tm_bus_arbiter` 当前的定位

`tm_bus_arbiter.h/.cc` 仍然保留在工程中，但在当前 ring 主路径里的定位已经变化：

- 它不再是主数据路径的核心依赖
- 更像是保留的扩展点

也就是说：

- 旧总线版本中，arbiter 是主路径核心
- 当前 ring 版本中，arbiter 是保留模块，不是主路径核心

## 8. 为什么不直接删掉 arbiter

保留它有两个好处：

1. 后面如果从 ring NoC-lite 往更复杂的 router 模型演进，可以直接在 router output arbitration 上复用这类模块化思路。
2. 如果以后又需要某些 node-local 优先级、QoS 或虚拟子网仲裁，也有明确的挂载点。

## 9. 后续可演进方向

如果后面要把 ring 继续往更真实的 NoC 推，可以考虑引入真正的 router 局部仲裁，例如：

- input-buffer to output-port 的 RR
- weighted RR
- age-based priority
- request/reply 分 subnet 后分别仲裁

到那时，`tm_bus_arbiter` 才会重新进入主路径核心位置。
