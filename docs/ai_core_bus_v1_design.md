# AI Core Bus Fabric V1 设计说明

## 1. 设计目标

`TmBusFabric` V1 的目标不是实现完整 NoC，而是提供一个可运行、可配置、适配当前 `aicore/` ESL 风格的事务级 SoC fabric。

核心目标如下：

1. 支持多个 AI Core 通过 `PemBiu` 并发访问共享存储系统。
2. 支持 `L2 / DDR / MMIO` 等多种下游端口。
3. 支持可配置的地址路由、`interleave`、仲裁、FIFO 深度、带宽和时延。
4. 支持显式 backpressure、局部热点和事务生命周期跟踪。
5. 保持实现复杂度可控，优先满足 CA / ESL 级验证需求。

## 2. 非目标

V1 不覆盖以下内容：

- cache coherence
- snoop / snoop filter
- flit / VC / router pipeline
- 多跳 NoC 拓扑
- 链路级 credit 环

## 3. 上下游接口

### 3.1 上游：`PemBiu`

`PemBiu` 已经完成 core 内部多源收敛，因此 `TmBusFabric` 只处理对外事务流。

当前显式建模三类请求：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`

返回路径依赖以下字段：

- `mst_id`
- `gid`
- `chan`

### 3.2 下游：`TmMem`

`TmMem` 是带流量约束的事务级存储端点，已经具备：

- 地址域区分
- credit 限制
- 带宽限制
- 访问时延

因此 `TmBusFabric` 主要建模互连层本身，而不是重复实现 memory controller。

## 4. 总体架构

`TmBusFabric` 位于多个 `PemBiu` 与多个 `TmMem` 之间：

```text
PemBiu[0] ----\
PemBiu[1] -----\
PemBiu[2] ------> TmBusFabric ---> L2Slice[0] / TmMem
... ----------/                  ---> L2Slice[1] / TmMem
PemBiu[N-1] --/                  ---> DDRCh[0]   / TmMem
                                   ---> DDRCh[1]   / TmMem
                                   ---> MMIO/CFG   / optional
```

建模原则如下：

1. 采用单跳事务级 fabric，不显式建模多级 router。
2. 冲突按 `target` 局部化，不把系统抽象成一根全局串行总线。
3. 通过显式 FIFO、credit、token 和 busy time 建模容量与吞吐上限。

## 5. 设计参考来源

### 5.1 gem5 XBar

主线借鉴 gem5 `BaseXBar / NoncoherentXBar`：

- `per-target` 竞争
- 事务级 request / response 模型
- 地址解码与回程路由
- 用宽度和时延折算 busy time

### 5.2 GPGPU-Sim LOCAL_XBAR

有选择地吸收以下思路：

- 显式 buffer 组织
- `request / reply subnet` 视角
- 可替换 arbiter

### 5.3 不直接吸收的部分

当前 V1 不直接引入：

- `INTERSIM / BookSim` 多跳网络
- flit 级路由
- VC 分配

## 6. 拓扑与 Interleave

V1 使用单级地址解码：

1. 先匹配显式 `target` 地址域
2. 如多个 `target` 共享一段地址空间，则再按 `interleave` 策略选 slice
3. 若都不命中，则回退到 `default target`

`interleave` 已改成可配置策略，而不是写死为固定线性条带。当前支持：

- `LINEAR`
- `XOR_HASH`

相关配置位于 `TmBusTargetCfg`：

- `interleave_type`
- `interleave_size`
- `interleave_num`
- `interleave_idx`
- `interleave_hash_shift`
- `interleave_hash_seed`

相关实现位于：

- `aicore/tm_bus_topology.h/.cc`
- `aicore/tm_bus_interleave.h/.cc`

专题说明见：

- `docs/aicore_bus/topology.md`

## 7. 事务模型

### 7.1 读事务

读事务生命周期为：

1. 进入 `master ingress FIFO`
2. 经 `per-target` 仲裁进入 `target FIFO`
3. 发往下游 `target`
4. 收到一个或多个读响应
5. 释放读 slot
6. 回收事务上下文

### 7.2 写事务

写事务生命周期为：

1. `WR_REQ` 入队并发往 `target`
2. 收到 `WR_REQ_RSP`
3. 生成 grant / DBID
4. 匹配到对应 `WR_DAT`
5. 发出 `WR_DAT`
6. 收到 `WR_DAT_RSP`
7. 释放写 slot
8. 回收事务上下文

## 8. Buffer 组织

V1 使用显式 buffer 组织，主要包括：

- `master ingress FIFO`
- `target-side request FIFO`
- `master-side response FIFO`
- `per-master grant FIFO`

这种结构既符合当前 `tm_*` 框架风格，也便于吸收 `LOCAL_XBAR` 的本地互连建模方法。

## 9. 仲裁与流控

### 9.1 仲裁

当前仲裁粒度是：

- 按 `target_id`
- 按请求类型 `RD_REQ / WR_REQ / WR_DAT`

当前默认策略：

- `RR`

保留扩展点：

- `ISLIP_LIKE`

### 9.2 流控

当前同时使用：

- `slot credit`
- `bandwidth token`
- `busy time`
- `outstanding` 深度

资源规则如下：

- `RD_REQ`：消耗读 slot 和读带宽
- `WR_REQ`：占用写 slot，但不消耗写数据带宽
- `WR_DAT`：消耗写带宽，在 `WR_DAT_RSP` 后释放写 slot

## 10. 当前代码组织

当前实现已经按职责拆分为：

- `tm_bus_types.h`
- `tm_bus.h`
- `tm_bus_core.cc`
- `tm_bus_req.cc`
- `tm_bus_rsp.cc`
- `tm_bus_topology.h/.cc`
- `tm_bus_interleave.h/.cc`
- `tm_bus_flow_ctrl.h/.cc`
- `tm_bus_arbiter.h/.cc`

这使得主类更像调度骨架，而不是把拓扑、仲裁、流控都堆在一起。

更细的说明见：

- `docs/aicore_bus/code_organization.md`

## 11. 当前实现状态

截至当前版本，`TmBusFabric` 已具备：

1. 多 `master` / 多 `target`
2. 地址解码与 `default target`
3. `LINEAR / XOR_HASH` 两种 `interleave` 策略
4. `RD_REQ / WR_REQ / WR_DAT` 三通路建模
5. 显式 `ingress / target / response / grant` FIFO
6. `per-target` 仲裁
7. 事务上下文跟踪
8. `slot credit / bandwidth token / busy time` 建模
9. 写事务 grant 顺序约束

## 12. 后续演进方向

V1 继续收口时，优先做：

1. 增强统计项和可观测性
2. 完善最小连接样例
3. 继续清理注释和编码

后续如果要走向 V2，可继续演进：

1. 更强的仲裁策略
2. 更丰富的 `interleave` / hash 规则
3. 显式 `request / reply subnet` 对象
4. 双级或多级 fabric
5. 简化版 NoC-lite 多跳延迟

## 13. 结论

`TmBusFabric` V1 的定位很明确：

它是一个 gem5 `XBar` 风格的事务级 AI Core SoC fabric，在不引入完整 NoC 复杂度的前提下，选择性吸收 `LOCAL_XBAR` 的 buffer / subnet / arbiter 思路，并保持与当前 `aicore/` ESL 风格一致。
