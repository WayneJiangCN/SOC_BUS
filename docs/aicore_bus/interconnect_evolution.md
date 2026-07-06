# 互连模型演进对照

## 1. 文档目的

本文档用于对照四类互连模型：

1. gem5 `NoncoherentXBar`
2. gem5 `CoherentXBar`
3. gem5 `Ruby/Garnet`
4. 当前 `BUS/aicore` 下实现的事务级 `ring NoC`

重点不是逐行解释源码，而是回答三个问题：

1. 它们各自解决什么问题
2. 它们的复杂度是怎么上去的
3. 你当前 AI Core 项目应该处在什么位置

## 2. 一句话概括

- `NoncoherentXBar`：无一致性的事务级交叉开关
- `CoherentXBar`：加入 snoop 和一致性流量的事务级交叉开关
- `Ruby/Garnet`：多跳、router/link/flit/credit 级网络
- 当前 `ring NoC`：保留事务级包语义的轻量多跳网络

## 3. 总体演进关系

可以把这几类模型理解成下面这条演进线：

```text
单跳无一致性总线
    -> 单跳一致性交叉开关
        -> 多跳事务级 NoC-lite
            -> 多跳 flit/VC 级 NoC
```

对应关系如下：

- `NoncoherentXBar`
  对应“单跳无一致性总线”
- `CoherentXBar`
  对应“单跳一致性交叉开关”
- 当前 `ring NoC`
  对应“多跳事务级 NoC-lite”
- `Ruby/Garnet`
  对应“多跳 flit/VC 级 NoC”

## 4. 对照表

| 维度 | `NoncoherentXBar` | `CoherentXBar` | `Ruby/Garnet` | 当前 `ring NoC` |
|---|---|---|---|---|
| 抽象层级 | 事务级 | 事务级 | flit / VC / router 级 | 事务级 |
| 拓扑 | 单跳 `xbar` | 单跳 `xbar` | 任意 NoC 拓扑 | 单向 ring |
| 竞争点 | `per-target layer` | `per-target layer` + snoop layer | `input/output port`, VC, switch | 每个 ring 节点的输入 FIFO 和下一跳链路 |
| 路由方式 | 地址译码到 target | 地址译码到 target | router 逐跳路由 | `addr -> target -> dst node -> 逐跳前推` |
| 流控 | busy/retry | busy/retry + snoop 特殊路径 | credit/link backpressure | FIFO backpressure + hop latency + target credit |
| 写事务拆分 | 通常不显式拆 `WR_DAT` | 通常不显式拆 `WR_DAT` | 可按消息类拆 | 明确拆 `WR_REQ / WR_DAT / RSP` |
| 一致性支持 | 无 | 有 | 可建模完整一致性网络 | 无 |
| 可扩展性 | 中 | 中偏低 | 高 | 中高 |
| 实现成本 | 低 | 中 | 高 | 中 |
| 适合阶段 | ESL/CA bring-up | CPU cache coherence 研究 | 高精度 NoC 研究 | 多 AI Core 事务级互连 |

## 5. `NoncoherentXBar` 的做法

gem5 `NoncoherentXBar` 的关键特点是：

1. 仍然是单跳
2. 仍然是事务级
3. 竞争按 `target` 维度发生
4. 不显式建二维 crossbar matrix，而是建 `per-target layer`

它的复杂度主要来自：

- 地址译码
- request / response 回程
- layer busy / retry

因此它本质上还是“高级一点的总线”，不是网络。

对 AI Core 场景的启发是：

- 很适合做单跳事务级 SoC fabric
- 很适合作为第一版轻量模型参考
- 但不适合直接表达多跳拓扑

## 6. `CoherentXBar` 的做法

`CoherentXBar` 不是把拓扑做得更复杂，而是把“流量种类”做得更复杂。

它在 `NoncoherentXBar` 的基础上增加：

- snoop
- snoop response
- snoop filter
- express snoop

所以它的复杂度上升路径是：

```text
无一致性 request/response
    -> 一致性 request/response/snoop
```

而不是：

```text
单跳
    -> 多跳
```

因此，对 AI Core 多核总线来说，它的借鉴点主要在：

- 事务级控制骨架
- 不同流量类型分层

而不是完整照搬其一致性逻辑。

## 7. `Ruby/Garnet` 的做法

`Ruby/Garnet` 复杂度真正上去的方式，是直接从“总线/交叉开关”切到“网络”。

它的关键对象是：

- router
- link
- flit
- VC
- credit

它解决的问题包括：

- 多跳路由
- router 内部竞争
- 链路带宽
- buffer 占用
- credit backpressure
- 虚通道隔离

所以 `Ruby/Garnet` 的复杂度不是来自协议通路变多，而是来自：

```text
节点内部结构 + 节点间链路 + 多跳传播
```

这也是为什么一旦需求进入：

- mesh
- torus
- router-based ring
- flit/VC 研究

gem5 就不再继续用 classic `XBar`，而是切到 Ruby 网络体系。

## 8. 当前 `ring NoC` 的定位

你现在这版 `ring NoC` 的定位，介于 `CoherentXBar` 和 `Ruby/Garnet` 之间。

它的特点是：

1. 已经不是单跳
2. 已经引入节点和逐跳转发
3. 仍然保持事务级包语义
4. 不拆 flit，不做 VC
5. 仍然保留 `PemBiu / TmMem` 现有接口风格

可以把它理解成：

**NoC-lite**

也就是：

- 借了网络的“多跳拓扑”思想
- 但没有进入 `Garnet` 那种细粒度复杂度

## 9. 当前 `ring NoC` 和 gem5 几类模型的关系

### 9.1 相比 `NoncoherentXBar`

当前 `ring NoC` 多了：

- 多跳路径
- ring 节点 FIFO
- hop latency
- request ring / response ring

少了：

- 中心化 `per-target layer` 仲裁骨架

### 9.2 相比 `CoherentXBar`

当前 `ring NoC`：

- 多了多跳拓扑
- 少了一致性和 snoop

### 9.3 相比 `Ruby/Garnet`

当前 `ring NoC`：

- 保留事务级包，不拆 flit
- 没有 VC
- 没有 router allocator
- 没有 credit link 级精细模型
- 实现成本明显更低

## 10. 什么时候该从总线升级到 NoC

如果出现以下信号，就说明单跳总线开始不够了：

1. target 数量明显增多
2. 请求不再主要集中在单个共享 memory port
3. 你开始关心路径长短，而不仅是目标端口拥塞
4. 你需要表达 cluster/local/global 分层互连
5. 你怀疑性能问题来自多跳传播而不是单点仲裁

这时就应该考虑从 `XBar` 风格升级到 `ring/mesh` 风格。

## 11. 什么时候该从事务级 NoC 升级到 flit/VC NoC

如果出现以下信号，事务级 `ring NoC` 也会开始不够：

1. 你要研究 router 内部堵塞
2. 你要研究虚通道
3. 你要研究 link utilization
4. 你要研究 packet 被切成多个传输单元后的行为
5. 你需要更精确地拟合真实芯片 NoC

这时才有必要往 `Ruby/Garnet` 那一类复杂度推进。

## 12. 对当前 AI Core 项目的建议

对于你当前这个多 AI Core 项目：

- 如果目标是先把互连从“总线”升级到“网络”
  当前事务级 `ring NoC` 是合理的一步
- 如果目标是快速验证多核互连和热点传播
  当前 `ring NoC` 也比单跳总线更贴近真实扩展行为
- 如果后面核数继续上升，且要更像大规模片上网络
  下一步建议从 ring 演进到 mesh
- 如果后面要做高精度网络研究
  再考虑 `Garnet` 那一档复杂度

## 13. 你当前的位置

如果只看模型演进位置，你当前项目大致处在这里：

```text
NoncoherentXBar
    -> CoherentXBar
        -> 当前事务级 ring NoC   <- 你现在在这里
            -> Ruby/Garnet
```

也就是说：

你已经从“复杂总线”跨到了“轻量网络”，但还没有进入“高精度 NoC”。

这对当前 AI Core SoC 建模来说，是一个合理的平衡点。
