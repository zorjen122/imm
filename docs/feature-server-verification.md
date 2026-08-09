# wimi Server 功能清单与验证状态

最后更新：2026-08-08。

本文只描述 `server/` 当前主线运行态，不追踪本机个人开发目录或历史参考代码。

## 状态口径

| 状态 | 含义 |
| --- | --- |
| 已验证 | 已有构建、CTest、smoke 或本地端到端验证覆盖。 |
| 部分验证 | 主路径可运行或有脚本覆盖，但故障分支、干净构建、外部依赖或生产配置尚未完整验证。 |
| 待验证 | 代码入口存在，但当前没有端到端断言。 |
| 未实现 | 协议或入口存在，业务逻辑仍为空或未接通。 |
| 非主线 | 为历史兼容或参考保留，不属于当前目标拓扑。 |

## 当前运行态

| 模块 | 当前职责 | 关键索引 |
| --- | --- | --- |
| Auth/API Gate | HTTP 注册、登录、验证码、短期连接 token、向 State 选择 Connection Gateway。 | [Service.cc](../server/gate/src/service/Service.cc)、[VerificationService.cc](../server/gate/src/service/VerificationService.cc) |
| State | 返回可连接 Gateway，返回版本化 Message 节点拓扑。 | [state.proto](../server/public/proto/state.proto)、[service.cc](../server/state/src/service/service.cc) |
| Connection Gateway | 客户端 TCP/TLV 长连接、token 校验、session lease、限流、发送队列、ACK/重传、Gateway -> Message 双向流客户端。 | [GatewaySession.cc](../server/gateway/src/GatewaySession.cc)、[MessageLink.cc](../server/gateway/src/MessageLink.cc) |
| Message 节点 | Message Core：业务命令处理、消息持久化、conversation_seq、幂等、关系/群/文件业务。 | [GatewayStreamService.cc](../server/message/src/service/GatewayStreamService.cc)、[MessageService.cc](../server/message/src/service/MessageService.cc) |
| File | 分片上传、断点续传、下载与 SHA-256 校验的对象存储服务。 | [Service.cc](../server/file/src/service/Service.cc)、[FileRpc.cc](../server/message/src/service/FileRpc.cc) |
| MySQL | 用户、关系、群、conversation、messages 等持久状态。 | [Mysql.h](../server/public/include/Mysql.h)、[init_message_mysql_test.sql](../scripts/sql/init_message_mysql_test.sql) |
| Redis | 验证码、短期连接 token、session lease、消息/用户 ID 生成等短状态。 | [Redis.h](../server/public/include/Redis.h) |

主链路已经从旧的 `Client -> Chat` 改为：

```text
Client -> Auth/API Gate -> State
Client -> Connection Gateway -> Message node
Message node -> Connection Gateway -> Client
```

Message 节点之间不再建立业务转发连接；Gateway 之间也不直接转发业务消息。

## 功能清单

| 编号 | 功能 | 状态 | 当前行为与验证入口 |
| --- | --- | --- | --- |
| S1 | C++20 server 总工程构建 | 已验证 | `server/CMakeLists.txt` 已统一 `CMAKE_CXX_STANDARD 20`，构建命令为 `cmake -S server -B build/wimi`、`cmake --build build/wwimi-j2`。 |
| S2 | public protobuf/gRPC 生成 | 已验证 | `tcp_message.proto`、`state.proto`、`gateway_message.proto` 由 `server/public` 生成；客户端只依赖 TCP 公开协议，Gateway-Message 协议只在服务端内部使用。 |
| S3 | MySQL 初始化 | 已验证 | `./scripts/init_mysql.sh` 初始化 `chatServ` 与测试数据；消息模型包含 `conversations`、`conversationUserStates`、`messages`、`devices`、`conversationDeviceStates`、`fileObjects` 和 `fileUploads`。文件消息的 `messages.content` 保存 `fileId`。好友使用 canonical uid pair 和 `friendshipId`，群表使用明确的 `groupId`。 |
| S4 | 本地服务编排 | 部分验证 | `./scripts/run_local_services.sh` 启动 state/file/message/gateway/gate，并支持 `--stop-existing`。干净构建下仍有历史测试客户端产物依赖。 |
| S5 | State 选择 Connection Gateway | 已验证 | Gate 登录调用 `PickConnectionGateway`，返回 `ip/port/gatewayId`，不再向客户端返回 Message 节点地址。 |
| S6 | State 返回 Message 拓扑 | 部分验证 | Gateway 每 5 秒调用 `ListMessageNodes` 拉取带 `topology_version` 的 Message 节点快照；节点增删与排空故障分支仍需专项测试。 |
| S7 | Gate 内置邮箱验证码 | 已验证 | `/post-verifycode` 由 C++ Gate 进程内处理，Redis Lua 原子控制 TTL、重发冷却、错误次数；本地可返回 `verificationCode`，生产应关闭响应暴露并启用 SMTP。 |
| S8 | 注册 | 已验证 | `/post-signUp` 校验用户名、邮箱、一次性验证码后写入 MySQL；验证码消费后不能重放。 |
| S9 | 登录与连接 token | 已验证 | `/post-signIn` 校验账号密码，返回 Gateway 地址、`gatewayId`、`chatToken` 和 TTL；Gateway 登录时验证 Redis 中的短期 token。 |
| S10 | 忘记密码/重置密码 | 待验证 | `/post-forget-password` 已接入邮箱归属校验、验证码消费和 MySQL 密码更新，但尚未用临时账号做完整端到端断言。 |
| S11 | Gateway TCP 登录与 session lease | 已验证 | `ID_LOGIN_INIT_REQ` 在 Gateway 本地处理并登记客户端生成的 `deviceId`；登录成功后写入 `im:session:<uid>:<deviceId>`，包含 `deviceId/gatewayId/instanceId/connectionId/generation`。仅同一用户同一设备的新 generation 会替换旧连接。 |
| S12 | Gateway 本地控制命令 | 部分验证 | `ID_PING_REQ`、`ID_USER_QUIT_REQ`、TRANSPORT ACK 在 Gateway 本地处理；ACK 重传取消和慢连接关闭仍需要更细的专项断言。 |
| S13 | Gateway -> Message 双向 gRPC 长流 | 部分验证 | Gateway 向每个 Message 节点建立一条 `Connect` 流，首帧 `RegisterGateway`，注册成功后进入 healthy；心跳、队列串行写、指数退避重连已实现，流断开后的精确故障恢复仍需专项验证。 |
| S14 | Gateway 业务命令转发 | 部分验证 | 登录/退出/心跳以外的业务包封装为 `CommandEnvelope`，用 `request_id` 多路复用响应；有 conversation 的请求按健康 Message 集合做亲和路由，无 conversation 的请求走 least-inflight。 |
| S15 | Message 端连接 fencing | 已验证 | Message 处理命令前重新查询 Redis session lease，校验 Gateway、instance、connection 和 generation，拒绝旧连接或伪造身份。 |
| S16 | 单聊文本消息闭环 | 已验证 | Message 原子持久化唯一一份单聊文本，生成 `messageId/conversationSeq`，返回 ACCEPTED，并向接收者全部在线设备及发送者其他设备读扩散；重复 `clientMessageId` 且内容一致返回原结果，内容冲突返回不可重试错误。 |
| S17 | 群聊文本消息闭环 | 已验证 | 群文本由 Message Core 分配同一会话内递增 `conversationSeq`，按成员及其全部在线设备 fan-out；smoke 覆盖两名成员看到 `[1, 2]` 顺序。 |
| S18 | conversation_seq 同步 | 已验证 | `ID_PULL_SESSION_MESSAGE_LIST_REQ` 支持按 `conversationId/afterSeq/limit` 拉取；客户端连续序号立即入库，乱序包等待 200ms，缺口仍存在时按最后连续 `conversationSeq` 补拉。 |
| S19 | DELIVERED/READ ACK | 已验证 | Gateway 内部返回排队结果；客户端 SQLite 提交后发送 DELIVERED，Message 更新该设备的 `persistedSeq`。READ 只允许私聊接收者改变消息全局状态，发送者其他设备的 ACK 仅推进自身会话/设备游标。 |
| S20 | 好友申请/回复/列表 | 已验证 | 好友申请、申请列表、回复和好友列表继续由 Message Core 执行业务逻辑，已有 relationship smoke 覆盖主路径。 |
| S21 | 建群/入群申请/审批 | 已验证 | 创建群、入群申请和审批已接入 Message Core，已有 relationship smoke 覆盖主路径。 |
| S22 | 文件上传与续传 | 部分验证 | 客户端按 1 MiB PDU 上传；Message 持久化确认 offset，File 服务按 `ownerUid/fileId` 写入 `.part`/完成对象，并校验重复块与 SHA-256。`integration_file_client_test` 覆盖服务内上传、续传、下载和摘要校验；尚未完成跨进程真实服务 smoke。 |
| S23 | 真实 SMTP 发信 | 待验证 | Gate 已通过 libcurl 实现 SMTP/TLS 投递，当前缺真实邮箱授权码和外部投递断言。 |
| S24 | gRPC mTLS/节点身份校验 | 部分验证 | Gateway-Message 和 Message server 使用统一安全配置构建 credentials；本地允许 insecure，生产 mTLS 证书链仍需部署验证。 |
| S25 | 查找用户 | 未实现 | 旧 `SerachUser` 逻辑没有注册到当前 `Service::Init`，当前协议请求会返回 NotFound。 |
| S26 | 点对点文件发送与接收 | 部分验证 | 上传完成后以 `ID_FILE_SEND_REQ` 创建 `messages.type=FILE`，其 `content` 保存 `fileId`。下载按会话成员权限查询对象，历史同步和在线推送携带完整 `FileInfo`。尚未完成跨进程真实服务 smoke。 |
| S27 | 群退出/群成员拉取 | 未实现 | 群退出和群成员/群消息拉取尚未形成当前主线闭环。 |
| S28 | KafkaProducer | 非主线 | 代码仍存在，但当前消息路径不依赖 Kafka；第一阶段明确不引入 MQ/Outbox。 |

## 关键验证脚本

| 脚本/命令 | 覆盖范围 | 当前注意事项 |
| --- | --- | --- |
| `cmake -S server -B build/wimi` | 配置 server 总工程、生成 protobuf/gRPC 代码。 | 已验证。 |
| `cmake --build build/wimi -j2` | 编译 gate/state/file/message/gateway/public tests。 | 已验证。 |
| `ctest --test-dir build/wimi --output-on-failure` | public、state、file、gate 的当前 CTest。 | 已验证。 |
| `./scripts/check_format.sh` | C++/脚本基础格式检查。 | 已验证。 |
| `./scripts/run_local_services.sh --stop-existing` | 停旧服务并启动本地联调拓扑。 | 脚本仍检查旧测试客户端产物；干净构建下不可作为独立验收入口。 |
| `./scripts/smoke_gateway_message.sh` | G x N 流数量、跨 Gateway 单聊、幂等冲突、群聊 seq、同步修复。 | 需要先用 `G=2、N=2` 配置启动本地服务。 |
| `./scripts/smoke_text_delivery.sh` | 在线文本投递、身份 canonicalize、DELIVERED/READ ACK 状态推进。 | 默认连接 Gateway TCP 端口；脚本变量名仍为 `CHAT_HOST/CHAT_PORT`。 |
| `./scripts/smoke.sh` | 验证码、注册、登录路由、离线文本、文件上传、在线直推。 | 仍引用旧测试客户端工作目录；干净构建下不可作为独立验收入口。 |
