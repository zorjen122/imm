# 工作区

## 验证

按改动范围选择验证：C++、Proto 或服务配置改动至少执行宿主机构建与基础测试；跨节点行为改动还应执行宿主机集成 smoke；Compose 改动执行配置检查。

### 宿主机构建与基础测试

```bash
./scripts/build-server.sh
ctest --test-dir build/imm-server --output-on-failure
python3 -m compileall -q scripts/smoke/src
bash -n scripts/smoke/*.sh
git diff --check
```

### 宿主机集成 smoke

第一个终端初始化测试库并启动默认的双 Gateway、双 Message 拓扑。`init-mysql.sh` 会重置测试表和种子数据。

```bash
./scripts/build-server.sh
./scripts/init-mysql.sh
./scripts/run-local-services.sh --cluster --stop-existing
```

服务就绪后，在第二个终端运行跨节点 smoke：

```bash
./scripts/smoke/smoke-multi-chat.sh
```

### 测试目录

`server/test/unit/` 保留配置、传输安全配置、协议校验、I/O 背压和文件存储等关键单元测试，统一通过 CTest 运行。
`server/test/integrated/` 仅作占位，暂不维护集成用例；跨服务验证使用上述 `scripts/smoke/` 流程。

首次使用先运行 `./scripts/install_dependency.sh`；Ubuntu 系统开发包通过 apt 补齐，匹配的 gRPC、Protobuf、Abseil、RE2、MariaDB 和 Seastar
默认安装在 `$HOME/.local/imm`。
服务端构建只链接安装结果。`build/` 可以清理；`build/deps/` 是可选的安装源码缓存，
`build/dependency/` 是安装构建缓存，不影响已经安装的库。

### 基础容器部署配置检查

```bash
docker compose config --quiet
```

基础 Compose 部署配置位于 `loopback/compose/`，不维护额外的 Docker 集群测试。

## 提交流程

仅在用户明确要求创建提交时执行。提交前检查工作区和暂存区，只纳入本次任务相关改动，并确认对应验证已通过。若修改范围包含 `server/`，还必须执行：

```bash
./scripts/check-format.sh
```

提交采用“用户告知 → 助手回答 → 用户确认 → 执行提交”的确认流程：

1. 用户告知需要创建提交。
2. 助手检查改动范围和验证结果，给出拟提交文件、提交标题及中文正文；此时不执行暂存或提交。
3. 用户确认提交方案。
4. 助手仅暂存已确认的文件，复核暂存区后创建提交，并报告提交 ID 和结果。

注意，提交流程中总是不可以主动的 git push 提交，应由用户完成。

提交遵循 Angular 规范：标题使用英文 `<type>(<scope>): <subject>`，正文使用通俗、简明的中文说明：

- `why`：为什么需要这次修改。
- `how`：采用什么方式实现。
- `result`：修改后得到什么效果。没有显著效果省略。
- `use`：功能性增加时说明怎么使用；不适用时省略。

示例：

```text
refactor(config): unify local topology configuration

why：需要验证双 Gateway、双 Message 的容器内跨节点链路。
how：统一共享依赖和具名节点，集群文件只追加节点。
result：State 拓扑与服务配置共用同一份节点定义。
use：运行 ./scripts/run-local-services.sh --cluster。
```
