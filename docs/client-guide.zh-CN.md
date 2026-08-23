# WIMI 客户端指南

客户端是使用 Qt 6、QML 与 C++20 开发的桌面应用。它既可以使用 Fake 数据体验界面，也
可以连接本地 WIMI 服务栈。

## 构建

先安装[服务端依赖](requirements.zh-CN.md)中列出的 Qt、编译器和协议依赖，再在仓库根目录
执行：

```bash
cmake -S client -B build/client-clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build/client-clang -j2
```

## 运行

使用确定性的演示数据启动本地界面：

```bash
./build/client-clang/wimi-client --scenario=normal
```

连接已启动的本地服务时，传入 Auth Gate 地址。不同的 `--account` 会使用不同的本地
SQLite 数据库。数据库在首次登录时绑定服务端 UID，之后必须继续用于同一个账号：

```bash
./build/client-clang/wimi-client \
  --gate-url=http://127.0.0.1:18080 \
  --account=my-account
```

默认数据库位于 Qt 应用数据目录的 `accounts/` 下；如需指定位置，可传入
`--database=/path/to/account.sqlite`。

## 测试

```bash
ctest --test-dir build/client-clang --output-on-failure
cmake --build build/client-clang --target wimi-client_qmllint -j2
```

默认测试不会访问外部服务。若要运行可选的真实网络测试，请先按
[服务端依赖与本地流程](requirements.zh-CN.md)启动本地服务，在 CMake 中启用
`-DWIMI_ENABLE_LIVE_TESTS=ON`，并按测试输出设置账号环境变量。

## 范围与安全

WIMI 是实验性项目。本地服务使用面向开发的传输配置；TLS、凭证安全存储和公网部署不在
当前已验证基线内。
