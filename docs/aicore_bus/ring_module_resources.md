# Ring 模块资源与连接关系

本文按当前 `tm_ring_*` 代码说明每个模块持有哪些资源，以及模块之间如何连接。目标是把“谁缓存、谁转发、谁负责协议状态”讲清楚，避免把所有逻辑揉进 Router 或 Fabric。

## 1. 总体结构

```text
BIU / API
  |
  | p_tm_com_inf_t
  v
TmRingInf
  |
  | router_inf_ <-> Router LOCAL master port
  v
TmRingRouter
  |
  | EAST/WEST output inf
  v
TmRingLink
  |
  | dst_out_inf_ -> next Router EAST/WEST input buffer
  v
TmRingRouter
  |
  | Router LOCAL target port <-> ring_inf_
  v
TmRingTargetPort
  |
  | inf_ <-> TmMem::rw_inf_
  v
TmMem / target
```

Ring 中有两个逻辑 subnet：

- `REQ subnet`：承载 `RD`、`WR`、`WR_DAT`。
- `RSP subnet`：承载 `RD_RSP`、`WR_RSP`、`RSP`。

`TmInf` 本身只作为 valid-ready 事件边界；`TmQue` 用于模块内部更明确的本地队列，例如 NIU 的命令队列、Router 的 EAST/WEST input buffer、TargetPort 的 request queue、Link 的 in-flight queue。

## 2. `TmRingFabric`

### 资源

- `clk_`：Ring 统一时钟。
- `cfg_`：Ring 配置。
- `master_nius_`：每个 master 一个 `TmRingInf`。
- `routers_`：每个 ring stop 一个 `TmRingRouter`。
- `links_`：有向 Link 表，key 由 `src_router/src_dir/dst_router/dst_dir` 组成。
- `target_ports_`：每个 target 一个 `TmRingTargetPort`。
- `topology_`：地址解码、master/target 节点映射、最短路径方向选择。
- `flow_ctrl_`：Target 侧 credit/token/outstanding。
- `log_`：Fabric 级 debug log。

### 职责

- 创建 NIU、Router、Link、TargetPort。
- 把 master、target、router、link 连接起来。
- 提供 API：`send_rd_req()`、`send_wr_req()`、`completed()`。
- 做全局 reset/idle 汇总。

### 不负责

- 不保存每跳 Router packet。
- 不做 Link 序列化。
- 不直接处理 Target memory handshake。
- 不作为大事务表保存所有生命周期。

## 3. `TmRingInf`

`TmRingInf` 是 master 侧 NIU，连接 BIU/API 和本地 source router。

### 资源

- `bus_inf_`：对上游 BIU/API 暴露的接口。
- `router_inf_`：连接本地 Router LOCAL master port 的接口。
- `rd_cmds_`：本地读命令队列。
- `wr_cmds_`：本地写命令队列。
- `wr_data_`：等待注入 Ring 的写数据队列。
- `wr_dat_rsp_q_`：等待返回给上游的写数据响应队列。
- `req_map_`：API 请求完成状态。
- `pending_writes_`：保存 API 写请求原始 payload，用于收到 `WR_RSP` 后生成新的 `WR_DAT`。
- `rd_rsp_states_`：多路读响应完成计数。
- `rd_outstanding_` / `wr_outstanding_`：master 侧 OSD。
- `topology_`：用于给请求打上 target、src node、dst node metadata。
- `flow_ctrl_`：用于响应完成时释放 target credit。
- `log_`：NIU 级 debug log。

### 连接

```text
BIU::out_intf_ <-> TmRingInf::bus_inf_
TmRingInf::router_inf_ <-> Router LOCAL master inf
```

### 请求方向

1. BIU 在 `bus_inf_` 对应通道发送 `RD` / `WR` / `WR_DAT`。
2. NIU 的 `recv_*` 函数把命令放入 `rd_cmds_`、`wr_cmds_` 或 `wr_data_`。
3. `send_*` 函数从本地队列取队头，补齐 `mst_id`、target、src/dst node、subnet、traffic class。
4. 如果本地 Router 端口可接收，则通过 `router_inf_->send()` 注入 Router。
5. 注入成功后才 pop 本地队列。

### 响应方向

1. Router 通过 `router_inf_` 把响应送回 NIU。
2. NIU `recv_router_rsp()` 根据 payload 命令分派：
   - `RD_RSP`：返回上游或完成 API read。
   - `WR_RSP`：写请求 grant；当前实现会用 `pending_writes_` 中的原始写请求 clone 出新的 `WR_DAT` 并放入 `wr_data_`。
   - `RSP`：写数据最终响应；释放 write OSD 和 target credit。
3. 事务完成后更新 `req_map_`、`rd_rsp_states_`、OSD。

## 4. `TmRingRouter`

`TmRingRouter` 是一个 ring stop。它只做短缓存、路由判断和转发，不做长期事务管理。

### 资源

- `port_infs_`：EAST/WEST 到站输入接口，承接相邻 Link 的 `dst_out_inf_`。
- `req_input_qs_`：EAST/WEST 的 REQ subnet 输入缓存。
- `rsp_input_qs_`：EAST/WEST 的 RSP subnet 输入缓存。
- `local_master_infs_`：本地 master ejection 接口。
- `local_target_infs_`：本地 target ejection 接口。
- `east_link_` / `west_link_`：左右两个有向 link。
- `topology_`：计算当前 router 到目标节点的下一跳方向。
- `output_rr_ptr_`：输出端口仲裁状态。
- `log_`：Router 级 debug log。

### 连接

```text
Router LOCAL port <-> TmRingInf::router_inf_
Router LOCAL port <-> TmRingTargetPort::ring_inf_
Router EAST/WEST output -> TmRingLink::accept_pkt()
Link dst_out_inf_ -> next Router port_inf(EAST/WEST)
Router port_inf(EAST/WEST) -> req_input_qs_ / rsp_input_qs_
```

### 转发规则

- 如果目的 node 是当前 router，走 `LOCAL` ejection：
  - request 送到本地 `TmRingTargetPort`。
  - response 送到本地 `TmRingInf`。
- 如果目的 node 不在当前 router，调用 `TmRingTopology::route_direction()`：
  - 顺时针更近走 `EAST`。
  - 逆时针更近走 `WEST`。
  - 等距走 `EAST`。

### 缓存边界

Router 的外部输入缓存来自 EAST/WEST 的 `req_input_qs_` 和 `rsp_input_qs_`。它只吸收到站 packet，帮助 Link 尽快释放 in-flight；长期等待、协议状态和事务完成状态不放在 Router。

## 5. `TmRingLink`

`TmRingLink` 是一个有向链路，连接一个 Router 输出端口和下一个 Router 输入端口。

### 资源

- `dst_out_inf_`：Link 发送到下游 Router 的出口接口。
- `inflight_packets_`：每个 subnet 一个 `TmQue`，表达传播延迟中的在途 packet。
- `inflight_count_`：当前每个 subnet 在途数。
- `next_send_time_`：链路序列化占用到什么时候。
- `width_bytes_`：每周期链路发送宽度。
- `latency_`：固定传播延迟。
- `stats_`：packet 数、byte 数、busy cycle、下游阻塞等统计。
- `log_`：Link 级 debug log。

### 连接

```text
Router output -> Link accept_pkt()
Link dst_out_inf_ -> next Router port_inf(EAST/WEST)
```

### 行为

1. Router 调用 `accept_pkt()` 把 packet 送入 Link。
2. Link 根据 `packet_bytes()` 和 `width_bytes_` 计算序列化周期。
3. Link 把 packet 放入 `inflight_packets_[subnet]`，等待固定传播延迟。
4. `drain_ready_packets()` 在 packet ready 后尝试发到 `dst_out_inf_`。
5. 下游 `dst_out_inf_` 满时不 pop，packet 保留在 Link FIFO，形成反压。

## 6. `TmRingTargetPort`

`TmRingTargetPort` 是 target/memory 侧 NIU。

### 资源

- `ring_inf_`：连接本地 Router LOCAL target port。
- `inf_`：连接下游 `TmMem::rw_inf_` 或 target 接口。
- `rd_req_q_`：读请求队列。
- `wr_req_q_`：写请求队列。
- `wr_dat_q_`：写数据队列。
- `next_req_issue_time_`：每类 request 下一次允许发往 target 的时间。
- `next_rd_rsp_issue_time_`：每个 RD response lane 的下一次返回 Ring 时间。
- `next_wr_req_rsp_issue_time_`：写 grant 响应节流。
- `next_wr_dat_rsp_issue_time_`：写完成响应节流。
- `flow_ctrl_`：Target credit/token/outstanding。
- `target_id_`：当前 target 编号。
- `log_`：TargetPort 级 debug log。

### 连接

```text
Router LOCAL target inf <-> TmRingTargetPort::ring_inf_
TmRingTargetPort::inf_ <-> TmMem::rw_inf_
```

### 请求方向

1. Router 将 `RD` / `WR` / `WR_DAT` 送入 `ring_inf_`。
2. TargetPort 将请求分别放入 `rd_req_q_`、`wr_req_q_`、`wr_dat_q_`。
3. `send_cmd()` 检查：
   - request queue 是否有数据。
   - `flow_ctrl_` 是否允许发。
   - `next_req_issue_time_` 是否到达。
   - 下游 `inf_` 是否能接收。
4. 发送成功后才 pop request queue，并消耗 target credit/token/outstanding。

### 响应方向

1. Target/TmMem 通过 `inf_` 返回响应。
2. TargetPort 根据 response 类型和 lane 注入本地 `ring_inf_`。
3. 响应路径使用 `next_*_rsp_issue_time_` 做 target response bandwidth throttle。
4. 下游 Router LOCAL port 不能接收时不 pop memory response。

## 7. `TmRingTopology`

### 资源

- `master_id_to_port_`：master id 到 master port 的映射。
- `port_to_master_id_`：master port 到 master id 的映射。
- `master_nodes_`：master port 对应 ring node。
- `target_nodes_`：target id 对应 ring node。
- 地址交织 helper：Topology 内部直接根据 target 配置计算 `LINEAR` / `XOR_HASH` slice。
- `router_count_`：Ring stop 数量。

### 职责

- `decode_target(addr)`：地址到 target id。
- `master_node(master_port)`：master port 到 source router。
- `target_node(target_id)`：target 到 destination router。
- `route_direction(cur, dst)`：当前 router 到目的 router 的下一跳方向。

## 8. `TmBusFlowCtrl`

`TmBusFlowCtrl` 不属于 Ring 拓扑，但它是 target 侧性能建模的核心。

### 管理内容

- target slot credit。
- target bandwidth token。
- target outstanding。
- target busy cycle。
- hotspot penalty。

### 使用位置

- `TmRingTargetPort` 发送 request 到 memory 前检查并消耗 flow-control 资源。
- `TmRingInf` 在完整 read/write transaction 完成后释放 target credit。

## 9. `TmPld` 中的 Ring metadata

当前 payload 中只保留相对稳定的 Ring 字段：

- `ring_subnet`：`REQ` 或 `RSP`。
- `ring_traffic_class`：对应 `PldCmd`。
- `ring_rsp_lane`：RD response lane。

逐跳变化的输入方向、输出方向不放在 payload 中，而由 Router 内部 `TmRingCandidate` 保存。这能避免 route ready 失败时提前污染 payload 状态。

## 10. 请求路径

### 读请求

```text
BIU/API
  -> TmRingInf::bus_inf_
  -> rd_cmds_
  -> TmRingInf::router_inf_
  -> Router LOCAL input
  -> Router EAST/WEST
  -> Link
  -> Target Router LOCAL
  -> TmRingTargetPort::ring_inf_
  -> rd_req_q_
  -> TmMem
```

### 写请求

```text
BIU/API WR
  -> TmRingInf::bus_inf_
  -> wr_cmds_
  -> Ring REQ subnet
  -> TmRingTargetPort::wr_req_q_
  -> TmMem
  -> WR_RSP grant
  -> Ring RSP subnet
  -> TmRingInf
  -> clone 原 WR payload 生成 WR_DAT
  -> wr_data_
  -> Ring REQ subnet
  -> TmRingTargetPort::wr_dat_q_
  -> TmMem
  -> RSP / write complete
```

## 11. 响应路径

### 读响应

```text
TmMem RD_RSP
  -> TmRingTargetPort::inf_
  -> response throttle
  -> TmRingTargetPort::ring_inf_
  -> Target Router LOCAL
  -> Link RSP subnet
  -> Master Router LOCAL
  -> TmRingInf::router_inf_
  -> TmRingInf::bus_inf_ 或 API completion
```

### 写响应

```text
TmMem WR_RSP / RSP
  -> TmRingTargetPort
  -> Ring RSP subnet
  -> TmRingInf
  -> WR_RSP 触发 WR_DAT
  -> RSP 释放 write outstanding 和 target credit
```

## 12. 反压规则

所有模块遵循同一条规则：

```text
下游可以接收：
    send / push 下游
    pop 上游

下游不能接收：
    不 send
    不 push
    不 pop
    保留队头
```

因此不需要额外 retry event。`TmInf` / `TmQue` 的 `vld`、`rdy` 事件负责继续驱动。

## 13. Debug log

每个主要模块都有独立 log：

- Fabric：`<fabric_name>.log`
- Inf：`<niu_name>.log`
- Router：`<router_name>.log`
- Link：`<link_name>.log`
- TargetPort：`<target_port_name>.log`

关键日志点包括：

- config / reset / attach。
- 请求进入 NIU、注入 Ring。
- Router route commit。
- Link enqueue / drain / dst full。
- TargetPort 接收 Ring request、发送 memory command、响应回 Ring。
- 事务完成和 OSD/credit 释放相关路径。
