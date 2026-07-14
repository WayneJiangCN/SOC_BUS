# 代码组织

## 主模块

- `BUS/aicore/tm_ring.h`
  顶层 `TmRingFabric`，持有 NIU、Router、Link、TargetPort、Topology 和 `txn_ctx_`。
- `BUS/aicore/tm_ring_core.cc`
  fabric 的配置、attach、reset、tick 和整体辅助函数。
- `BUS/aicore/tm_ring_inf.h/.cc`
  master 侧 NIU。负责上游请求接收、本地 pending、grant 管理和完成态查询。
- `BUS/aicore/tm_ring_router.h/.cc`
  端口化 router。负责 `LOCAL/NORTH/SOUTH/EAST/WEST` 输入队列、输出仲裁和仲裁统计。
- `BUS/aicore/tm_ring_link.h/.cc`
  端口到端口的有向链路。负责一拍一单位发射、链路延迟和在途包落地。
- `BUS/aicore/tm_ring_target_port.h/.cc`
  target 侧接入点。负责本地 target 队列和下游 target 接口。
- `BUS/aicore/tm_ring_topology.h/.cc`
  地址到 target 的映射、master/target 节点映射，以及下一跳方向计算。
- `BUS/aicore/tm_ring_req.cc`
  请求主线。负责 master 接收、NIU 注入、router 前推、链路回灌、target 发送。
- `BUS/aicore/tm_ring_rsp.cc`
  响应主线。负责 target 响应收集、响应重新注入 ring、最终回到 master。
- `BUS/aicore/tm_ring_types.h`
  ring 相关公共类型，包括 `TmRingTxnCtx`、port 方向、配置和 grant。
- `BUS/aicore/tm_bus_flow_ctrl.h/.cc`
  target 侧流控、OSD、busy/token/credit 相关逻辑。

## tick 顺序

`TmRingFabric::tick()` 的主顺序是：

1. `master_niu->tick()`
2. `flow_ctrl_.update_tokens(time())`
3. `recv_target_rsps()`
4. `recv_master_reqs()`
5. `inject_ring_reqs()`
6. `advance_ring_routers()`
7. `send_target_reqs()`

这条顺序对应一拍内的完整数据流：

```text
target 响应回灌
-> 上游请求吸收
-> NIU 注入 source router
-> router/link 前推
-> target 本地发送
```

## 主路径文件怎么读

- 先看 `tm_ring.h` 和 `tm_ring_core.cc`
  先建立“谁持有什么”的全局图。
- 再看 `tm_ring_inf.cc`
  先理解上游请求怎么进 NIU、本地 grant 怎么工作。
- 再看 `tm_ring_req.cc`
  请求和写数据怎么进 ring、怎么走 router、怎么到 target。
- 再看 `tm_ring_rsp.cc`
  target 回包怎么回 source router、怎么回 NIU。
- 最后看 `tm_ring_router.h/.cc`、`tm_ring_link.h/.cc`
  这是当前模型最接近互连微结构的部分。
