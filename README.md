<p align="center">
  <img src="client/resources/icons/wimi-client.svg" width="112" alt="WIMI">
</p>

<h1 align="center">WIMI</h1>

<p align="center">
  An experimental C++20 instant-messaging project.<br>
  <a href="README.zh-CN.md">简体中文</a> · English
</p>

> **Experimental and non-commercial.** WIMI is a learning and engineering
> experiment, not a production service. It has no public-service security or
> operational guarantee; do not expose it to the Internet or use it for
> sensitive data.

<p align="center">
  <img src="docs/icons/view.png" width="800" alt="WIMI desktop client preview">
</p>

## Functionality

WIMI provides a Qt/QML desktop chat client backed by a C++ service stack. Its
current experience focuses on the everyday chat path:

- Account registration, email verification, sign-in, and reconnecting through
  the Auth Gate.
- Direct conversations, group text messages, delivery/read acknowledgements,
  and incremental conversation sync.
- Separate sessions for devices on the same account, with delivery and sync for
  online devices.
- File messages with resumable chunked transfer and SHA-256 verification.
  PNG, JPG, and Markdown files can be previewed; text and other files can be
  downloaded locally.
- Per-account SQLite storage for local history, drafts, outgoing messages,
  sync cursors, and unfinished file transfers.
- A responsive light/dark Qt interface with desktop notifications on Linux.

## Project highlights

WIMI breaks a chat service into small, understandable parts: one part handles
sign-in, one keeps client connections alive, one processes messages, and one
handles files. This makes it a practical project for learning how a chat
application moves from a desktop UI to a service cluster.

The current server can run several Connection Gateways and several Message
nodes (`G × N`). State tells the client which Gateway to use and tells each
Gateway which Message nodes are available. A conversation can therefore keep
working across Gateway and Message service instances.

## Architecture

```mermaid
flowchart LR
    Client[Qt / QML Client]
    Gate[Auth / API Gate]
    State[State]
    File[File]
    MySQL[(MySQL)]
    Redis[(Redis)]

    subgraph Gateways[Connection Gateway cluster · G]
        G1[Gateway 1]
        G2[Gateway 2]
    end

    subgraph Messages[Message cluster · N]
        M1[Message 1]
        M2[Message 2]
    end

    Client -->|register / sign in| Gate
    Gate <-->|gateway placement| State
    Client <-->|long-lived TCP| G1
    State -->|versioned message topology| G1
    State -->|versioned message topology| G2
    G1 <-->|gRPC commands / delivery| M1
    G1 <-->|gRPC commands / delivery| M2
    G2 <-->|gRPC commands / delivery| M1
    G2 <-->|gRPC commands / delivery| M2
    M1 <-->|file RPC| File
    M2 <-->|file RPC| File
    Gate --> MySQL
    Gate --> Redis
    M1 --> MySQL
    M2 --> MySQL
    M1 --> Redis
    M2 --> Redis
```

## Try and build it

Detailed setup of this page:

- [Client guide](docs/client-guide.md) — build, run, local data, live service mode,
  tests, and Linux desktop integration.
- [Server requirements and local workflow](docs/requirements.md) — packages,
  database/Redis initialization, services, ports, and smoke tests.
- [Configuration guide](docs/configuration.md) — configuration files and
  override rules.
- [Server verification status (Chinese)](docs/feature-server-verification.md)
  — current service capability and known boundaries.

## Development

The repository is organized around a Qt client in `client/`, C++ services in
`server/`, shared protocols in `server/public/proto/`, and repeatable local
scripts in `scripts/`. The client uses C++20, Qt 6, QML, QtProtobuf, and
SQLite; services use C++20, gRPC, MySQL, and Redis.

Before changing the system, read the guides above and keep the experimental
scope in mind: TLS, secure credential storage, production deployment, and
several platform paths remain outside the verified baseline.
