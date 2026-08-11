# 工作区

## 验证

```bash
./script/build-server.sh
python3 -m py_compile scripts/lib/wim_tcp_client.py
bash -n scripts/smoke/smoke-base.sh scripts/smoke/smoke-gateway-message.sh scripts/smoke/smoke-multi-chat.sh
git diff --check
```

如需进行集成测试，先启动 `scripts/run-local-services.sh`，再运行针对性的冒烟测试脚本。
运行 `scripts/smoke/smoke-multi-chat.sh` 前，请使用双节点配置。
