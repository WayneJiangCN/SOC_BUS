# 仲裁说明

## 当前实现结论

当前 `TmRingRouter` 已经能形成共享 Link 的竞争，但尚未实现严格的跨输入、按输出端口统一仲裁。代码为每个输入方向和子网分别注册推进过程；`select_input_candidate(in_dir, subnet)` 只在该输入内部对 traffic class/lane 做轮询。候选通过 `route_ready()` 后立即尝试发送，因此多个输入在同一拍竞争一个输出时，实际 winner 可能由事件 callback 的执行先后决定。

`output_rr_ptr_` 当前只在成功提交后记录 `slot_id`，没有参与跨输入 winner 选择。代码中也没有旧文档曾描述的 `pick_output_winner()` 或 `OutputArbDebug`。因此当前版本不能宣称具备严格 Round-Robin（RR）、bounded fairness 或每输出口可证明的仲裁顺序。

## 当前实际流程

```text
每个 in_dir × subnet 独立触发
  -> 在该输入内部轮询 traffic class/lane
  -> 解析 LOCAL/EAST/WEST 输出
  -> 检查本地接口或 Link 是否可接受
  -> 立即发送并提交
  -> 更新 input_rr_ptr_ 和 output_rr_ptr_
```

Link 的 `next_send_time`、FIFO 和 inflight 限制通常会阻止同一有向 Link 在一个序列化窗口内接受多个 packet，所以能够产生链路竞争与反压；但这只是下游准入约束，不等价于 Router 在所有输入之间完成公平输出仲裁。本地输出也必须纳入统一 winner 选择，不能假设下游多 channel 接口天然等价于一个物理输出。

## 目标仲裁模型

V1 应将 Router 每拍处理改为四个原子阶段：

1. 从 LOCAL/EAST/WEST 的请求和响应输入收集队头候选。
2. 按 `(subnet, output_port)` 分组，形成每个输出端口的 request 集合。
3. 对每个输出应用可配置策略并检查下游容量。
4. 每个物理输出最多提交一个 winner；只有成功提交才推进仲裁指针并弹出输入。

V1 至少支持 `ROUND_ROBIN` 和 `WEIGHTED_RR`。如果需要同时处理多输入到多输出的匹配，可借鉴 GPGPU-Sim `LOCAL_XBAR` 的 iSLIP 组织，但不需要引入 flit/VC allocator。仲裁策略必须是配置项，配置单位、权重含义和指针更新时间必须固定。

## 仲裁单位与顺序

首版仲裁单位仍是“输入端某个 Traffic Class 的队头协议段/packet”，即：

```text
in_dir + subnet + traffic_class + lane + front_packet
```

请求、写数据和响应应保留独立 Traffic Class。REQ/RSP 如果对应独立物理子网，可以各自仲裁；如果最终硬件共享物理通路，则必须在更高一层合并带宽预算或共享输出仲裁，不能同时假设独立峰值。

同一 Master 的 ordering domain、两阶段写关系和多响应完成条件由 NIU/协议层维护，Router 不应凭地址或到达顺序重写事务完成语义。

## 必需统计与验证

每个 `Router × Output × Subnet` 至少输出：

- `request_rounds`、`contention_rounds`、`grants` 和 `blocked_grants`；
- 按 Master/Traffic Class 的 request、win 和服务字节；
- 仲裁等待周期总和、最大连续等待和等待分位数；
- 下游 FIFO 满、Link 忙和本地出口阻塞次数；
- RR/WRR 指针及配置权重。

验证必须覆盖两个及以上输入同拍争一个输出、不同输出并行、下游持续反压和恢复、长短 packet 混合、请求/响应混合及低权重流量无饥饿。验收条件包括：每个物理输出每拍最多一个 winner、无 callback 顺序依赖、RR 顺序可复现、WRR 长窗口份额符合权重，并且所有输入在下游可用时最终获得服务。

## 当前评审状态

该机制是多 Core 公平性和关键瓶颈可信度的 P0 阻塞项。在统一输出仲裁完成前，当前模型可用于功能流和链路饱和现象观察，但不应签核多 Master 的精确带宽份额、最坏等待时间或仲裁策略收益。
