# 架构图说明

- `system_context.mmd`：模型边界及独立 ESL 运行环境。
- `overall_architecture.mmd`：Master Adapter、Router/Link、Target Adapter、资源控制和性能监控。
- `transaction_flow.mmd`：读写事务、OSD 预留和终态响应释放流程。
- `performance_model.mmd`：延迟、带宽、排队、Target 服务和精度校准关系。

图中实线表示请求，粗线表示写数据，虚线表示响应或控制路径。所有图均为可编辑 Mermaid 文件。
