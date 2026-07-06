# Buffer 与 Subnet 说明

## 1. 文档范围

本文档描述 `TmBusFabric` 中的 buffering 组织和 subnet 思想，重点包括：

- ingress buffer
- target-side buffer
- response buffer
- grant buffer
- 逻辑 subnet 划分

## 2. Buffer 设计原则

当前 fabric 采用显式 buffering，而不是把所有输运行为藏在一次函数调用里。

当前主要 buffer 分组包括：

1. master ingress FIFO
2. target-side request FIFO
3. master-side response FIFO
4. per-master write grant FIFO

这是当前实现中最明显吸收 GPGPU-Sim `LOCAL_XBAR` 思想的地方。

## 3. Master Ingress FIFO

master ingress FIFO 用于吸收上游 `PemBiu` 发来的流量。

它的作用有两个：

1. 解耦 master 时序与 target 竞争
2. 让上游 backpressure 清晰可见、可统计

每类请求都有独立的 ingress FIFO。

## 4. Target FIFO

事务经过仲裁后，会进入 target 侧 FIFO。

这些队列代表的是：

- 每个 target 的局部竞争
- 仲裁层与真正 target issue 之间的明确交接点

这和 gem5 `XBar` 里按 target 组织竞争层的思想是对齐的。

## 5. Response FIFO

响应不会在 target 收到后立刻直接同步回 master，而是先进入显式 response FIFO。

这样做的好处是：

1. master 侧反压可见
2. 响应路径时序可控
3. 多 lane 读响应可以清楚拆开

## 6. Grant FIFO

写命令响应会生成 grant 信息。

这部分被显式保存在 grant FIFO 中，因为 `WR_DAT` 后续还可能继续被以下因素阻塞：

- 仲裁
- bandwidth token 不足
- target busy time
- 下游接口反压

如果没有 grant FIFO，写数据阶段就会丢失顺序上下文。

## 7. Subnet 视角

当前 fabric 没有像 GPGPU-Sim `LOCAL_XBAR` 那样显式实例化多个物理子网对象，但已经吸收了 **逻辑 subnet 分离** 的思想。

当前逻辑上至少区分：

- request-side progression
- response-side progression

而在 request-side 内部，又继续区分：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`

这意味着虽然代码里还没有独立的 `ReqSubnet` / `ReplySubnet` 类，但设计思路上已经具备了这种分离。

## 8. 后续演进方向

如果 V2 需要更强地借鉴 `LOCAL_XBAR`，下一步可以考虑：

1. 显式引入 request subnet 对象
2. 显式引入 reply subnet 对象
3. 为不同 subnet 分别维护仲裁器和 timing 状态

当前代码拆分已经为这种演进预留了空间。

## 9. 当前代码映射

- `aicore/tm_bus_core.cc`
- `aicore/tm_bus_req.cc`
- `aicore/tm_bus_rsp.cc`
- `aicore/tm_bus_types.h`
