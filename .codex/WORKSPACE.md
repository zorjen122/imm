# 工作区

## 项目结构

- C++ 服务位于 `server/{gate,state,chat,file}` 下。
- 共享的 protobuf、数据库、Redis、网络和日志代码位于 `server/public` 下。
- `server/verify` 是一个 Node.js gRPC 服务。
- 本地配置应放在 `server/conf` 中；构建产物应放在 `build/wim` 中。

## 验证

```bash
./script/build-server.sh
python3 -m py_compile scripts/lib/wim_tcp_client.py
bash -n scripts/smoke/smoke-base.sh scripts/smoke/smoke-gateway-message.sh scripts/smoke/smoke-multi-chat.sh
git diff --check
```

如需进行集成测试，先启动 `scripts/run-local-services.sh`，再运行针对性的冒烟测试脚本。
运行 `scripts/smoke/smoke-multi-chat.sh` 前，请使用双节点配置。
