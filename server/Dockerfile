FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG REDIS_PLUS_PLUS_VERSION=1.3.15

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        g++-14 \
        gcc-14 \
        gnupg \
        libboost-all-dev \
        libc-ares-dev \
        libcrypto++-dev \
        libcurl4-openssl-dev \
        libfmt-dev \
        libgnutls28-dev \
        libgrpc++-dev \
        libhiredis-dev \
        libhwloc-dev \
        libjsoncpp-dev \
        liblttng-ust-dev \
        liblz4-dev \
        libnuma-dev \
        libpciaccess-dev \
        libprotobuf-dev \
        libspdlog-dev \
        libssl-dev \
        libsctp-dev \
        libtool \
        liburing-dev \
        libxml2-dev \
        libyaml-cpp-dev \
        meson \
        ninja-build \
        pkg-config \
        protobuf-compiler \
        protobuf-compiler-grpc \
        python3-pyelftools \
        python3-yaml \
        ragel \
        valgrind \
        xfslibs-dev \
    && curl --retry 5 --retry-all-errors -fsSL https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 \
        | gpg --dearmor -o /usr/share/keyrings/mysql-apt-config.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/mysql-apt-config.gpg] https://repo.mysql.com/apt/ubuntu/ noble mysql-tools" \
        > /etc/apt/sources.list.d/mysql.list \
    && apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends libmysqlcppconn-dev \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends doxygen \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends stow \
    && rm -rf /var/lib/apt/lists/*

RUN curl --retry 5 --retry-all-errors -fsSL \
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

COPY --from=seastar-source . /tmp/seastar

RUN cd /tmp/seastar \
    && mkdir -p build/wimi-container/release/_cooking/ingredient/fmt/src \
    && curl --http1.1 --retry 10 --retry-all-errors -fsSL \
        https://github.com/fmtlib/fmt/releases/download/12.2.0/fmt-12.2.0.zip \
        -o build/wimi-container/release/_cooking/ingredient/fmt/src/fmt-12.2.0.zip \
    && ./configure.py \
        --mode=release \
        --c++-standard=23 \
        --compiler=/usr/bin/g++-14 \
        --c-compiler=/usr/bin/gcc-14 \
        --compiler-cache=none \
        --cook=fmt \
        --build-root=build/wimi-container \
        --prefix=/usr/local \
        --enable-io_uring \
        --disable-dpdk \
    && cmake --build build/wimi-container/release \
        --target seastar seastar_testing seastar_perf_testing -j2 \
    && cp -aL build/wimi-container/release/_cooking/installed/. /usr/local/ \
    && cmake --install build/wimi-container/release \
    && ldconfig \
    && rm -rf /tmp/seastar

COPY server /src/server

RUN cmake \
        -S /src/server \
        -B /tmp/wimi-build \
        -DBUILD_TESTING=OFF \
        -DWIMI_BUILD_UNIT_TESTS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-14 \
        -DCMAKE_CXX_COMPILER=g++-14 \
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
