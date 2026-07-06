# AI Core Modeling Code Style Guide

## 1. 文档目的

本文用于定义 `aicore/` 目录下模型代码的推荐风格，统一以下几类模块的实现方法：

- `PemBiu`
- `TmMem`
- `TmBusFabric`
- 后续新增的 core-side、interconnect-side、memory-side ESL 模块

本文不是通用 C++ 编码规范，而是 **AI Core 架构模型代码规范**。  
重点约束的是：

1. 模块该按什么抽象层级来写
2. 接口、队列、时序、反压如何表达
3. 文件如何拆分
4. 哪些风格应该保持，哪些风格应该避免

## 2. 风格定位

`aicore/` 目录代码的风格定位如下：

**自研 ESL / 类 TLM 的事务级行为模型风格**

更具体地说，它具有以下特征：

1. 使用 C++ 编写硬件行为模型，而不是编写普通业务逻辑。
2. 以模块、接口、队列、事件、时钟、事务为主要建模对象。
3. 以 `tm_*` 框架为基础，采用事件驱动和进程触发方式组织行为。
4. 重点表达协议语义、流控语义、时延语义和资源竞争语义。
5. 抽象层级高于 RTL，低于纯函数级模型。

因此，`aicore/` 下的代码不应被理解成：

- 一般应用软件代码
- 完整的 RTL 模型
- gem5 原生端口回调式对象模型

它更接近：

- 自研 SystemC/TLM 风格 ESL 模型
- 架构探索阶段的事务级硬件模型
- 面向 CA/ESL 验证的中高层协议模型

## 3. 推荐的核心建模原则

### 3.1 事务优先

模型应优先表达事务，而不是过早下沉为信号级细节。

推荐表达的对象：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`
- `RD_RSP`
- `WR_REQ_RSP`
- `WR_DAT_RSP`

不推荐在 V1/V2 阶段引入过多底层细节，例如：

- beat 级细分
- flit 级切分
- bit-level handshake
- router pipeline 内部寄存器级行为

### 3.2 显式时序

时序应通过以下机制显式表达：

- `tm_sensitive(...)`
- FIFO
- queue
- `time()`
- 延迟队列
- credit / token / outstanding 计数

不推荐把关键时序藏在难以追踪的隐式调用链中。

### 3.3 显式反压

所有流量前进条件都应尽量显式建模，常见反压来源包括：

- 接口 `send()` 失败
- FIFO 满
- target credit 不足
- bandwidth token 不足
- grant 未到
- outstanding 超限

反压应是模型的一等公民，而不是在逻辑里顺手补一个 `if` 就结束。

### 3.4 协议生命周期完整

如果一个协议天然是多阶段事务，就应保留多阶段语义，不应为了“看起来简单”而粗暴合并。

例如写事务应明确区分：

1. `WR_REQ`
2. `WR_REQ_RSP / grant / DBID`
3. `WR_DAT`
4. `WR_DAT_RSP`

只有这样，后续的：

- backpressure
- target 资源占用
- 生命周期统计
- 瓶颈分析

才会保持一致。

## 4. 模块组织风格

### 4.1 生命周期统一

所有主要模块都应尽量保持以下生命周期接口：

- `config()`
- `build()`
- `reset()`
- `idle()`

必要时可增加：

- `attach(...)`
- `tick()`
- `recv_*()`
- `send_*()`

推荐语义如下：

- `config()`：读取参数、创建接口、创建队列、注册敏感事件
- `build()`：做模块间连接
- `reset()`：清空状态、清空 FIFO、恢复 credit/token
- `idle()`：判断模块内部是否已经没有在途事务

### 4.2 角色清晰

建议把模块分成以下几类：

1. **Endpoint 模块**
   例如 `PemBiu`、`TmMem`
2. **Fabric / Interconnect 模块**
   例如 `TmBusFabric`
3. **Traffic / Demo 模块**
   例如 `demo.cc` 里的流量发生模块
4. **辅助子模块**
   例如 `Topology`、`FlowCtrl`、`Arbiter`

每类模块关注点应尽量单一：

- `PemBiu` 负责 core 内部收敛和协议适配
- `TmMem` 负责存储端时延、带宽、credit 行为
- `TmBusFabric` 负责互连层仲裁、路由、反压、事务跟踪

### 4.3 行为拆函数，不要塞进一个大函数

推荐把行为路径拆成清晰的阶段函数，例如：

- `recv_master_reqs()`
- `arbitrate_to_targets()`
- `send_target_reqs()`
- `recv_target_rsps()`
- `send_master_rsps()`

不推荐把完整通路塞进一个超长 `tick()` 或一个超长 `process()` 里。

## 5. 文件拆分风格

### 5.1 按职责拆分，而不是只按类型拆分

推荐像当前 `TmBusFabric` 这样按职责拆分：

- `tm_bus_types.h`
- `tm_bus.h`
- `tm_bus_core.cc`
- `tm_bus_req.cc`
- `tm_bus_rsp.cc`
- `tm_bus_topology.h/.cc`
- `tm_bus_flow_ctrl.h/.cc`

这种拆法比“全部放一个 `.cc`”更适合长期演进。

### 5.2 推荐拆分原则

一个模块可以按下面的维度拆：

1. **类型定义**
   放在 `*_types.h`
2. **主类声明**
   放在 `*.h`
3. **生命周期与装配**
   放在 `*_core.cc`
4. **请求路径**
   放在 `*_req.cc`
5. **响应路径**
   放在 `*_rsp.cc`
6. **拓扑 / 路由**
   放在 `*_topology.*`
7. **流控 / 时延 / credit**
   放在 `*_flow_ctrl.*`

### 5.3 不推荐的拆分方式

不推荐以下情况：

1. 所有逻辑都堆在一个 `*.cc`
2. 一个文件里同时混杂 topology、credit、req、rsp、stats
3. 头文件里混入大量实现代码
4. demo 代码和正式模块实现完全缠在一起

## 6. 命名风格

### 6.1 类型和句柄命名

延续当前项目已有风格：

- `xxx_t`
- `p_xxx_t`
- `tm_make_xxx(...)`

例如：

- `tm_mem_cfg_t`
- `p_tm_mem_t`
- `p_tm_com_inf_t`

如果继续写 `aicore/` 代码，建议保持这一套，不要突然换成另一套完全不同的类型命名体系。

### 6.2 成员变量命名

推荐保留下划线后缀：

- `clk_`
- `cfg_`
- `rd_cmds_`
- `m_wr_grant_fifo_`

推荐使用前缀表达容器含义：

- `v_`：vector
- `m_`：module 内部主状态或 map 风格状态
- `p_`：pointer/shared_ptr 风格别名

### 6.3 通道和事务命名

协议字段应尽量贴近硬件语义，不要用过度抽象的软件词汇替代。

推荐保留：

- `RD_REQ`
- `WR_REQ`
- `WR_DAT`
- `grant`
- `dbid`
- `outstanding`
- `credit`

### 6.4 函数命名

推荐使用“动作 + 对象”命名：

- `recv_rd_cmd()`
- `send_wr_dat()`
- `decode_target()`
- `update_tokens()`

这样比过度抽象的命名更适合协议模型代码。

## 7. 接口设计风格

### 7.1 用接口和队列表达连接关系

推荐使用：

- `p_tm_com_inf_t`
- `p_tm_com_que_t`
- `attach(...)`

来表达模块连接，而不是让模块之间直接调用大量内部函数。

### 7.2 对外接口保持小而清晰

每个模块对外暴露的接口应尽量有限，常见包括：

- `attach(...)`
- `config()/build()/reset()/idle()`
- 少量 `pv_*` 接口

不推荐把大量内部 helper 都做成 public。

### 7.3 functional path 和 timing path 分开

例如：

- `pv_read/pv_write` 代表功能路径
- `recv/send + FIFO + delay` 代表 timing path

这两条路径应在语义上分清楚，不要在一个函数里混写。

## 8. 时序和流控表达规范

### 8.1 `send()` 成功后再弹队头

这是当前代码非常重要的风格，应明确保留：

1. 先查看 FIFO 队头
2. 尝试 `send()`
3. 成功后再 `pop_front()`

这样可以天然表达 backpressure。

### 8.2 credit 在“真正接受”时扣除

推荐在事务真正被下一层接受时才扣 credit / token，而不是在进入本地 FIFO 时就扣。

这样资源占用时刻更清晰，也更容易解释。

### 8.3 资源释放要和事务生命周期对齐

例如：

- 读事务可在响应真正回送后释放读 slot
- 写事务应在 `WR_DAT_RSP` 完成后释放写 slot

不要为了省事在错误的阶段提前释放资源。

### 8.4 反压要逐级传播

推荐的传播路径应是清晰可追踪的：

1. target FIFO 满
2. 停止 target 仲裁出队
3. master ingress FIFO 堆积
4. 上游 `send()` 失败
5. 上游模块停发

这种逐级传播方式是当前风格的重要组成部分。

## 9. 事务状态管理规范

### 9.1 复杂事务要有上下文表

只要事务跨越多个阶段，就推荐显式维护上下文，例如：

- `master_port`
- `target_id`
- `req_type`
- `state`
- `rsp_seen`
- `rsp_expected`
- `slot_released`

### 9.2 状态要有明确语义

事务状态命名应反映协议阶段，而不是模糊表达。

推荐类似：

- `IN_INGRESS_FIFO`
- `IN_TARGET_FIFO`
- `WAIT_RD_RSP`
- `WAIT_WR_REQ_RSP`
- `GRANT_READY`
- `WAIT_WR_DAT_RSP`
- `DONE`

### 9.3 状态跳转要靠事件驱动

不推荐在一个函数里无条件跨多个阶段乱跳。  
推荐由实际事件触发状态变化，例如：

- 入 FIFO
- 发送成功
- grant 到达
- 响应回送成功

## 10. 注释风格

### 10.1 注释要解释“为什么”

这类模型代码最有价值的注释不是解释语法，而是解释设计意图，例如：

- 为什么 `WR_DAT` 必须等 grant 队头
- 为什么 credit 在这里扣
- 为什么 slot 在那个阶段释放
- 为什么按 target 做 RR

### 10.2 优先加在这些地方

推荐重点注释：

1. 关键函数开头
2. 协议分支
3. 资源释放点
4. backpressure 形成点
5. 事务状态切换点

### 10.3 不推荐的注释

不推荐：

- 机械翻译代码本身
- 每行都写废话注释
- 大段过期说明

## 11. 推荐保留的现有风格

以下风格建议继续保留：

1. `config/build/reset/idle` 生命周期
2. `tm_sensitive` 驱动的事件式行为组织
3. 显式 FIFO / queue 建模
4. 事务级接口和 `pv_*` 路径并存
5. 协议字段命名贴近硬件语义
6. 模块内部显式维护 outstanding / credit / grant
7. 文件按职责拆分

## 12. 建议逐步改进的地方

以下地方建议逐步收敛，但不必一下子彻底重写：

### 12.1 减少头文件中的实现污染

头文件中尽量只放：

- 类型
- 声明
- 少量内联 helper

### 12.2 降低 public 成员暴露

当前风格里很多内部状态直接暴露为 public。  
后续建议逐步把真正的内部状态收回 `protected/private`。

### 12.3 统一编码和注释质量

建议：

- 文件编码统一成 UTF-8
- 关键注释统一用中文
- 文件头增加职责说明

### 12.4 demo 与正式模块分离更清楚

`demo.cc` 更偏用例和流量发生器。  
建议后续继续把：

- demo / testcase
- 正式模块
- 共用工具

分得更干净一些。

## 13. 不推荐引入的风格

当前阶段不推荐突然切换到以下风格：

1. 完全照搬 gem5 端口回调体系
2. 完全照搬 BookSim / NoC flit 级写法
3. 为了“现代 C++”而引入大量不必要模板技巧
4. 把行为拆得过碎，导致时序路径反而不清楚
5. 用隐藏状态机替代显式 FIFO / 显式反压

## 14. 对后续模块的直接建议

如果后续继续新增 AI Core 相关模块，推荐按以下模板来想：

1. 这个模块是 endpoint、fabric 还是 traffic generator。
2. 它的事务入口和出口是什么。
3. 它的 backpressure 从哪里来。
4. 它的资源计数是什么。
5. 它是否需要事务上下文表。
6. 它应拆成几个职责文件。

## 15. 最终结论

`aicore/` 目录最适合继续沿着下面这条路线演进：

**事务级 ESL 建模 + 事件驱动进程 + 显式 FIFO/反压 + 协议语义优先 + 按职责拆分文件**

这套风格和你当前的 `PemBiu`、`TmMem`、`TmBusFabric` 是一致的，也最适合继续扩 AI Core 多核互连模型。
