# Ring Link 建模与改进说明

本文专门说明当前 `TmRingLink` 的职责、资源、数据流和后续改进方向。它是 `ring_module_resources.md` 的 Link 子文档，重点回答一个问题：

> Link 到底是在模拟一根线、一个 FIFO，还是一个带传播延迟和带宽约束的链路流水？

当前答案是：`TmRingLink` 是 **message-level 的单向链路流水模型**。它不把 packet 拆成 flit/beat，但会按 packet 大小占用链路带宽若干周期。

## 1. Link 在 Ring 中的位置

Ring 中每条 Link 都是单向的。

```text
Router i EAST 输出
  -> TmRingLink(i, EAST)
  -> Router i+1 WEST 输入

Router i WEST 输出
  -> TmRingLink(i, WEST)
  -> Router i-1 EAST 输入
```

所以 Link 不需要在运行时判断“谁是上游、谁是下游”：

- 上游由持有该 Link 的 Router 输出方向决定。
- 下游由 `dst_router_` 和 `dst_dir_` 在创建时固定。
- `attach()` 只负责把 Link 的出口 `dst_out_inf_` 连接到下游 Router 的 EAST/WEST 输入接口。

## 2. 当前 Link 的核心资源

`TmRingLink` 当前主要资源如下：

| 资源 | 作用 |
| --- | --- |
| `inflight_packets_` | 每个 subnet 一个 `TmQue`，表达已经离开上游 Router、正在链路上传播的 packet。 |
| `next_send_time_` | 每个 subnet 的发送器下一次可接收新 packet 的时间，用于表达序列化带宽占用。 |
| `inflight_count_` | 每个 subnet 当前在途 packet 数。 |
| `link_capacity_` | Link 每个 subnet 的容量上限，当前由 `ring_link_latency + 1` 推导。 |
| `dst_out_inf_` | Link 到下游 Router 的 valid-ready 出口接口。 |
| `stats_` | 记录 packet 数、字节数、busy cycle 和 stall 原因。 |

这里要特别区分：

- `inflight_packets_` 是 Link 的真实建模队列，用于表达传播延迟和在途状态。
- `dst_out_inf_` 只是出口握手接口，不应被理解成额外的链路缓存。
- 下游 Router 的 EAST/WEST input buffer 才是 packet 到站后的 Router 本地缓存。

## 3. 请求进入 Link 的流程

Router 仲裁出 winner 后，不是通过 `TmInf` 直接把 packet 送到下游 Router，而是交给 Link：

```text
Router
  -> link->can_accept(pld)
  -> link->accept_pkt(pld)
  -> inflight_packets_[subnet]
  -> dst_out_inf_->send(...)
  -> 下游 Router port_inf(EAST/WEST)
  -> Router input buffer
```

Link 接收上游 packet 时检查的是 Link 自己是否有资源：

- 当前时间是否达到 `next_send_time_`。
- `inflight_count_` 是否小于 `link_capacity_`。
- `inflight_packets_` 是否未满。

它不会在接收时要求下游 Router 立刻 ready。原因是 Link 模拟的是有传播延迟的流水，packet 进入链路时还没有到达下游。

## 4. 下游反压如何生效

packet 进入 `inflight_packets_` 后，会在 `latency_` 周期后变为 valid。`drain_ready_packets()` 再尝试通过 `dst_out_inf_` 发给下游 Router 输入接口，Router 随后把到站 packet 搬入 EAST/WEST input buffer。

如果下游 Router 当前不能接收：

```text
dst_out_inf_->send(...) == false
```

则 Link 会：

- 不 pop `inflight_packets_` 队头。
- 不释放 `inflight_count_`。
- 记录 `downstream_inf_full_stall`。
- 依赖 `TmQue` 的 valid 事件继续调度。

这就是当前模型中的链路反压。

Router input buffer 的作用是让已经到站的 packet 尽快离开 Link，避免 Router 仲裁慢时长期占住 Link in-flight 资源。

## 5. Link 如何模拟 128B 经过 16B 链路

当前模型不会把一个 128B packet 拆成 8 次 `send()`。它采用 message-level 序列化：

```cpp
serialization_cycles =
    max(1, ceil(packet_bytes / ring_link_width_bytes));
```

例如：

```text
packet size = 128B
link width  = 16B/cycle
serialization_cycles = 8 cycles
```

Link 会一次性保存完整 packet，但让该 subnet 的发送资源忙 8 个周期：

```text
next_send_time_[subnet] = now + serialization_cycles
```

这样能模拟：

- 大 packet 占用链路更久。
- 后续 packet 被链路带宽反压。
- REQ/RSP subnet 可以分别统计链路拥塞。

但不模拟：

- flit/beat 逐个到达。
- packet 内部 backpressure。
- VC 和 flit 级仲裁。

## 6. packet_bytes 的计算规则

不同命令在链路上占用的字节数不同：

| 命令 | 字节数来源 |
| --- | --- |
| `RD` | 固定请求头大小，当前模型内部按 16B 计算。 |
| `WR` | 固定请求头大小，当前模型内部按 16B 计算。 |
| `WR_DAT` | 写数据 payload 大小 `pld->size`。 |
| `RD_RSP` | 读返回 payload 大小 `pld->size`。 |
| `WR_RSP` | 固定响应头大小，当前模型内部按 16B 计算。 |
| `RSP` | 固定响应头大小，当前模型内部按 16B 计算。 |

这样可以避免把 `RD` 请求错误地当成完整读数据包处理。

## 7. 当前 capacity 语义

当前采用：

```cpp
link_capacity_ = max(1, ring_link_latency + 1);
```

含义是：

- `ring_link_latency` 表达传播流水深度。
- 额外的 `+1` 表示链路末端弹性位置，避免 `latency=1` 时 capacity 也只有 1。

当：

```text
ring_link_latency = 1
```

得到：

```text
link_capacity_ = 2
```

这比 `capacity=1` 更安全，因为本地注入可以保留一个空泡位置，降低 Ring 被本地注入填满后的环形等待风险。

## 8. bubble 保护的现状

当前代码对本地注入使用更保守的入口检查：

```text
LOCAL input -> can_accept_preserving_bubble()
EAST/WEST input -> can_accept()
```

这样做的含义是：

- 本地新注入的流量不能轻易把 Link 填满。
- 已经在 Ring 上的转发流量优先保证继续前进。

这是一种轻量 Ring 防堵策略，不是严格的 NoC 死锁证明。

## 9. 为什么不直接删除 Link

可以把 Router 之间直接用 `TmInf delay` 连接，但那会丢失以下能力：

- 按 packet 大小计算序列化占用。
- 区分 REQ/RSP subnet 的链路统计。
- 记录 link busy cycle。
- 记录 link inflight peak。
- 记录 downstream full、serialization busy、inflight limit 等瓶颈。
- 把链路行为从 Router 中解耦出来。

因此在当前轻量 CA/ESL Ring 目标下，保留 `TmRingLink` 更合适。

## 10. 当前不足

当前 Link 模型还有这些不足：

1. 不是 flit/beat 模型。

   128B packet 只会一次性进入 `inflight_packets_`，不会拆成多个 16B beat 逐次 `send()`。

2. bubble 规则只保护本地注入。

   EAST/WEST 转发仍可占满 Link capacity。这是为了保证在途包继续前进，但不等于完整的死锁规避协议。

3. `dst_out_inf_` 仍有 TmInf 自身 depth/delay 语义。

   设计上它只是出口握手接口，不应承担额外链路容量建模。Router input buffer 才是到站后的显式缓存。后续如果发现多打一拍，需要继续收敛内部 TmInf 的使用方式。

4. capacity 由 `latency + 1` 推导，缺少独立校准自由度。

   当前是简洁优先。如果后续需要拟合 RTL/平台结果，可能要重新引入 `ring_link_capacity`，但不建议恢复多套 `fifo_depth/max_inflight` 配置。

5. 下游满时依赖 `TmQue` valid 事件继续调度。

   这符合当前工程“不增加 retry event”的原则，但需要通过压力测试确认不会因事件语义导致队头长期不再触发。

## 11. 后续改进建议

### P0：保持当前 message-level Link，补齐验证

- 单跳 `latency=1`，确认 `link_capacity_=2` 不会卡住。
- 多 master 同时注入，确认 `bubble_reserved_stall` 有统计。
- 下游 Router 输入满，确认 Link 不丢包、不 pop 队头。
- 128B / 16B 链路宽度，确认下一个 packet 至少等待 8 cycle。

### P1：明确 Link 出口接口语义

- 文档和注释中统一称 `dst_out_inf_` 为出口握手接口。
- 避免把 `dst_out_inf_` 描述成 Link 内部缓存。
- 如果需要更精确控制内部 delay，考虑让 Ring 内部 `TmInf` 只作为事件边界。

### P2：增强死锁规避策略

- 继续保留本地注入 bubble 限制。
- 增加压力场景验证：全 master 热点、全环同方向、请求响应混合。
- 如果仍有环形等待，再考虑 escape lane 或更严格的 injection throttle。

### P3：可选 beat/flit 模型

只有当需要更接近 RTL AXI beat 或 NoC flit 行为时，才考虑：

- 把 `WR_DAT` / `RD_RSP` 拆成多个 beat。
- 每个 beat 单独占用 Link。
- 增加 packet reassembly。
- 增加更细的 backpressure 语义。

这会明显增加复杂度，当前阶段不推荐。

## 12. 推荐结论

当前阶段建议保持：

```text
Router 负责仲裁和下一跳选择
Link 负责传播延迟、序列化、in-flight 和下游反压
TargetPort/Inf 负责协议状态和事务完成
```

Link 不应被删除，也不应下沉成 flit 模型。更合理的路线是继续保持轻量 message-level Link，并用测试把 `latency + 1` capacity、bubble 保护和 backpressure 行为验证清楚。
