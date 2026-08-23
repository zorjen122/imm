# 工作区

## 验证

按改动范围选择验证：C++、Proto 或服务配置改动至少执行宿主机构建与基础测试；Dockerfile、Compose 或跨节点行为改动还应执行 Docker 服务集群 smoke。

### 宿主机构建与基础测试

```bash
./scripts/build-server.sh
ctest --test-dir build/wimi-server --output-on-failure
python3 -m compileall -q scripts/smoke/src
bash -n scripts/smoke/*.sh
git diff --check
```

### 宿主机集成 smoke

第一个终端初始化测试库并启动默认的双 Gateway、双 Message 拓扑。`init-mysql.sh` 会重置测试表和种子数据。

```bash
./scripts/build-server.sh
./scripts/init-mysql.sh
./scripts/run-local-services.sh --stop-existing
```

服务就绪后，在第二个终端运行跨节点 smoke：

```bash
./scripts/smoke/smoke-multi-chat.sh
```

### Docker 服务集群 smoke

需要 Docker Compose，以及宿主机上的 `python3`、`mysql`、`redis-cli`：

```bash
docker compose config --quiet
docker compose -f compose.yaml -f compose.service-cluster.yaml config --quiet
./scripts/smoke/smoke-compose-service-cluster.sh
```

该脚本启动隔离的 10 容器测试项目，验证双 Gateway、双 Message 的跨节点链路。退出时删除本次测试的容器、网络和数据卷，但保留镜像与 Docker 构建缓存；`8090`、`8091`、`18080` 需要可用。

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
feat(docker): add service cluster smoke

why：需要验证双 Gateway、双 Message 的容器内跨节点链路。
how：增加 Compose 覆盖、第二节点配置和一键 smoke 包装脚本。
result：可以在隔离环境中完成跨节点消息、好友和数据落库验证，并自动清理测试资源。
use：运行 ./scripts/smoke/smoke-compose-service-cluster.sh。
```
