# 性能表现与硬件对齐度

## 文档目的

这页只回答两个问题：

1. 当前模型能看出哪些性能现象。
2. 当前模型和真实硬件大致对齐到什么层次。

## 当前模型能表达的性能行为

### 1. 多核并发下的竞争

当前模型能较好表达以下竞争：

- 多个 master 同时注入请求时的源端竞争
- 多个输入 port 争同一个 output port 时的 router 竞争
- `REQ / WR_DAT / RSP` 共享同一输出方向时的互相压制
- 多条事务在 target 入口前排队时的目标端竞争

这意味着它适合看：

- 哪个方向最拥塞
- 哪类流量最容易被压住
- 多核同时发压测时瓶颈首先出现在哪里

### 2. 多跳路径差异

当前模型保留了：

- ring 节点位置
- `LOCAL/NORTH/SOUTH/EAST/WEST` 端口
- `src_port -> dst_port` 的有向 link
- link latency 和逐拍发射

因此它能看出：

- 近端 target 和远端 target 的访问差异
- 拓扑映射不同时的路径长度差异
- hotspot 是否集中在某几个 router 或某几个方向

### 3. target 端瓶颈

`TmBusFlowCtrl` 和 `TmRingTargetPort` 仍然是这套模型里最关键的性能来源之一。它们能表达：

- outstanding/OSD 限制
- token/busy 限制
- target 侧 credit 限制
- 目标端请求排队

因此这套模型对以下问题比较敏感：

- interleave 配置是否均衡
- channel 数量变化是否真正有收益
- 某个 target 是否成为全局瓶颈

### 4. 写事务两阶段开销

这套模型保留了：

```text
WR_REQ -> WR_REQ_RSP(grant) -> WR_DAT -> WR_DAT_RSP
```

所以它能看出：

- 写事务在请求阶段和写数据阶段之间的等待
- grant 不足时的写吞吐下降
- 读写混跑时写路径被 request/rsp 挤压的现象

## 当前模型的性能精度边界

### 比较可信的结论

下面这些结论通常可以比较放心地用：

- 不同 interleave 方案的相对优劣
- 不同 ring 大小、节点映射的相对趋势
- 读多、写多、混合流量下的大方向瓶颈
- target 数、读返回 lane 数变化带来的大体收益
- 哪类流量占用了主要带宽

### 不要过度解读的结论

下面这些数值不建议直接当作硬件周期真值：

- 极细粒度单事务绝对时延
- 每一拍的真实物理线占用细节
- flit/beat 级返回顺序
- VC、credit、crossbar pipeline 带来的细微时序影响
- 某个 router 微结构优化带来的小幅收益

一句话说，当前模型更适合：

- 看趋势
- 看瓶颈
- 看方案对比

不适合：

- 做 RTL 级周期对拍

## 当前模型和硬件对齐到什么层次

## 已经比较对齐的部分

### 1. 网络基本结构

当前代码已经有：

- NIU
- Router
- Link
- TargetPort
- Topology

这和真实多 AI Core 互连在结构上是对得上的。虽然细节更粗，但“大框架”已经不是简单 bus，而是明确的 NoC-lite。

### 2. 端口化方向竞争

router 已经按：

- `LOCAL`
- `NORTH`
- `SOUTH`
- `EAST`
- `WEST`

拆开输入输出语义。真实硬件也是围绕方向 port 建模的，所以这一层的抽象是合理的。

### 3. 链路逐拍发射

link 现在不是“占一次锁整条边很多拍”，而是：

- 每拍最多发 1 个单位
- 经过 latency 后到达

这比旧版粗粒度 hop time 更接近真实硬件的流水化链路。

### 4. 目标端 backpressure

真实芯片里，很多性能问题并不来自 router 本身，而来自：

- HBM/DDR channel
- memory slice
- target queue
- outstanding 限制

当前模型对这部分保留得比较完整，所以这一层和硬件对齐度反而是比较高的。

## 仍然是抽象的部分

### 1. output port 仍是事务级仲裁

当前 output port 每拍只在 `REQ / WR_DAT / RSP` 之间选一个 winner，但发送单位还是“一个事务包”，不是 flit 或 beat。

这意味着：

- 竞争关系是对的
- 发送粒度仍偏粗

### 2. 没有 flit / VC / credit

这套模型没有显式表达：

- flit 拆分
- VC 分配
- credit 返回
- router pipeline 分级

所以它无法准确回答：

- 更细粒度的链路复用
- 更细粒度的 HOL 现象
- 微结构优化的绝对周期收益

### 3. 读返回 lane 仍是逻辑抽象

`rd_rsp_port_num` 代表的是“读返回能力”的逻辑 lane 数，不是一一对应某一根真实物理线。

它的好处是：

- 容易调参
- 容易做灵敏度分析

它的代价是：

- 不能直接解释成“芯片里一定有这么多根独立 response port”

### 4. NIU 仍是 message-buffer 风格

`TmRingInf` 仍然是：

- 上游请求先入本地 pending
- 后续再注入网络

这更接近系统级模型，而不是严格的逐拍端口握手 RTL。

## 推荐的对齐目标

如果你的目标是：

- SoC 级 CA/ESL
- 多 AI Core 负载对比
- 找关键瓶颈
- 做 80% 左右趋势建模

那么当前模型的对齐层次已经基本合适。

建议把它看成：

- 结构上对齐真实 NoC
- 性能上对齐主要瓶颈
- 时序上保留必要竞争
- 微结构上故意不做过细

## 建议怎样使用这套模型

### 适合做的事

- 比较 interleave 策略
- 比较不同 target 映射
- 比较不同 ring 规模
- 比较读写流量比例变化
- 看 router 方向竞争和 target 热点

### 不适合单独拿它下结论的事

- 判断某个 cycle 级微优化是否有效
- 判断某条 response 物理通道是否一定存在
- 直接给出和 RTL 一致的绝对延迟

## 一句话总结

当前模型已经足够接近“多 AI Core SoC 的轻量 NoC 性能模型”。它对结构、竞争、target 瓶颈和多跳路径的表达是可信的；它与真实硬件的主要差距，集中在 flit/VC/credit 和更细粒度的物理发送细节上。
