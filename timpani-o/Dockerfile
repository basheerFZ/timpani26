# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

#################################
# === Build Stage ===
#################################
FROM ubuntu:22.04 AS builder

ARG TARGETARCH
ENV TARGETARCH=${TARGETARCH}
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /timpani-o

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY . .

# Version info
ARG VERSION=2026.04.1
LABEL version="${VERSION}" \
      project="timpani" \
      component="timpani-o"
RUN echo "${VERSION}" > VERSION

# Build timpani-o
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Prepare glibc shared libraries for Alpine runtime
WORKDIR /dummy
RUN if [ "$TARGETARCH" = "amd64" ] || [ -z "$TARGETARCH" ]; then \
        LIBARCH="x86_64"; \
        mkdir -p /dummy/lib64 /dummy/${LIBARCH}-linux-gnu/ && \
        cp -v /lib64/ld-linux-x86-64.so.2 /dummy/lib64/ 2>/dev/null || true && \
        cp -v /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true; \
    elif [ "$TARGETARCH" = "arm64" ]; then \
        LIBARCH="aarch64"; \
        mkdir -p /dummy/${LIBARCH}-linux-gnu/ && \
        cp -v /lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true && \
        cp -v /lib/ld-linux-aarch64.so.1 /dummy/ 2>/dev/null || true; \
    fi && \
    LIBARCH=$([ "$TARGETARCH" = "arm64" ] && echo "aarch64" || echo "x86_64") && \
    for lib in \
        libc.so.6 \
        libstdc++.so.6 \
        libgcc_s.so.1 \
        libm.so.6 \
        libpthread.so.0 \
        libdl.so.2 \
        librt.so.1 \
        libgrpc++.so* \
        libgrpc.so* \
        libprotobuf.so* \
        libabsl*.so* \
        libcares.so* \
        libaddress_sorting.so* \
        libgpr.so* \
        libupb.so* \
        libre2.so* \
        libz.so.1 \
        libssl.so* \
        libcrypto.so* \
        liblzma.so.5 \
        liblz4.so.1 \
        libzstd.so.1 \
        libgcrypt.so.20 \
        libgpg-error.so.0 \
        libcap.so.2 ; do \
        cp -v /lib/${LIBARCH}-linux-gnu/$lib /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true; \
        cp -v /usr/lib/${LIBARCH}-linux-gnu/$lib /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true; \
    done

#################################
# === Runtime Stage ===
#################################
FROM alpine:3.21

ARG TARGETARCH
ENV TARGETARCH=${TARGETARCH}

WORKDIR /timpani-o

# Copy shared libraries from builder
COPY --from=builder /dummy /lib

# Setup dynamic linker for glibc
RUN if [ "$TARGETARCH" = "amd64" ] || [ -z "$TARGETARCH" ]; then \
        mkdir -p /lib64 && \
        cp -v /lib/lib64/ld-linux-x86-64.so.2 /lib64/ 2>/dev/null || true; \
    elif [ "$TARGETARCH" = "arm64" ]; then \
        echo "Using aarch64 runtime"; \
    fi

# Copy built binary
COPY --from=builder /timpani-o/build/timpani-o /timpani-o/

# Copy example configuration
COPY --from=builder /timpani-o/examples/node_configurations.yaml /timpani-o/examples/

# Expose ports
# 50052: gRPC SchedInfoServer (Pullpiri -> timpani-o)
# 50060: gRPC OrchestratorServer (timpani-n <-> timpani-o)
EXPOSE 50052 50060

# Environment variables for configuration
ENV SINFO_PORT=50052
ENV ORCHESTRATOR_PORT=50060
ENV FAULT_HOST=localhost
ENV FAULT_PORT=50053

ENTRYPOINT ["/timpani-o/timpani-o"]
CMD ["-s", "50052"]
