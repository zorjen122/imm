# WIMI service configuration

Canonical YAML files for local services live in `server/conf/`.

中文版本：[配置说明](configuration.zh-CN.md)。

| File | Purpose |
| --- | --- |
| `gate.yaml` | Auth/API HTTP endpoint, verification code, and SMTP settings. |
| `file.yaml` | File service gRPC address, port, and object storage root. |
| `state-single.yaml` / `state-multi.yaml` | One-node and `G × N` local topologies. |
| `message-*.yaml` | Message service nodes. |
| `gateway-*.yaml` | Connection Gateway nodes. |
| `test-client.yaml` / `public-test.yaml` | Test-client and DAO test settings. |

The local scripts read these files by default. Use `WIMI_STATE_CONFIG`,
`WIMI_GATE_CONFIG`, `WIMI_FILE_CONFIG`, `WIMI_MESSAGE_CONFIGS`, or
`WIMI_GATEWAY_CONFIGS` to choose alternatives.

`file.yaml` resolves a relative `storageRoot` from the File executable's
directory. Use an absolute path for a fixed object-storage location.

`transportSecurity.mode: insecure` is for development only. Production
configuration requires `mode: mtls`; Gateway certificate identity must match
the registered `gateway_id`.
