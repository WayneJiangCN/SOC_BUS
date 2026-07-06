# `pem_bus` / `pem_aic_core` 与 `TmBusFabric` 集成方案

## 1. 文档目的

本文档说明如何在保留现有项目外部接口的前提下，将当前已实现的：

- `pem_aic_core`
- `bus_top_wrapper`
- `pem_bus`

与新的事务级共享总线模型 `TmBusFabric` 对接。

目标不是推翻现有工程壳，而是：

1. 保留 `pem_aic_core -> bus_top_wrapper -> pem_bus` 这层系统集成接口。
2. 保留 `directRead/directWrite/canSend/completed` 这类上层使用习惯。
3. 用 `TmBusFabric` 替换 `pem_bus` 内部原有的共享互连核心。

## 2. 当前问题

从现有 `pem_bus` 实现看，它内部的共享互连主路径更接近：

```text
Core Port -> TmRouter -> TmMem
```

这套实现可以完成：

- 基本请求/响应路由
- 单个下游 memory 对接
- 兼容上层 `pem_aic_core` 和总线包装接口

但它不具备 `TmBusFabric` 当前已经实现的这些能力：

- `per-target` 仲裁
- 显式 FIFO 反压
- `RD_REQ / WR_REQ / WR_DAT` 分通路
- `grant/DBID` 约束
- `credit/token` 流控
- `interleave` 目标选择
- 关键热点与 busy time 建模

因此，现有 `pem_bus` 不能继续作为共享总线的“核心实现”，更适合作为：

**系统集成 wrapper**

## 3. 推荐的新分层

推荐将整体结构调整为：

```text
pem_aic_core
  -> PemBiu
  -> pem_bus (wrapper)
      -> TmBusFabric
          -> TmMem / L2 / DDR target
```

其中职责划分如下：

- `pem_aic_core`
  继续负责任务启动、寄存器读写、状态统计、同步控制。
- `PemBiu`
  继续负责 core 内部多源请求收敛，并向外提供统一事务接口。
- `pem_bus`
  负责总线顶层包装、时钟和外部兼容接口，不再承担共享仲裁逻辑。
- `TmBusFabric`
  成为真正的共享事务级互连核心。
- `TmMem`
  继续作为 memory/target endpoint。

## 4. `pem_bus` 的新定位

### 4.1 保留什么

建议保留 `pem_bus` 的以下对外能力：

- 构造函数与总线对象生命周期
- `get_clk()`
- `directRead()/directWrite()`
- `canSendRdReq()/canSendWrReq()`
- `completed()`
- `get_bus()`

这些接口对上层控制软件、回归环境和历史代码都还有价值。

### 4.2 替换什么

建议替换 `pem_bus` 当前内部这条共享路径：

```text
TmRouter + Pem_bus_inf + 单 memory 路由
```

改为：

```text
TmBusFabric + target memory list
```

也就是说：

- `TmRouter` 不再是共享总线核心。
- `TmBusFabric` 成为共享总线核心。
- `pem_bus` 只负责包装与对外接口兼容。

## 5. 推荐的 `pem_bus` 成员设计

建议 `pem_bus` 新增或保留以下成员。

### 5.1 基础成员

```cpp
SIM_BUS_TYPE type_;
tm_engine::p_tm_clk_t clk_;
uint32_t chip_id_ = 0;
uint32_t req_id_ = 0;
```

### 5.2 共享总线核心

```cpp
p_tm_bus_fabric_t fabric_ = nullptr;
```

用途：

- 共享总线模式下的真正互连核心。

### 5.3 下游 target

```cpp
std::vector<p_tm_mem_t> targets_;
```

用途：

- 保存 `L2 / DDR / MMIO` 等 target。
- 供 `attach_target()`、`get_bus()`、`directRead/directWrite()` 使用。

如果当前阶段只保留一个默认内存端口，也可以先做成：

```cpp
p_tm_mem_t default_target_ = nullptr;
```

### 5.4 兼容接口层

如果外部仍依赖旧的 `Pem_bus_inf` 或旧的 `read()/write()` 访问语义，可以保留：

```cpp
std::shared_ptr<Pem_bus_inf> compat_inf_ = nullptr;
```

但它的作用不再是“共享总线本体”，而只是可选兼容层。

## 6. `pem_bus` 推荐对外接口

建议在 `pem_bus` 中增加如下接口：

```cpp
std::shared_ptr<TmBusFabric> get_fabric();
bool attach_core(uint32_t master_port, p_pem_biu_t biu);
bool attach_target(uint32_t target_port, p_tm_mem_t mem);
```

其中：

- `get_fabric()`
  便于调试、统计和特殊集成场景直接访问 fabric。
- `attach_core()`
  将 `PemBiu` 接到 fabric 的 master 侧。
- `attach_target()`
  将 `TmMem` 接到 fabric 的 target 侧。

### 6.1 `get_bus()` 是否保留

建议保留 `get_bus()`，但语义要收窄：

- 如果只有单一默认 target，则返回该 target。
- 如果是多 target 系统，可约定返回 default target，或明确只用于 direct access。

不建议再把 `get_bus()` 理解成“共享总线核心对象”。

共享总线核心应该通过：

```cpp
get_fabric()
```

来获取。

## 7. `pem_aic_core` 需要补的接口

### 7.1 最关键接口：暴露 `PemBiu`

为了让 `TmBusFabric` 直接复用当前总线接口，建议在 `pem_aic_core` 或其 CA core 子对象上增加：

```cpp
p_pem_biu_t get_biu() const;
```

原因是当前 `TmBusFabric` 的主接口已经是：

```cpp
attach_master(uint32_t idx, p_pem_biu_t biu)
```

它需要直接拿到：

- `biu->out_intf_`
- `biu->core_id_`

而不是只拿一个旧式的 `bus_inf_`。

### 7.2 可选接口：显式获取 memory 口

如果后续要支持 dedicated bus 或直连 memory，也可以增加：

```cpp
p_tm_mem_t get_attached_mem() const;
```

不过这不是共享总线改造的强依赖。

## 8. `configure_shared_bus()` 推荐改法

### 8.1 旧逻辑

旧逻辑大致是：

```cpp
std::shared_ptr<pem_bus> p_bus = std::dynamic_pointer_cast<pem_bus>(slave_bus_port);
std::shared_ptr<TmRouter> p_rt = p_bus->get_router();
p_ca_core->bus_inf_->connect(p_rt->get_inf(core_id_));
p_ca_core->attach(p_bus->get_bus());
```

问题在于：

- 它绑定的是 `TmRouter` 路由口。
- 它没有接入 `TmBusFabric` 的事务级仲裁/流控。

### 8.2 新逻辑

建议改成：

```cpp
bool pem_aic_core::configure_shared_bus(uint32_t slave_bus_id,
                                        std::shared_ptr<bus_top_wrapper> slave_bus_port)
{
    auto p_bus = std::dynamic_pointer_cast<pem_bus>(slave_bus_port);
    if (p_bus == nullptr) {
        return false;
    }

    if (type_ == SIM_CA) {
        auto biu = p_ca_core->get_biu();
        if (biu == nullptr) {
            return false;
        }

        return p_bus->attach_core(core_id_, biu);
    }

    // PV 路径保留旧式功能模型接法，或按项目需要单独处理。
    return true;
}
```

### 8.3 说明

这里的关键变化是：

- 不再显式拿 `router port`
- 不再让 `pem_aic_core` 直接感知路由器
- 改为通过 `pem_bus` 提供的 `attach_core()` 接入 `TmBusFabric`

这样共享总线内部结构变化时，上层 `pem_aic_core` 不需要再跟着改接口。

## 9. `configure_dedicated_bus()` 推荐改法

dedicated bus 不一定要走 `TmBusFabric`。

如果该模式的目标就是“每个 core 直连一个 memory/slave”，那么建议继续保持简单：

```text
PemBiu -> TmMem
```

### 9.1 推荐做法

```cpp
bool pem_aic_core::configure_dedicated_bus(uint32_t slave_bus_id,
                                           std::shared_ptr<bus_top_wrapper> slave_bus_port)
{
    auto p_bus = std::dynamic_pointer_cast<pem_esl_bus>(slave_bus_port);
    if (p_bus == nullptr) {
        return false;
    }

    if (type_ == SIM_CA) {
        auto biu = p_ca_core->get_biu();
        if (biu == nullptr) {
            return false;
        }

        biu->attach(p_bus->get_bus());
        return true;
    }

    return true;
}
```

### 9.2 为什么 dedicated 不一定要复用 fabric

因为 dedicated 模式通常不关心：

- 多 master 竞争
- interleave
- target 级仲裁

它更适合作为：

- 最简单 bring-up 路径
- 参考基线
- 单核或单端口功能验证

## 10. `pem_bus` 内部构造建议

### 10.1 共享总线模式

共享总线模式下，构造流程建议变成：

1. 创建 `clk_`
2. 构造 `TmBusCfg`
3. 构造 `fabric_`
4. 构造默认 `TmMem` 或多个 target `TmMem`
5. 将 target 挂到 `fabric_`

示意伪代码：

```cpp
clk_ = tm_make_clk();

auto bus_cfg = tm_make_bus_cfg();
bus_cfg->name = "pem_bus_" + std::to_string(chip_id);
bus_cfg->num_masters = num_ai_core;
bus_cfg->num_targets = 1;
bus_cfg->rd_rsp_port_num = 2;
bus_cfg->targets.push_back(make_default_target_cfg_from_mem_cfg(...));

fabric_ = tm_make_bus(clk_, bus_cfg);

auto mem_cfg = tm_make_mem_cfg("ddr", ddr_cfg);
auto mem = tm_make_mem(clk_, mem_cfg);
targets_.push_back(mem);
fabric_->attach_target(0, mem);
```

### 10.2 shared bus attach core

```cpp
bool pem_bus::attach_core(uint32_t master_port, p_pem_biu_t biu)
{
    if (fabric_ == nullptr || biu == nullptr) {
        return false;
    }
    fabric_->attach_master(master_port, biu);
    return true;
}
```

### 10.3 shared bus attach target

```cpp
bool pem_bus::attach_target(uint32_t target_port, p_tm_mem_t mem)
{
    if (fabric_ == nullptr || mem == nullptr) {
        return false;
    }
    fabric_->attach_target(target_port, mem);
    return true;
}
```

## 11. 最小集成伪代码

### 11.1 总线创建

```cpp
auto bus = std::make_shared<pem_bus>(num_ai_core, SIM_BUS_CA, chip_id, pem_cfg);
```

### 11.2 core 接入共享总线

```cpp
for (uint32_t i = 0; i < num_ai_core; ++i) {
    auto core = create_core(i);
    core->configure_shared_bus(0, bus);
}
```

### 11.3 `configure_shared_bus()` 内部

```cpp
auto p_bus = std::dynamic_pointer_cast<pem_bus>(slave_bus_port);
auto biu = p_ca_core->get_biu();
return p_bus->attach_core(core_id_, biu);
```

### 11.4 dedicated 路径

```cpp
auto biu = p_ca_core->get_biu();
biu->attach(dedicated_mem);
```

## 12. 迁移顺序建议

建议按下面顺序迁移，风险最小。

1. 先给 `pem_aic_core` 或 `p_ca_core` 增加 `get_biu()`
2. 给 `pem_bus` 增加 `fabric_ / attach_core() / attach_target() / get_fabric()`
3. 保留 `directRead/directWrite/get_bus()` 原接口
4. 把 `configure_shared_bus()` 从 `TmRouter` 连法切到 `TmBusFabric`
5. dedicated 路径最后再收口

## 13. 风险点

### 13.1 `core_id_` 与 `master_port`

当前 `TmBusFabric` 默认把 `core_id_` 作为 `master_id` 绑定。  
如果未来：

- 物理 core 编号不连续
- 逻辑 port 排布与 core_id 不一致

则应通过：

```cpp
bind_master_id(port_id, mst_id)
```

显式绑定，而不是默认假设一一相等。

### 13.2 `rd_rsp_port_num`

`TmBusFabric` 的 `rd_rsp_port_num` 必须与 `PemBiu::rd_port_num` 保持一致。  
否则读响应 lane 的语义会不一致。

### 13.3 PV 路径

PV 路径更像功能模型，不一定需要完整复用 `TmBusFabric`。  
建议先保证 CA 路径稳定，再决定是否让 PV 也挂到同一事务级 fabric 上。

## 14. 最终建议

最终建议可以概括成一句话：

**保留 `pem_aic_core` 和 `pem_bus` 作为项目已有的控制/包装层，但把 `pem_bus` 内部共享互连核心从 `TmRouter` 替换成 `TmBusFabric`。**

这样做有三个好处：

1. 对上层接口最稳定
2. 对下层总线能力提升最大
3. 便于后续继续扩展 `interleave / 仲裁 / 流控 / 多 target`
