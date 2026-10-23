FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG IMM_BUILD_JOBS=4

COPY scripts/install_dependency.sh /src/scripts/install_dependency.sh
COPY scripts/dependency /src/scripts/dependency

RUN CC=gcc-14 CXX=g++-14 JOBS="${IMM_BUILD_JOBS}" \
        /src/scripts/install_dependency.sh --prefix /usr/local \
    && ldconfig \
    && rm -rf /src/build /src/scripts

COPY server /src/server

RUN cmake \
        -S /src/server \
        -B /tmp/imm-build \
        -DBUILD_TESTING=OFF \
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
