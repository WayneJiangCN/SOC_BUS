# 多核 Ring ESL 独立运行入口

固定入口为 `tm_ring_esl_main.cc`。该入口直接实例化 4 个独立
`PemTrDemo` Master、Ring Fabric 和 Memory Target，不实例化 SoC top，也不依赖
SoC testcase。GTest 入口仅保留为可选回归封装，不再是独立场景的运行前提。

可执行文件仍依赖项目公共 ESL 运行时，包括 `tm_engine`、`tm_clock`、`tm_mem`
和 `pem_biu`，以及原有 DDR/PEM TOML。这些是模型运行时依赖，不是 SoC 依赖。

## 场景与配置

运行时使用两层 TOML，二者职责不同：

- `config/tm_ring_demo.toml`：Ring 场景配置，负责多核数量、交织、链路、FIFO、
  场景级 OSD 和性能目标；
- 原 `pem_config_cloud.toml`（DDR/PEM TOML）：负责 DDR 读写延迟、credit 上限、
  credit 补充周期/数量以及 BIU 公共配置。

Ring 场景 TOML 通过 `ddr_config` 字段引用 DDR TOML，路径相对于 Ring 场景 TOML。
命令行 `--ddr-config` 或脚本参数 `-DdrConfig` 可以覆盖该路径；旧名称
`--pem-config`、`-PemConfig` 继续作为兼容别名。

Ring TOML 只保留两个多核场景：

- `multi_core`：4 Master、4 个交织 Target；
- `multi_core_backpressure`：4 Master，通过缩小 Target/FIFO/Link 资源制造反压，
  用于瓶颈分类回归。

延迟、交织、FIFO、宽度、Link in-flight、Master/Global/Target OSD 和 80% 性能
目标均为可读 TOML 字段。独立场景要求 Target OSD 显式且非零，不再静默继承
DDR credit；但 Target 带宽 token 的上限和补充速率仍来自 DDR TOML，因此 DDR
配置仍然会影响吞吐、反压和最终瓶颈位置。

父项目构建出 `tm_ring_esl.exe` 后，可从 PowerShell 运行：

```powershell
.\run_tm_ring_esl.ps1 -Binary C:\path\to\tm_ring_esl.exe
.\run_tm_ring_esl.ps1 -Case multi_core_backpressure `
  -Binary C:\path\to\tm_ring_esl.exe
.\run_tm_ring_esl.ps1 -Binary C:\path\to\tm_ring_esl.exe `
  -DdrConfig C:\path\to\pem_config_cloud.toml
```

临时参数覆盖无需修改 TOML：

```powershell
.\run_tm_ring_esl.ps1 -Binary C:\path\to\tm_ring_esl.exe `
  -Set ring_link_width_bytes=8,ring_link_max_inflight=2
```

构建目标需要同时编译 `tm_ring_esl_main.cc` 和 `tm_ring_demo_config.cc`，并链接
现有 Ring 源码及公共 ESL 运行时。本源码快照没有父项目构建文件和公共 ESL 头文件，
因此最终可执行文件需要接入父项目现有构建系统。

## 瓶颈输出

`TEST_BOTTLENECK` 除保留 `ring_link_stalls` 汇总值外，还会输出：

- `ring_link_serialization_busy_stalls`：链路序列化器仍忙；
- `ring_link_inflight_limit_stalls`：Link in-flight 上限耗尽；
- `ring_link_fifo_full_stalls`：Link 内部传播 FIFO 满；
- `ring_link_downstream_fifo_full_stalls`：下游 Router 接口 FIFO 满。

四项采用互斥主因计数，之和等于 `ring_link_stalls`，避免同一次拒绝被重复归因。
当 Ring 为主瓶颈时，`dominant` 会直接报告上述具体原因，而不是笼统的
`ring_link`。
