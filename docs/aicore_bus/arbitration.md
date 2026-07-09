# 仲裁说明

## 仲裁发生在哪里

当前 mesh 主线的仲裁不在 fabric 外层，也不在旧的 bus arbiter 中，而是在 `TmMeshRouter::pick_output_winner()`。

fabric 的工作是：

- 收集候选
- 按 output port 分组
- 调 router 的 RR 选择器选 winner
- 把 winner 发到 link 或本地出口

## 当前仲裁单位

仲裁的单位是“某个输入 port 上、某个 traffic class 的队头包”。

因此候选的粒度是：

```text
in_dir + traffic_class + front_packet
```

而不是：

- 整个 router 一个总队列
- 或 flit 级包片段

## 当前约束

当前实现固定有两层约束：

- 每个 output port 每拍最多选 1 个 winner
- 每个 input port 每拍最多成功发送 1 个单位

这两条一起保证了：

- 不会同一拍多个输入同时占用同一输出
- 不会同一拍同一个输入同时去多个输出

## 流量类之间的关系

`REQ`、`WR_DAT`、`RSP` 当前共享同一类 output port 资源。

这意味着：

- 同一 output port、同一拍
- 三者只能有一个 winner

它带来的好处是：

- 竞争关系清楚
- 结构简单

它带来的代价是：

- 相比更细分的物理子网，会更保守一些

## 统计信息怎么看

router 提供每个输出口的 `OutputArbDebug`，可以直接看：

- 这个输出口总共仲裁了多少次
- 有多少次真的发生了竞争
- 哪类流量最常出现在候选里
- 哪类流量最常赢

如果你在分析瓶颈，最有用的通常是：

- `contention_rounds`
- `req_wins`
- `wr_dat_wins`
- `rsp_wins`

它们能直接回答：

- 哪个方向在打架
- 谁更常抢到带宽
