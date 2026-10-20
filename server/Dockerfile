FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

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
        libprotoc-dev \
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
    && rm -rf /var/lib/apt/lists/*

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends doxygen \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends stow \
    && rm -rf /var/lib/apt/lists/*

COPY --from=seastar-source . /tmp/seastar

RUN cd /tmp/seastar \
    && mkdir -p build/imm-container/release/_cooking/ingredient/fmt/src \
    && curl --http1.1 --retry 10 --retry-all-errors -fsSL \
        https://github.com/fmtlib/fmt/releases/download/12.2.0/fmt-12.2.0.zip \
        -o build/imm-container/release/_cooking/ingredient/fmt/src/fmt-12.2.0.zip \
    && ./configure.py \
        --mode=release \
        --c++-standard=23 \
        --compiler=/usr/bin/g++-14 \
        --c-compiler=/usr/bin/gcc-14 \
        --compiler-cache=none \
        --cook=fmt \
        --build-root=build/imm-container \
        --prefix=/usr/local \
        --enable-io_uring \
        --disable-dpdk \
    && cmake --build build/imm-container/release \
        --target seastar seastar_testing seastar_perf_testing -j2 \
    && cp -aL build/imm-container/release/_cooking/installed/. /usr/local/ \
    && cmake --install build/imm-container/release \
    && ldconfig \
    && rm -rf /tmp/seastar

COPY server /src/server

ARG IMM_BUILD_JOBS=4

RUN cmake \
        -S /src/server \
        -B /tmp/imm-build \
        -DBUILD_TESTING=OFF \
        -DIMM_BUILD_UNIT_TESTS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-14 \
        -DCMAKE_CXX_COMPILER=g++-14 \
    && cmake --build /tmp/imm-build -j"${IMM_BUILD_JOBS}" \
    && install -D /tmp/imm-build/state/state /opt/imm/bin/state \
    && install -D /tmp/imm-build/file/file /opt/imm/bin/file \
    && install -D /tmp/imm-build/message/message /opt/imm/bin/message \
    && install -D /tmp/imm-build/gateway/gateway /opt/imm/bin/gateway \
    && install -D /tmp/imm-build/gate/gate /opt/imm/bin/gate \
    && install -D /tmp/imm-build/common/libimCommon.so /opt/imm/lib/libimCommon.so \
    && rm -rf /src/server /tmp/imm-build

ENV LD_LIBRARY_PATH=/opt/imm/lib:/usr/local/lib

WORKDIR /opt/imm

STOPSIGNAL SIGTERM
