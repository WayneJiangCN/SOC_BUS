# 事务模型说明

## 1. 文档范围

本文档描述 ring 版本 `TmBusFabric` 中事务的种类、状态和生命周期。

当前模型保留三类请求：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`

同时保留三类返回路径：

- `RD_RSP`
- `WR_REQ_RSP`
- `WR_DAT_RSP`

## 2. 为什么仍然保留三类请求

当前 ring 版本虽然把中心总线改成了多跳互连，但并没有改变 AI Core 侧事务协议本身。

`PemBiu` 这边仍然是：

- 读请求单独发起
- 写请求先发 `WR_REQ`
- 收到 grant/DBID 后再发 `WR_DAT`

因此 ring 网络只负责转发这些事务，不应把它们粗暴揉成一种统一 packet 语义。

## 3. 事务唯一标识

当前实现继续使用：

- `mst_id`
- `gid`

共同组成事务 key。

在代码里：

```text
txn_key = (mst_id << 32) | gid
```

这张 key 用来贯穿：

1. 请求进入 ingress FIFO
2. 请求注入 ring
3. 请求到达 target
4. 响应从 target 注入 ring
5. 响应回到 master
6. 事务 retire

## 4. 事务上下文

每笔事务在 ring 版本里都挂一条上下文 `TmBusTxnCtx`。

当前上下文里最关键的字段包括：

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

ring 版本相比旧总线版本多了：

- `src_node`
- `dst_node`

这两个字段让事务能在网络层逐跳前进，而不是只记录端点信息。

## 5. 当前主要状态

当前 ring 版本常用状态包括：

- `ALLOCATED`
- `IN_INGRESS_FIFO`
- `IN_REQ_RING`
- `IN_TARGET_FIFO`
- `WAIT_RD_RSP`
- `WAIT_WR_REQ_RSP`
- `WAIT_WR_DAT_RSP`
- `IN_RSP_RING`
- `DONE`

需要注意的是：

- `IN_REQ_RING` 表示请求已经进入 ring，正在多跳前推
- `IN_RSP_RING` 表示响应已经进入 ring，正在多跳返回

这是 ring 版本相对旧总线版本最明显的状态机变化。

## 6. 读事务路径

当前 `RD_REQ` 的生命周期可以写成：

```text
master inf
  -> master ingress FIFO
  -> source master node 注入 ring
  -> ring 逐跳前推
  -> 到达 dst target node
  -> target FIFO
  -> target inf
  -> target 返回读响应
  -> target node 注入响应 ring
  -> ring 逐跳回传
  -> source master node
  -> master response FIFO
  -> master inf
```

在事务状态上，大致对应：

```text
IN_INGRESS_FIFO
  -> IN_REQ_RING
  -> IN_TARGET_FIFO
  -> WAIT_RD_RSP
  -> IN_RSP_RING
  -> DONE
```

## 7. 写事务路径

当前写事务仍然分两段。

完整路径是：

```text
WR_REQ
  -> ingress FIFO
  -> 请求 ring
  -> target
  -> WR_REQ_RSP
  -> 响应 ring
  -> master
  -> grant FIFO 就绪
  -> WR_DAT
  -> ingress FIFO
  -> 请求 ring
  -> target
  -> WR_DAT_RSP
  -> 响应 ring
  -> master
```

对应状态大致是：

```text
WR_REQ:
IN_INGRESS_FIFO
  -> IN_REQ_RING
  -> IN_TARGET_FIFO
  -> WAIT_WR_REQ_RSP
  -> IN_RSP_RING

WR_DAT:
IN_INGRESS_FIFO
  -> IN_REQ_RING
  -> IN_TARGET_FIFO
  -> WAIT_WR_DAT_RSP
  -> IN_RSP_RING
  -> DONE
```

## 8. WR_DAT 为什么要等 grant

ring 版本没有改变写协议的这个基本约束：

- `WR_DAT` 不能仅因为自己在 FIFO 里就直接上网
- 它必须先匹配 `WR_REQ_RSP` 产生的 grant

因此当前实现里：

- `WR_REQ_RSP` 到达 target 后会先生成 grant
- grant 进入 `m_wr_grant_fifo_[master]`
- 只有 grant 匹配成功，`WR_DAT` 才允许从 master 侧注入 ring

也就是说，ring 网络改变了路径，不改变写协议阶段关系。

## 9. 响应返回

当前响应不再从 target 直接同步回 master，而是：

1. 先进入 target 节点的响应 ring FIFO
2. 再沿 ring 逐跳回到 `src_node`
3. 再进入 master 侧本地响应 FIFO
4. 最后送回 `PemBiu`

因此 ring 版本里，响应已经成为真正的网络事务，而不只是 target 完成后的局部动作。

## 10. 当前模型边界

当前事务模型仍然是 packet 级，而不是 flit 级：

- 一个 `p_tm_pld_t` 代表一个完整事务包
- 不拆 header flit / data flit
- 不做 VC
- 不做 packet 内部逐拍切片

这正是当前模型保持轻量化的关键边界。
