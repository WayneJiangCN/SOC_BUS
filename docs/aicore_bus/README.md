# AI Core Interconnect Docs

## Current Position

`BUS/aicore/` 当前主线应理解为一套 **SoC 级轻量 interconnect / mesh-lite 模型**：

- 不是传统单共享总线
- 不是 Garnet 那种 flit/VC/credit 级 NoC
- 是面向多 AI Core CA/ESL 用例的 transaction/message-level 互连模型

当前主线结构：

```text
Core / BIU
  -> Tm_mesh_inf
  -> TmMeshRouter
  -> TmMeshLink
  -> TmMeshRouter
  -> TmMeshTargetPort
  -> TmMem / target
```

## Recommended Reading

1. [soc_light_interconnect_plan.md](./soc_light_interconnect_plan.md)
2. [mesh_modeling_plan.md](./mesh_modeling_plan.md)
3. [mesh_fabric_design.md](./mesh_fabric_design.md)
4. [code_organization.md](./code_organization.md)
5. [arbitration.md](./arbitration.md)

## What Was Removed

旧的 ring / `TmBusFabric` 时代文档已经从本目录移除，原因是它们会误导当前 mesh 主线，包括：

- ring 拓扑说明
- ring 事务路径说明
- ring FIFO / 子网说明
- 旧 `pem_bus` 与 `TmBusFabric` 集成说明
- 旧 bus-to-ring 演进说明

如果后面需要保留历史设计记录，建议单独迁到 `legacy/` 或项目外部笔记，而不要继续和当前 mesh 主线文档混放。

## One-Line Summary

这套代码最准确的定位是：

**一套保留 AI Core 事务协议语义、面向多 core SoC CA/ESL 用例的轻量 mesh-lite 互连模型。**
