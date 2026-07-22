# 多核 Ring ESL 独立入口

固定入口是 `tm_ring_esl_main.cc`。该入口直接实例化多个 `PemTrDemo`
Master、Ring Fabric 和 Memory Target，用来独立验证 Ring 互连，不依赖完整
SoC top。

## 单文件配置

当前只使用一份 TOML：`config/pem_config_cloud.toml`。

同一个文件内按 section 分工：

- `[ARCH]`、`[DDR]`、`[L2]`、`[BIU]`：继续给原 PEM/DDR/BIU 运行时读取。
- `[RING_DEMO]`：Ring demo 的公共场景参数，例如 Master/Target 数量、交织、
  Link 宽度、Router 输入缓存、FIFO 深度和 OSD。
- `[RING_DEMO.case.multi_core]`：`multi_core` 场景覆盖项。
- `[RING_DEMO.case.multi_core_backpressure]`：反压场景覆盖项。

`ring_router_input_depth` 属于 `[RING_DEMO]`，不要放在 `[DDR]` 中。`[DDR]`
只保留 DDR 延迟、带宽和 credit 参数。

## 运行

PowerShell 脚本默认使用 `config/pem_config_cloud.toml`：

```powershell
.\run_tm_ring_esl.ps1 -Binary C:\path\to\tm_ring_esl.exe
```

切换场景：

```powershell
.\run_tm_ring_esl.ps1 -Case multi_core_backpressure `
  -Binary C:\path\to\tm_ring_esl.exe
```

临时覆盖 Ring 参数：

```powershell
.\run_tm_ring_esl.ps1 -Binary C:\path\to\tm_ring_esl.exe `
  -Set ring_link_width_bytes=8 `
  -Set ring_router_input_depth=4
```

如果你的真实 PEM 配置在其他路径，直接传给 `-Config`：

```powershell
.\run_tm_ring_esl.ps1 -Config C:\path\to\pem_config_cloud.toml `
  -Binary C:\path\to\tm_ring_esl.exe
```

## 兼容性

解析器仍兼容旧的独立 Ring TOML 格式：`[default]` 和 `[case.<name>]`。
但推荐以后只维护 `pem_config_cloud.toml`，避免 Ring 参数和 PEM 参数读反。
