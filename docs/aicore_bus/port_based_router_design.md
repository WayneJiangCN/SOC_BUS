# Port-Based Router 设计

## 为什么要端口化

旧版 router 只表达“当前 router 把包推进到下一个 router”。新版 router 明确引入方向 port，是为了把以下三件事拆清楚：

- 包从哪个输入方向进入
- 包想从哪个输出方向离开
- 同一拍哪些输入在竞争同一个输出

当前方向集合固定为：

- `LOCAL`
- `NORTH`
- `SOUTH`
- `EAST`
- `WEST`

## Router 内部状态

每个输入 port 都持有自己的本地队列：

- `REQ`
- `WR_DAT`
- `WR_REQ_RSP`
- `WR_DAT_RSP`
- `RD_RSP(lane)`

这意味着当前 router 的状态不再是“一个总队列”，而是“按输入方向拆开的多类队列”。

## 输出口仲裁

当前模型的仲裁语义是：

- 每个 output port 每拍最多选 1 个 winner
- 同一个 input port 每拍最多成功发送 1 个单位
- `REQ / WR_DAT / RSP` 共用同一类 output port 资源

这是一种典型的 NoC-lite 抽象：

- 保留方向竞争
- 保留请求/数据/响应的相互影响
- 不继续下沉到 flit/crossbar/VC

## Link 绑定方式

链路不再只是 `src_router -> dst_router`，而是：

```text
src_router.src_dir -> dst_router.dst_dir
```

例如：

```text
Router(3).EAST -> Router(4).WEST
```

这样可以明确表达：

- 当前包是从哪个输出方向发出的
- 到达下一跳时落到哪个输入方向队列

## 链路发射语义

当前 `TmMeshLink` 的语义是：

- 每条有向 link 每拍最多发 1 个单位
- 发射后包进入 in-flight 队列
- 经过 `latency` 拍后，包落到目标 router 对应输入口

它不再使用“占一次锁整条边很多拍”的粗粒度做法，而是更接近：

- 逐拍发射
- 延迟后到达

## 调试与统计

`TmMeshRouter` 为每个 output port 保存 `OutputArbDebug`。当前统计重点是：

- 仲裁轮数
- 发生竞争的轮数
- `REQ / WR_DAT / RSP` 各自出现为候选的轮数
- `REQ / WR_DAT / RSP` 各自获胜次数
- 最近一次 winner 来自哪个输入方向、哪个 class

这组统计的用途是：

- 看哪个方向最忙
- 看哪类流量最容易被压制
- 看竞争主要发生在 request、write data 还是 response

## 和真实硬件的距离

当前 port-based router 已经比旧版更接近硬件，但仍然是事务级模型。它表达的是：

- 端口方向
- 输出竞争
- 链路逐拍发射

它还没有表达的是：

- flit
- per-flit crossbar
- VC/credit
- router pipeline 分级

所以它更适合 SoC 级性能模型，而不是 RTL 对拍。
