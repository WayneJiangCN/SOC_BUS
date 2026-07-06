# AI Core Bus 设计包

## 1. 目录目的

本目录用于承载 `TmBusFabric` 的专题化设计文档。

它和其余总文档的关系如下：

- `docs/ai_core_interconnect_selection.md`
  说明为什么主线选择 gem5 `XBar` 风格事务级 fabric，并有选择地吸收 `LOCAL_XBAR`
- `docs/aicore_code_style_guide.md`
  说明 `aicore/` 目录应该遵循什么样的 ESL / TLM 风格
- `docs/ai_core_bus_v1_design.md`
  说明 `TmBusFabric` V1 的总体目标、边界和实现状态

本目录则进一步把总线设计拆成几个专题，做到“一个专题对应一组源码职责”。

## 2. 文档结构

1. [topology.md](./topology.md)
   说明拓扑结构、地址解码、`default target` 和可配置 `interleave` 规则。

2. [transactions.md](./transactions.md)
   说明 `RD_REQ / WR_REQ / WR_DAT` 三类事务的语义和生命周期。

3. [flow_control.md](./flow_control.md)
   说明 `slot credit / bandwidth token / busy time / hotspot penalty`。

4. [arbitration.md](./arbitration.md)
   说明 `per-target` 仲裁，以及当前 `RR / ISLIP_LIKE` 的定位。

5. [buffers_and_subnets.md](./buffers_and_subnets.md)
   说明显式 buffer 组织，以及从 `LOCAL_XBAR` 借来的 `request/reply subnet` 视角。

6. [code_organization.md](./code_organization.md)
   说明源码文件如何按职责拆分，以及与设计专题如何一一映射。

## 3. 当前主线

`TmBusFabric` 当前沿以下方向演进：

1. 以 gem5 `BaseXBar / NoncoherentXBar` 为事务级主参考。
2. 保持 `PemBiu / TmMem / tm_*` 这一套 ESL 风格，不切换到 gem5 原生端口回调风格。
3. 吸收 `LOCAL_XBAR` 的显式 buffer、逻辑 subnet 和可替换 arbiter 思路。
4. 将 `interleave` 从写死规则改成可配置策略类，便于按 `linear / hash` 等方式扩展。

## 4. 当前源码映射

与总线直接相关的源码目前包括：

- `tm_bus_types.h`
- `tm_bus.h`
- `tm_bus_core.cc`
- `tm_bus_req.cc`
- `tm_bus_rsp.cc`
- `tm_bus_topology.h/.cc`
- `tm_bus_interleave.h/.cc`
- `tm_bus_flow_ctrl.h/.cc`
- `tm_bus_arbiter.h/.cc`

这组文档和这组源码保持尽量一一对应，后续继续演进时也建议保留这种关系。
