# WIMI 服务配置

本地服务使用的 YAML 配置统一放在 `server/conf/`。

English: [Service configuration](configuration.md)。

| 文件 | 用途 |
| --- | --- |
| `gate.yaml` | Auth/API HTTP 地址、验证码和 SMTP 配置。 |
| `file.yaml` | File 服务的 gRPC 地址、端口和对象存储根目录。 |
| `state-single.yaml` / `state-multi.yaml` | 单节点与 `G × N` 本地拓扑。 |
| `message-*.yaml` | Message 服务节点配置。 |
| `gateway-*.yaml` | Connection Gateway 节点配置。 |
| `test-client.yaml` / `public-test.yaml` | 测试客户端和 DAO 测试配置。 |

本地脚本默认读取这些文件。需要替换时，可设置 `WIMI_STATE_CONFIG`、
`WIMI_GATE_CONFIG`、`WIMI_FILE_CONFIG`、`WIMI_MESSAGE_CONFIGS` 或
`WIMI_GATEWAY_CONFIGS`。

`file.yaml` 的相对 `storageRoot` 会以 File 可执行文件所在目录为基准；需要固定对象存储
位置时请使用绝对路径。

`transportSecurity.mode: insecure` 仅用于开发环境。生产配置必须使用 `mode: mtls`，并且
Gateway 证书中的身份必须与已注册的 `gateway_id` 一致。
