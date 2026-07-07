# AI Core Ring/Mesh 互连文档索引

## 1. 文档目标

本目录用于说明 `BUS/aicore/` 下的互连模型设计与实现。

当前主线已经拆成两套并行版本：

- ring 版本
- mesh 版本

两者都保持以下共同点：

- 事务级建模
- `PemBiu / TmMem / tm_*` 风格接口
- `RD_REQ / WR_REQ / WR_DAT` 协议语义

## 2. 当前文档结构

### 2.1 Ring 版本

ring 版本定位为：

- 事务级单向 ring NoC-lite
- request/response 逻辑子网分离
- target 级流控为主

对应文档：

- [topology.md](./topology.md)
- [transactions.md](./transactions.md)
- [flow_control.md](./flow_control.md)
- [arbitration.md](./arbitration.md)
- [buffers_and_subnets.md](./buffers_and_subnets.md)
- [code_organization.md](./code_organization.md)

### 2.2 Mesh 版本

mesh 版本定位为：

- 事务级 mesh NoC-lite
- 借鉴 gem5 `Mesh_XY` 的网格拓扑与确定性路由
- 保留 target 级事务流控

对应文档：

- [mesh_fabric_design.md](./mesh_fabric_design.md)
- [mesh_modeling_plan.md](./mesh_modeling_plan.md)

## 3. 推荐阅读顺序

如果要先了解 ring 主线，建议按以下顺序阅读：

1. [topology.md](./topology.md)
2. [transactions.md](./transactions.md)
3. [flow_control.md](./flow_control.md)
4. [buffers_and_subnets.md](./buffers_and_subnets.md)
5. [arbitration.md](./arbitration.md)
6. [code_organization.md](./code_organization.md)

如果要了解 mesh 升级版，建议按以下顺序阅读：

1. [mesh_fabric_design.md](./mesh_fabric_design.md)
2. [mesh_modeling_plan.md](./mesh_modeling_plan.md)
3. [interconnect_evolution.md](./interconnect_evolution.md)

## 4. 与其他文档的关系

- [pem_bus_integration.md](./pem_bus_integration.md)
  说明 `pem_bus` 和 `pem_aic_core` 如何接入当前 fabric。

- [interconnect_evolution.md](./interconnect_evolution.md)
  说明 `XBar -> ring NoC-lite -> mesh NoC-lite -> Ruby/Garnet` 的演进关系。

## 5. 当前代码范围

ring 版本核心代码：

- `BUS/aicore/tm_bus_types.h`
- `BUS/aicore/tm_bus.h`
- `BUS/aicore/tm_bus_core.cc`
- `BUS/aicore/tm_bus_topology.h`
- `BUS/aicore/tm_bus_topology.cc`
- `BUS/aicore/tm_bus_interleave.h`
- `BUS/aicore/tm_bus_interleave.cc`
- `BUS/aicore/tm_bus_req.cc`
- `BUS/aicore/tm_bus_rsp.cc`
- `BUS/aicore/tm_bus_flow_ctrl.h`
- `BUS/aicore/tm_bus_flow_ctrl.cc`

mesh 版本核心代码：

- `BUS/aicore/tm_mesh_types.h`
- `BUS/aicore/tm_mesh.h`
- `BUS/aicore/tm_mesh_core.cc`
- `BUS/aicore/tm_mesh_topology.h`
- `BUS/aicore/tm_mesh_topology.cc`
- `BUS/aicore/tm_mesh_req.cc`
- `BUS/aicore/tm_mesh_rsp.cc`

`tm_bus_arbiter.h/.cc` 当前仍保留在工程中，但 ring 和 mesh 主路径都没有把它当成核心调度器使用，更多是为后续 router 局部仲裁扩展预留。
