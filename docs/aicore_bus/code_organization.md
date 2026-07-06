# 代码组织说明

## 1. 文档范围

本文档说明 `TmBusFabric` 相关源码如何按职责拆分，以及这种拆分如何对应到设计专题。

## 2. 当前文件布局

当前 `aicore/` 下采用扁平布局，相关文件如下：

- `tm_bus_types.h`
- `tm_bus.h`
- `tm_bus_core.cc`
- `tm_bus_req.cc`
- `tm_bus_rsp.cc`
- `tm_bus_topology.h`
- `tm_bus_topology.cc`
- `tm_bus_interleave.h`
- `tm_bus_interleave.cc`
- `tm_bus_flow_ctrl.h`
- `tm_bus_flow_ctrl.cc`
- `tm_bus_arbiter.h`
- `tm_bus_arbiter.cc`

对当前仓库规模来说，这种布局仍然可接受，因为：

- 总线相关文件数量还不算多
- 文件名已经能清楚表达职责
- 本地跳转和阅读成本仍然较低

## 3. 按职责划分

### 3.1 `tm_bus_types.h`

存放：

- 配置结构
- 枚举类型
- grant 结构
- transaction context 结构

### 3.2 `tm_bus.h`

存放：

- `TmBusFabric` 主类声明
- 生命周期接口
- attach 接口
- 主类拥有的核心成员

### 3.3 `tm_bus_core.cc`

存放：

- 构造和析构
- `config / build / reset / idle`
- attach 逻辑
- `tick()` 主调度顺序

### 3.4 `tm_bus_req.cc`

存放：

- `master` 请求接收
- `master ingress FIFO` 入队
- `per-target` 仲裁入口
- 向 `target` 发请求

### 3.5 `tm_bus_rsp.cc`

存放：

- `target` 响应接收
- grant / DBID 管理
- 向 `master` 回响应
- 事务回收

### 3.6 `tm_bus_topology.*`

存放：

- `master_id <-> master_port` 映射
- 地址解码主流程
- `default target` 回退逻辑

### 3.7 `tm_bus_interleave.*`

存放：

- `interleave` 策略基类
- `LINEAR` 子类
- `XOR_HASH` 子类
- 策略工厂函数

这部分的目标是把“共享地址空间下如何选 slice”的算法从 `decode_target()` 主流程里剥离出来。

### 3.8 `tm_bus_flow_ctrl.*`

存放：

- `slot credit`
- `bandwidth token`
- `busy time`
- `outstanding` 统计
- `hotspot penalty`

### 3.9 `tm_bus_arbiter.*`

存放：

- `per-target` 仲裁状态
- 当前 `RR` 策略
- `ISLIP_LIKE` 扩展点

## 4. 这样拆分的原因

这套拆法综合吸收了两类参考实现的优点。

### 4.1 来自 gem5 XBar 的影响

- 拓扑是一个独立主题
- 流控是一个独立主题
- 仲裁是一个独立主题
- `fabric` 主类更像调度骨架，而不是所有逻辑都堆在一起

### 4.2 来自 GPGPU-Sim LOCAL_XBAR 的影响

- buffer 组织是显式可见的
- arbiter 可以独立替换
- 后续可以继续往 `request / reply subnet` 方向演进

## 5. 与设计文档的映射关系

- `topology.md` -> `tm_bus_topology.*` + `tm_bus_interleave.*`
- `transactions.md` -> `tm_bus_req.cc` + `tm_bus_rsp.cc` + `tm_bus_types.h`
- `flow_control.md` -> `tm_bus_flow_ctrl.*`
- `arbitration.md` -> `tm_bus_arbiter.*` + `tm_bus_req.cc`
- `buffers_and_subnets.md` -> `tm_bus_core.cc` + `tm_bus_req.cc` + `tm_bus_rsp.cc`

这种一一对应关系是刻意设计的，后续如果继续扩展模块，建议尽量保持。

## 6. 后续可选的目录层次化

如果后面文件继续增多，可以再切换成分层目录结构，例如：

```text
aicore/bus/
  tm_bus.h
  tm_bus_types.h
  core/
    tm_bus_core.cc
    tm_bus_req.cc
    tm_bus_rsp.cc
  topology/
    tm_bus_topology.h
    tm_bus_topology.cc
    tm_bus_interleave.h
    tm_bus_interleave.cc
  flow/
    tm_bus_flow_ctrl.h
    tm_bus_flow_ctrl.cc
  arb/
    tm_bus_arbiter.h
    tm_bus_arbiter.cc
```

但在当前阶段，继续使用扁平布局仍然是合理的。
