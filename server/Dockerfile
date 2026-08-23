FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG REDIS_PLUS_PLUS_VERSION=1.3.15

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        gnupg \
        libboost-filesystem-dev \
        libcurl4-openssl-dev \
        libfmt-dev \
        libgrpc++-dev \
        libhiredis-dev \
        libjsoncpp-dev \
        libprotobuf-dev \
        libspdlog-dev \
        libssl-dev \
        libyaml-cpp-dev \
        pkg-config \
        protobuf-compiler \
        protobuf-compiler-grpc \
    && curl -fsSL https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 \
        | gpg --dearmor -o /usr/share/keyrings/mysql-apt-config.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/mysql-apt-config.gpg] https://repo.mysql.com/apt/ubuntu/ noble mysql-tools" \
        > /etc/apt/sources.list.d/mysql.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends libmysqlcppconn-dev \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL \
        "https://github.com/sewenew/redis-plus-plus/archive/refs/tags/${REDIS_PLUS_PLUS_VERSION}.tar.gz" \
        -o /tmp/redis-plus-plus.tar.gz \
    && tar -xzf /tmp/redis-plus-plus.tar.gz -C /tmp \
    && cmake \
        -S "/tmp/redis-plus-plus-${REDIS_PLUS_PLUS_VERSION}" \
        -B /tmp/redis-plus-plus-build \
        -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
        -DREDIS_PLUS_PLUS_CXX_STANDARD=17 \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/redis-plus-plus-build -j"$(nproc)" \
    && cmake --install /tmp/redis-plus-plus-build \
    && ldconfig \
    && rm -rf /tmp/redis-plus-plus.tar.gz \
        "/tmp/redis-plus-plus-${REDIS_PLUS_PLUS_VERSION}" \
        /tmp/redis-plus-plus-build

COPY server /src/server

RUN cmake \
        -S /src/server \
        -B /tmp/wimi-build \
        -DBUILD_TESTING=OFF \
        -DWIMI_BUILD_UNIT_TESTS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
    && cmake --build /tmp/wimi-build -j"$(nproc)" \
    && install -D /tmp/wimi-build/state/state /opt/wimi/bin/state \
    && install -D /tmp/wimi-build/file/file /opt/wimi/bin/file \
    && install -D /tmp/wimi-build/message/message /opt/wimi/bin/message \
    && install -D /tmp/wimi-build/gateway/gateway /opt/wimi/bin/gateway \
    && install -D /tmp/wimi-build/gate/gate /opt/wimi/bin/gate \
    && install -D /tmp/wimi-build/common/libimCommon.so /opt/wimi/lib/libimCommon.so \
    && rm -rf /src/server /tmp/wimi-build

ENV LD_LIBRARY_PATH=/opt/wimi/lib:/usr/local/lib

WORKDIR /opt/wimi

STOPSIGNAL SIGTERM
