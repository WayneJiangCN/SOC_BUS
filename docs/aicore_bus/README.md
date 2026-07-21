# AI Core Ring-Lite 文档索引

这套代码是一个面向多 AI Core SoC 的轻量 Ring 互连模型。当前主线是 **message-level bidirectional Ring**，不是 flit/VC 级 NoC，也不是单层共享 bus。

历史上部分文件曾使用 `Mesh` 命名，但当前 `tm_ring_*` 已经是主线实现。

## 主路径

```text
BIU / API
  -> TmRingInf
  -> TmRingRouter
  -> TmRingLink
  -> TmRingRouter
  -> TmRingTargetPort
  -> TmMem / target
  -> response subnet 按 Ring 返回 Master
```

## 推荐阅读顺序

1. [AI Core Ring-Lite 总线模型方案与架构](../ai_core_bus_v1_design.md)
   先了解模型定位、总体架构、事务路径、流控机制和性能精度边界。
2. [AI Core 与 GPGPU-Sim 互连对比及需求收敛方案](../ai_core_interconnect_selection.md)
   对照 `LOCAL_XBAR` 与 `INTERSIM/Intersim2`，了解可借鉴机制、当前差距和分阶段验收门。
3. [code_organization.md](./code_organization.md)
   先看代码分层、主要文件和阅读入口。
4. [ring_module_resources.md](./ring_module_resources.md)
   重点看每个模块持有哪些资源，以及模块之间如何连接。
5. [ring_fabric_design.md](./ring_fabric_design.md)
   再看 `TmRingFabric` 如何创建和连接整张网络。
6. [port_based_router_design.md](./port_based_router_design.md)
   继续看 Router/Link 的端口化设计。
7. [ring_link_model_and_improvements.md](./ring_link_model_and_improvements.md)
   单独看 Link 的传播延迟、序列化、capacity、反压和后续改进。
8. [arbitration.md](./arbitration.md)
   如果关心多输入竞争同一输出口，重点看仲裁。
9. [performance_and_alignment.md](./performance_and_alignment.md)
   如果关心性能趋势、瓶颈和真实硬件对齐，重点看这页。

## 术语

- `NIU`：`TmRingInf`，master 侧接入点。
- `Router`：`TmRingRouter`，每个 ring stop 上的轻量转发节点。
- `Link`：`TmRingLink`，一个方向上的有向链路。
- `TargetPort`：`TmRingTargetPort`，target/memory 侧接入点。
- `Fabric`：`TmRingFabric`，负责创建、持有和连接整张 Ring。
- `REQ subnet`：承载 `RD`、`WR`、`WR_DAT`。
- `RSP subnet`：承载 `RD_RSP`、`WR_RSP`、`RSP`。

## 当前模型边界

支持：

- 双向 Ring，`EAST` 顺时针，`WEST` 逆时针。
- 最短路径路由，等距时走 `EAST`。
- 独立 request/response subnet。
- Router 输入端口短缓存、Link 延迟和序列化。
- Target credit/token/outstanding。
- Master 侧 read/write outstanding。
- API 路径 `send_rd_req()` / `send_wr_req()` / `completed()`。
- BIU 通过 `p_tm_com_inf_t` 接入 Ring。
- 每个主要模块独立 log 文件，便于 debug。

不支持：

- flit、VC、VC allocator、switch allocator。
- RTL AXI 五通道逐拍行为。
- cache coherence / snoop。
- 用显式 retry event 每周期扫描全网。
