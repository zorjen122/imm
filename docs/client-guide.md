# WIMI client guide

The client is a Qt 6, QML, and C++20 desktop application. It can run with fake
data for UI exploration or connect to a local WIMI service stack.

中文版本：[客户端指南](client-guide.zh-CN.md)。

## Build

Install the Qt, compiler, and protocol dependencies listed in
[server requirements](requirements.md), then configure and build from the
repository root:

```bash
cmake -S client -B build/client-clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build/client-clang -j2
```

## Run

Run the local UI with deterministic demonstration data:

```bash
./build/client-clang/wimi-client --scenario=normal
```

To connect to locally running services, pass the Auth Gate address. A different
`--account` value keeps each local SQLite database separate. The database is
bound to the server UID on first sign-in and must then stay with that account:

```bash
./build/client-clang/wimi-client \
  --gate-url=http://127.0.0.1:18080 \
  --account=my-account
```

The default database location is Qt's application-data directory under
`accounts/`; use `--database=/path/to/account.sqlite` when an explicit path is
needed.

## Test

```bash
ctest --test-dir build/client-clang --output-on-failure
cmake --build build/client-clang --target wimi-client_qmllint -j2
```

The standard tests do not call external services. For the optional live network
test, start the local service stack described in [server requirements](requirements.md),
configure with `-DWIMI_ENABLE_LIVE_TESTS=ON`, and provide the credentials listed
by the test's CMake output.

## Scope and safety

WIMI is experimental. The local stack uses development-oriented transport
settings; TLS, secure credential storage, and public deployment are outside the
verified baseline.
