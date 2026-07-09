# AI Core Mesh-Lite 文档索引

这套代码是一个面向多 AI Core SoC 的轻量互连模型。它保留了多跳、仲裁、链路延迟、目标端瓶颈和写事务两阶段语义，但不下沉到 flit、VC、credit 级实现。

主路径：

```text
Core/上游
  -> Tm_mesh_inf
  -> TmMeshRouter
  -> TmMeshLink
  -> TmMeshRouter
  -> TmMeshTargetPort
  -> Target/TmMem
  -> 响应按原路径返回
```

推荐阅读顺序：

1. [code_organization.md](./code_organization.md)
   先看代码分层和主入口。
2. [mesh_fabric_design.md](./mesh_fabric_design.md)
   再看 `TmMeshFabric` 怎么调度整张网络。
3. [port_based_router_design.md](./port_based_router_design.md)
   最后看端口化 router、link 和仲裁语义。
4. [arbitration.md](./arbitration.md)
   如果你主要关心竞争和带宽分配，直接看这一页。
5. [design_tradeoffs.md](./design_tradeoffs.md)
   如果你想知道哪些地方是抽象、哪些地方更接近硬件，再看这一页。
6. [performance_and_alignment.md](./performance_and_alignment.md)
   如果你想知道这套模型能看出什么性能现象，以及和真实硬件大致对齐到哪一层，直接看这一页。

术语约定：

- `NIU`：`Tm_mesh_inf`，master 侧接入点。
- `Router`：`TmMeshRouter`，端口化粗粒度 router。
- `Link`：`TmMeshLink`，`src_port -> dst_port` 的有向链路。
- `TargetPort`：`TmMeshTargetPort`，target 侧接入点。
- `Fabric`：`TmMeshFabric`，统一调度器和事务上下文持有者。
- `txn_ctx_`：共享事务表，用来跟踪请求从发起到完成的生命周期。
