# Ring 建模方案

## 1. 建模定位

当前 `tm_ring_*` 的目标不是做一个完整硬件 NoC，而是做一版：

**transaction/message-level ring NoC-lite**

它更接近：

- gem5 `SimpleNetwork` 的 message-buffer endpoint 思路
- AI Core 自己的事务协议语义

而不是：

- gem5 `Garnet` 的 flit/VC/credit 级网络

## 2. 为什么适合当前目标

它适合当前项目，是因为你要解决的是：

- 多 core 并发
- 多 target / 多 channel
- interleave
- target 侧 OSD / outstanding
- 瓶颈趋势

而不是：

- router 微结构研究
- VC 数量优化
- credit deadlock 分析

## 3. 当前结构

```text
Core / BIU
  -> TmRingInf
  -> TmRingRouter
  -> TmRingLink
  -> TmRingRouter
  -> TmRingTargetPort
  -> TmMem / target
```

### 3.1 `TmRingInf`

角色：

- master-side NIU
- message-buffer endpoint

负责：

- 上游接口
- 本地 request pending
- 本地 grant
- completion 跟踪
- response 回送

### 3.2 `TmRingRouter`

角色：

- 粗粒度 transaction/message 级 router

负责：

- 本地 request/data/response queues
- 同一输出口上的本地 RR 选择

不负责：

- flit
- VC
- credit
- crossbar 微结构

### 3.3 `TmRingLink`

角色：

- 轻量有向 link

负责：

- hop latency
- 共享输出节流

当前更像 `SimpleNetwork::Throttle` 的粗粒度链路，而不是 `Garnet CreditLink`。

### 3.4 `TmRingTargetPort`

角色：

- target-side endpoint / ingress port

负责：

- target-local queues
- 下游 target/TmMem 接口
- target response 回注入

### 3.5 `TmRingFabric`

角色：

- 共享容器和统一调度骨架

负责：

- 拥有 NIU / Router / Link / TargetPort
- 维护 `txn_ctx_`
- 驱动主 tick
- 组织 request / response 主路径
- 调用 target-level flow control

## 4. 数据路径

### 4.1 请求路径

```text
upstream / API
  -> TmRingInf pending
  -> request/data subnet
  -> Router
  -> Link
  -> Router
  -> TargetPort queue
  -> target / TmMem
```

当前子网划分：

- request subnet：`RD_REQ + WR_REQ`
- data subnet：`WR_DAT`

### 4.2 响应路径

```text
target / TmMem
  -> TargetPort
  -> response subnet
  -> Router
  -> Link
  -> Router
  -> TmRingInf
  -> upstream
```

当前响应语义：

- `RD_RSP`
- `WR_REQ_RSP`
- `WR_DAT_RSP`

## 5. 当前 router 为什么不向 Garnet 靠齐

当前不建议向 Garnet 全面靠齐，原因是：

- 你的目标是 SoC 级互连模型，不是 NoC 微结构模型。
- 一旦下沉到 flit/VC/credit，会明显增加复杂度。
- AI Core 协议语义会被网络细节稀释。

建议只向 Garnet 靠齐“形状”，不要靠齐“粒度”。

也就是保留：

- NIU
- Router
- Link
- TargetPort
- Fabric

但不引入：

- flit
- VC
- InputUnit / OutputUnit
- SwitchAllocator / CrossbarSwitch
- credit link

## 6. 和 SimpleNetwork 的关系

当前更适合的对齐对象是：

**SimpleNetwork 的 endpoint + switch/throttle 思路**

对应关系：

- `TmRingInf`
  类似 message-buffer endpoint
- `TmRingRouter`
  类似粗粒度 switch
- `TmRingLink`
  类似输出 throttle / link timing

这比 Garnet 更贴近你现在的目标层次。

## 7. 模型边界

当前建议把模型边界明确成：

- 支持多 core、多 target、多 channel
- 支持可配 interleave、延迟、OSD、busy time
- 支持主瓶颈分析
- 不追求 router 微结构逐周期准确

这就是当前模型最合理的收敛边界。
