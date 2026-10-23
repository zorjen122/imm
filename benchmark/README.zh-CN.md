# 独立基准客户端

测试 State 查询、Message 心跳、File 上传下载和 Gateway PING，输出成功次数、RTT 和应用层吞吐。

从仓库根目录构建（需要 CMake 4.1+ 和 C++23）：

```bash
./scripts/install_dependency.sh
cmake -S benchmark -B build/benchmark -DCMAKE_PREFIX_PATH="$HOME/.local/imm"
cmake --build build/benchmark -j 2
```

自定义依赖安装位置时，将 `CMAKE_PREFIX_PATH` 改为对应路径。

以下以中等并发作为起点，服务端资源不足时可降低连接数：

```bash
./build/benchmark/bm_state --endpoint 127.0.0.1:50052 --sent_nr 10000 --connection_nr 32
./build/benchmark/bm_message --endpoint 127.0.0.1:50055 --gateway_id benchmark --sent_nr 1000 --connection_nr 32
./build/benchmark/bm_file --endpoint 127.0.0.1:51100 --bytes 262144 --sent_nr 100 --connection_nr 16

# Gateway：先登录，再进行 PING 测试；脚本会构建客户端
./benchmark/run_gateway.sh zorjen 123456 32 4 10000
```

Gateway 示例账号需替换为实际测试账号；File 示例会留下约 400 MiB 文件。

`--sent_nr` 指每连接操作次数，`--connection_nr` 指并发连接数，`--bytes` 指单个文件大小。各程序通过 `--help` 查看参数。
Gateway 脚本可通过 `GATE_URL` 和 `GATEWAY_ENDPOINT` 指定服务地址。

主要限制：

- 服务和数据库需自行启动；客户端连接不支持 TLS/mTLS。
- Message 多 shard 部署需指定测试 `gateway_id` 所属 shard 的端口，建议先使用单 shard。
- File 会留下测试文件，需要自行清理；单个文件最大 1 MiB。
