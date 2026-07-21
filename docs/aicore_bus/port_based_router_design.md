# Port-Based Router 设计

## 当前模型边界

当前 `TmRingRouter` 是双向 Ring 的轻量转发节点，方向集合固定为 `LOCAL`、`EAST` 和 `WEST`。旧文档中的 `NORTH/SOUTH` 属于历史 Mesh 方案，不是当前 `tm_ring_*` 实现。

端口化的目的，是明确 packet 从哪个方向进入、将从哪个方向离开，以及哪些输入会竞争同一个输出。Router 仍是事务/协议段级，不模拟 flit crossbar、Virtual Channel（VC）或逐 flit credit。

## 输入与连接

EAST/WEST 输入分别由对应上游 Link 连接到 `port_infs_`。LOCAL 不是 Router 内部的一组自有 FIFO，而是通过 `local_master_infs_` 和 `local_target_infs_` 引用 Master NIU 或 TargetPort 的接口队列。各接口按 command/lane 划分 channel，Router 从有效 channel 的队头选择候选。

```text
Master NIU / TargetPort -- LOCAL --+
                                  |
Upstream EAST Link ----- EAST ----+--> route/arb --> LOCAL/EAST/WEST
                                  |
Upstream WEST Link ----- WEST ----+
```

请求根据 `dst_node` 路由到 Target，响应根据 `src_node` 返回 Master。当前位置等于目标节点时走 LOCAL，否则 Topology 在 EAST/WEST 间选择最短方向；等距时当前规则选择 EAST。

## 当前仲裁状态

当前实现按 `in_dir × subnet` 独立推进，在每个输入内部通过 `input_rr_ptr_` 轮询 Traffic Class/lane。packet 解析输出方向、检查本地接口或 Link 容量后立即尝试提交。`output_rr_ptr_` 只记录提交历史，没有参与跨输入 winner 选择。

因此，有限 Link 可以形成竞争和反压，但多个输入同拍争一个输出时，winner 可能受事件 callback 执行顺序影响。当前版本不能保证严格输出 RR、bounded fairness，也没有旧文档曾描述的 `OutputArbDebug`。目标仲裁结构及验收条件见 [仲裁说明](./arbitration.md)。

## Link 绑定与传输

Link 绑定关系为：

```text
src_router.src_dir -> dst_router.opposite_dir
```

例如 `Router(3).EAST -> Router(4).WEST`。每个方向的 REQ 和 RSP subnet 分别维护 FIFO、inflight、`next_send_time` 和统计。

packet 进入 Link 后，以 `ceil(packet_bytes / link_width_bytes)` 计算序列化间隔，并在固定 `ring_link_latency` 后到达下游输入接口。传播延迟可以与后续 packet 的序列化流水重叠；Link FIFO 或 max inflight 耗尽时，反压返回 Router。该模型比单一 hop time 更接近流水链路，但仍不表达 packet 内 flit 交错。

## 目标状态与统计

Router 应改为每拍集中收集全部输入候选，再按 `(subnet, output_port)` 做 RR/WRR 或候选 iSLIP 仲裁。每个物理输出最多提交一个 winner，只有成功提交才弹出输入并推进仲裁指针。本地输出必须与 EAST/WEST 一样纳入资源约束。

每个 Router 输出至少统计 request、contention、grant、blocked grant、按 Master/Traffic Class 的 win、仲裁等待、最大连续等待和下游阻塞原因。Link 继续按方向和 subnet 统计 packet/byte、busy cycle、inflight peak、FIFO full 和下游 full。只有这些统计与放宽实验一起成立时，才能把某个 Router 输出或 Link 判定为关键瓶颈。

## 与真实硬件的距离

该抽象适合表达端口位置、多跳路径、包级输出竞争和链路流水。它不能直接回答 flit/VC 级队头阻塞、Router 微流水旁路、credit 往返或 RTL 单拍优化收益。是否增加 segment/flit/VC，必须由对标误差证明，而不是仅因 GPGPU-Sim Intersim2 提供这些机制就直接照搬。
