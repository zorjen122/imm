# IMM 服务配置

每个进程通过 `IMM_CONFIG` 指定 YAML 文件，通过 `IMM_NODE` 选择其中的节点。
同一部署的各进程使用一致的节点清单，State 从清单生成 Gateway 和 Message 拓扑。

## 配置模板

以下地址、密码和证书路径需要替换为实际部署值。证书文件由部署方提供；同一路径在不同机器上应放置对应节点自己的证书和私钥。

```yaml
shared:
  mysql:
    host: mysql.internal
    port: 3306
    user: imm
    password: REPLACE_ME
    schema: imm
    caFile: /etc/imm/tls/mysql-ca.pem
  redis:
    masterName: imm-master
    password: REPLACE_ME
    sentinelPassword: REPLACE_ME
    sentinels:
      - {host: sentinel-1.internal, port: 26379}
      - {host: sentinel-2.internal, port: 26379}
      - {host: sentinel-3.internal, port: 26379}
  transportSecurity:
    environment: production
    mode: mtls
    rootCertificates: /etc/imm/tls/ca.pem
    certificateChain: /etc/imm/tls/node.pem
    privateKey: /etc/imm/tls/node.key

nodes:
  gate:
    service: gate
    host: auth.example.com
    port: 18080
    shards: 2
    machineId: 1
    verification:
      maxAttempts: 5
      codeTtlSeconds: 180
      resendCooldownSeconds: 60
      exposeCodeInResponse: false
      email:
        enabled: true
        smtpUrl: smtps://smtp.example.com:465
        username: noreply@example.com
        password: REPLACE_ME
        # caFile: /etc/imm/tls/smtp-ca.pem  # 省略时使用系统信任库
  state:
    service: state
    host: state.internal
    listenHost: 0.0.0.0
    port: 50052
    shards: 1
  file:
    service: file
    host: file.internal
    listenHost: 0.0.0.0
    port: 51100
    shards: 1
    storageRoot: /var/lib/imm/files
  gateway-1:
    service: gateway
    host: gateway-1.example.com
    port: 8090
    shards: 2
    commandRateLimit: 100
  message-1:
    service: message
    host: message-1.internal
    listenHost: 0.0.0.0
    port: 50055
    shards: 2
    machineId: 2
    status: active
```

### 共享依赖

| 字段 | 含义 |
| --- | --- |
| `mysql.host / port` | MySQL 地址及经典协议端口。 |
| `mysql.user / password / schema` | 应用账号、密码和数据库名；部署前准备好库表与权限。 |
| `mysql.caFile` | 可选的 PEM CA 文件；配置后强制 TLS 并校验服务器证书及主机名。 |
| `redis.masterName` | Sentinel 管理的主节点名称。 |
| `redis.password / sentinelPassword` | Redis 数据节点与 Sentinel 的密码，分别用于 AUTH。 |
| `redis.sentinels[].host / port` | Sentinel 地址列表，至少一个。 |
| `transportSecurity.environment` | `development` 或 `production`；生产模式要求 Gateway–Message 使用 mTLS。 |
| `transportSecurity.mode` | Gateway–Message 使用 `insecure` 或 `mtls`。 |
| `transportSecurity.rootCertificates` | mTLS 信任的 PEM CA 文件路径。 |
| `transportSecurity.certificateChain / privateKey` | 本节点的 PEM 证书链和私钥路径。 |

### 节点字段

| 字段 | 含义 |
| --- | --- |
| `nodes` 下的名称 | 唯一节点身份；Gateway 注册身份及其客户端证书身份须与此名称一致。 |
| `service` | `gate`、`gateway`、`message`、`state` 或 `file`；Gate、State、File 各只能定义一个。 |
| `host / port` | 发布给调用方的地址和端口；Gateway 地址须能被客户端访问，内部服务地址须能被其他服务访问。 |
| `listenHost` | State、File、Message 的绑定地址，默认使用 `host`。Gate、Gateway 当前只按端口监听所有 IPv4 接口，不读取此字段。 |
| `shards` | 各服务的默认 shard 数，默认 1；显式 `--smp` 优先，Message 须与配置一致。 |
| `machineId` | Gate、Message 生成 ID 时使用的机器标识，范围 0～1023；同类服务的不同节点应使用不同值。 |
| `status` | State 只发布值为 `active` 的 Gateway/Message，默认 `active`；不主动探测节点健康。 |
| `weight` | Message 拓扑中的权重，默认 1；当前 Gateway 为每个 Message 建立连接，不按此值进行加权分配。 |
| `commandRateLimit` | Gateway 单连接每秒转发业务命令限额，默认 100，设为 0 关闭；不包含登录、PING、退出和 ACK。 |
| `storageRoot` | File 存储目录；生产部署使用持久化绝对路径，相对路径以可执行文件目录为基准。 |

Message 的监听范围为 `[port, port + shards - 1]`，显式传入 `--smp` 时必须与 `shards` 一致；防火墙和端口分配需覆盖整个范围。
State、File 的多个 shard 共用一个监听端口。

### 验证码与负载限制

Gate 的 `verification` 控制验证码：`maxAttempts` 为最大校验次数，`codeTtlSeconds` 为有效期，
`resendCooldownSeconds` 为重发间隔，时间单位为秒。`exposeCodeInResponse` 控制是否在响应中返回验证码。
`email.enabled` 控制邮件发送；`smtpUrl` 支持 `smtps://` 隐式 TLS 和 `smtp://` STARTTLS，
`username / password` 为 SMTP 账号，`caFile` 可指定自定义 CA。

Gate 支持 `IMM_VERIFY_SEND_EMAIL`、`IMM_VERIFY_EXPOSE_CODE`、`IMM_VERIFY_EMAIL_USER`、`IMM_VERIFY_EMAIL_PASS`、`IMM_VERIFY_SMTP_URL` 环境变量覆盖相应值。

以下可选块放在具体节点下，数量和字节额度均按 shard 计算，超额时明确拒绝：

| 字段 | 适用服务与含义 |
| --- | --- |
| `transaction.maxRequests / maxBytes` | Gate、Gateway、Message、File 的在途请求数和字节额度；State 仅使用 `maxRequests`。 |
| `io.maxPending / maxBytes / concurrency` | Gate、Gateway、Message 的待执行 I/O 数量、字节额度和消费者数量。 |
| `transport.maxWrites / maxWriteBytes` | Gateway、Message 的待发送数量和字节额度。 |
| `transport.maxPending / maxPendingBytes` | Gateway 等待结果或确认的请求数量和字节额度。 |

## 环境部署

**目前该项目还处于实验和开发阶段，仅建议个人部署，而非生产性部署。**

1. 准备 MySQL 库表、应用账号、Redis/Sentinel 和 SMTP；使用独立凭据，不执行会重置数据的 `scripts/init-mysql.sh`。
2. 将节点的发布地址、证书与 File 持久化目录写入配置。证书须覆盖连接时使用的主机名；Gateway 的客户端证书还须包含与节点名一致的身份。
3. 各机器安装依赖并构建服务，将配置及私钥限制为运行账号可读，使用进程管理器启动并监控各节点。
4. 按机器资源分配 CPU 和内存，分别设置 Seastar 的 `--smp` 和 `--memory`；数据库连接数和请求额度会随 shard 数增加。
5. 对外仅开放 Gate 和 Gateway 的受保护入口，内部服务及存储端口限制在可信网络，并为数据库和文件配置备份。

例如启动一个 Message 节点：

```bash
IMM_CONFIG=/etc/imm/services.yaml IMM_NODE=message-1 \
  /opt/imm/bin/message --smp 2 --memory 2G --log_level info
```

配置在启动时加载，修改后需重启对应进程。`IMM_EXTRA_CONFIG` 可指定仅包含 `nodes` 的追加文件，不允许重复节点名或覆盖共享配置；它不是动态服务发现机制。

## 当前安全支持情况

当前仍是实验性服务端，以上是部署组织方式，不代表已具备完整的生产安全保障。

| 链路或能力 | 当前支持与限制 |
| --- | --- |
| 客户端 → Gate / Gateway | 原生入口为 HTTP / TCP，无内置 TLS；对外部署需分别通过 HTTPS 反向代理和 TCP TLS 代理保护，并限制后端直连。 |
| Gateway ↔ Message | 支持 mTLS，Message 校验 Gateway 证书身份与注册节点名一致；`production` 模式拒绝此链路使用明文。 |
| Gate/Gateway → State、Message → File | 当前为明文 gRPC，State/File 未实现调用方认证；必须限制内部访问，跨不可信网络需另行提供加密与认证保护。 |
| MySQL | 配置 `caFile` 后启用强制 TLS 和服务器证书校验；未配置时不强制 TLS。 |
| Redis / Sentinel | 支持密码 AUTH，当前未接入 TLS。 |
| SMTP | 支持隐式 TLS、STARTTLS 和证书校验。 |
| 账号与验证码 | 有验证码有效期、校验次数和重发间隔限制；账号密码目前仍直接存储和比较，尚未实现密码哈希。 |
