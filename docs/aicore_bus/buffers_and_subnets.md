# Buffer 与 Subnet 说明

## 1. 文档范围

本文档描述当前 ring 版本 `TmBusFabric` 中所有关键 FIFO 的放置位置，以及请求/响应逻辑子网的组织方式。

## 2. 为什么 ring 版本更依赖显式 FIFO

在中心总线版本里，很多阻塞还能被理解为“抢不到 target”。

到了 ring 版本，阻塞来源明显变多：

- source 节点注入失败
- 中途某个 node 的下一跳满
- target 本地 FIFO 满
- 响应回程节点堵塞
- master 侧响应 FIFO 满

因此 ring 版本必须把各层 FIFO 显式放出来，否则很难解释 backpressure 是如何传播的。

## 3. 当前 FIFO 分层

当前 ring 版本里，FIFO 可以分成五层。

### 3.1 Master Ingress FIFO

每个 master 都有三类本地入口 FIFO：

- `m_rd_req_fifo_`
- `m_wr_req_fifo_`
- `m_wr_dat_fifo_`

作用是：

- 吸收 `PemBiu` 的本地突发
- 解耦 source 端时序与 ring 注入时机

### 3.2 Ring Request FIFO

每个 ring node 都维护请求侧 FIFO：

- `n_rd_req_fifo_`
- `n_wr_req_fifo_`
- `n_wr_dat_fifo_`

这表示：

- ring 上每个节点都能暂存路过的请求
- 请求不再是“看见目的端就瞬间到达”，而是要经过显式中间缓存

### 3.3 Target Local FIFO

每个 target 仍然保留本地接收 FIFO：

- `t_rd_req_fifo_`
- `t_wr_req_fifo_`
- `t_wr_dat_fifo_`

作用是：

- 让网络到达和 target 实际发射解耦
- 保持 target credit/busy time 模型清晰

### 3.4 Ring Response FIFO

每个 ring node 都维护响应侧 FIFO：

- `n_rd_rsp_fifo_`
- `n_wr_req_rsp_fifo_`
- `n_wr_dat_rsp_fifo_`

注意读响应还按 lane 再拆一层：

- `n_rd_rsp_fifo_[node][lane]`

这表示响应已经被建模成真正的回程网络流量。

### 3.5 Master Response FIFO

每个 master 侧还维护本地响应 FIFO：

- `m_rd_rsp_fifo_`
- `m_wr_req_rsp_fifo_`
- `m_wr_dat_rsp_fifo_`

它们负责：

- 吸收 ring 回程流量
- 与 `PemBiu` 的接收节奏解耦

## 4. Grant FIFO

除了请求/响应 FIFO 之外，当前实现还保留一个非常关键的专用 FIFO：

- `m_wr_grant_fifo_`

它不是网络缓存，而是写事务上下文缓存。

作用是：

- 保存 `WR_REQ_RSP` 产生的 grant/DBID
- 约束后续 `WR_DAT` 只能在 grant 匹配时注入 ring

如果没有这层 FIFO，写请求和写数据之间的协议关系就会丢失。

## 5. 当前逻辑子网

当前版本虽然没有把 `ReqSubnet`、`RspSubnet` 写成显式类对象，但逻辑上已经形成了两个子网：

### 5.1 Request Subnet

请求子网承载：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`

### 5.2 Response Subnet

响应子网承载：

- `RD_RSP`
- `WR_REQ_RSP`
- `WR_DAT_RSP`

所以当前版本已经不是“所有包共用一套单队列”的结构，而是：

```text
请求侧一套 ring FIFO
响应侧一套 ring FIFO
```

## 6. 为什么请求和响应必须分开

如果 ring 里把请求和响应全揉在一起，会有几个明显问题：

1. 写数据流量容易压住响应回程。
2. 读响应容易被请求洪峰拖慢。
3. 更容易出现严重 HOL blocking。
4. 后面很难演进到更真实的 NoC 子网模型。

因此当前版本虽然还是轻量模型，但已经有意识地把 request 和 response 分成两张逻辑网。

## 7. 当前版本的限制

当前版本的 subnet 仍然是逻辑分离，不是完整物理子网对象：

- 还没有独立 `ReqSubnet` 类
- 还没有独立 `RspSubnet` 类
- 还没有 per-subnet 独立 router 模块

但从结构上讲，已经为后面继续细化留好了空间。
