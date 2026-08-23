<p align="center">
  <img src="client/resources/icons/wimi-client.svg" width="112" alt="WIMI">
</p>

<h1 align="center">WIMI</h1>

<p align="center">
  一个实验性的 C++20 即时通讯项目。<br>
  中文 · <a href="README.md">English</a>
</p>

> **实验性项目。** WIMI 用于学习和工程实践；

<p align="center">
  <img src="docs/icons/view.png" width="680" alt="WIMI 桌面客户端预览">
</p>

## 功能

WIMI 提供基于 Qt/QML 的桌面聊天客户端，并配套 C++ 服务端。当前侧重于系统的可靠可用能力，
其中核心功能包括：

- 通过 Auth Gate 完成注册、邮箱验证码、登录和重连。
- 单聊、群文本消息、送达/已读确认，以及会话增量同步。
- 同一账号的设备会话隔离，并向在线设备投递和同步消息。
- 支持断点续传和 SHA-256 校验的文件消息。PNG、JPG、Markdown 可预览；文本和其他
  文件可下载到本地。
- 按账号隔离的 SQLite 本地数据：历史消息、草稿、待发消息、同步游标和未完成文件传输。
- 支持浅色/深色与响应式布局的 Qt 界面；Linux 提供桌面通知。

## 架构

<p align="center">
  <img src="docs/icons/architecture.png" width="100%" alt="WIMI 服务架构">
</p>

## 尝试与构建

构建和联调细节请参阅：

- [客户端指南](docs/client-guide.zh-CN.md)：构建、运行、本地数据、真实服务模式、测试和 Linux
  桌面集成。
- [服务端部署](docs/requirements.zh-CN.md)：依赖安装、MySQL/Redis 初始化、服务启动、
  端口和 smoke 测试。
- [配置说明](docs/configuration.zh-CN.md)：配置文件与覆盖规则。
- [客户端验证状态](docs/client-feature-verification.zh-CN.md)：当前客户端核心能力与验证状态。
- [服务端验证状态](docs/server-feature-verification.zh-CN.md)：当前服务端核心能力与验证状态。
