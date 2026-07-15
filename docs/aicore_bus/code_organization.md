# Ring 代码组织

当前 Ring 代码集中在 `BUS/aicore/` 下，主文件名是 `tm_ring_*`。设计目标是保持模块低耦合：上游、Router、Link、TargetPort 之间主要通过 `p_tm_com_inf_t` / `p_tm_com_que_t` 的 valid-ready 语义交互。

## 主模块

- `tm_ring.h`
  定义顶层 `TmRingFabric`。Fabric 持有 NIU、Router、Link、TargetPort、Topology 和 FlowCtrl。
- `tm_ring_core.cc`
  实现 Fabric 的创建、连接、attach、reset、idle 和 API 转发。
- `tm_ring_inf.h/.cc`
  Master 侧 NIU。负责接 BIU/API、缓存本地命令、补 Ring metadata、处理响应、维护 master outstanding。
- `tm_ring_router.h/.cc`
  Ring stop/router。负责从 `LOCAL/EAST/WEST` 输入端口取包，按路由方向转发到本地端口或左右 Link。
- `tm_ring_link.h/.cc`
  有向链路。负责 Link 输入、传播延迟、序列化、in-flight 限制和下游反压。
- `tm_ring_target_port.h/.cc`
  Target 侧 NIU。负责接收 Ring 请求、发送到 `TmMem`/target、接收 target 响应并注入本地 Router。
- `tm_ring_topology.h/.cc`
  负责 master/target 到 ring node 的映射、地址到 target 的解码，以及最短路径方向选择。
- `tm_ring_types.h`
  定义 Ring 配置、端口方向、subnet、命令到通道的映射和公共状态。
- `tm_pld.h/.cc`
  定义通用 payload。Ring 只把稳定字段放入 payload，例如 subnet、traffic class、RD response lane。
- `tm_bus_flow_ctrl.h/.cc`
  Target 侧 credit、token、outstanding、busy cycle 计算。
- `tm_ring_demo_top.cc`
  独立 demo top，用于把 BIU、Ring、Target/TmMem 串起来做最小测试。

## 代码阅读入口

1. 先看 `tm_ring.h` / `tm_ring_core.cc`，理解 Fabric 创建了哪些模块。
2. 再看 `tm_ring_inf.cc`，理解 BIU/API 请求如何进入 Ring。
3. 再看 `tm_ring_router.cc`，理解一个 ring stop 如何转发 `LOCAL/EAST/WEST` 输入。
4. 再看 `tm_ring_link.cc`，理解 Link 延迟、带宽和反压。
5. 再看 `tm_ring_target_port.cc`，理解请求如何进入 memory、响应如何回到 Ring。
6. 最后看 `tm_ring_topology.cc` 和 `tm_bus_flow_ctrl.cc`，理解路由和 target 流控。

## 当前事件风格

模型遵循 `tm_engine` 的事件驱动语义：

- 有数据进入 `TmInf` 或 `TmQue` 后，由 `vld` 事件触发对应处理函数。
- 下游可接收时才 `send` / `push_back` 并 `pop` 上游。
- 下游不能接收时不 pop，上游队头保留，依赖 TM FIFO 自身事件语义继续尝试。
- 不新增 `retry_event_`，不使用 `notify_after(1)` 做重试。

## 日志约定

Ring 主模块都带独立 log：

- Fabric：创建、连接、reset、attach。
- Inf：BIU/API 请求、注入 Ring、响应完成、写数据生成。
- Router：端口绑定、Link 绑定、route commit。
- Link：enqueue、drain、dst full。
- TargetPort：请求进入 target、发 memory 命令、响应回 Ring。

日志用于 debug 数据流，不替代 assert 或错误处理。
