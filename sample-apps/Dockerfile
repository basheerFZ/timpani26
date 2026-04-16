# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

#################################
# === Build Stage ===
#################################
FROM ubuntu:22.04 AS builder

ARG TARGETARCH
ENV TARGETARCH=${TARGETARCH}
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /sample-apps

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY . .

# Build sample_apps
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Prepare glibc shared libraries for Alpine runtime
WORKDIR /dummy
RUN LIBARCH=$([ "$TARGETARCH" = "arm64" ] && echo "aarch64" || echo "x86_64") && \
    mkdir -p /dummy/lib64 /dummy/${LIBARCH}-linux-gnu/ && \
    if [ "$TARGETARCH" = "amd64" ] || [ -z "$TARGETARCH" ]; then \
        cp -v /lib64/ld-linux-x86-64.so.2 /dummy/lib64/ 2>/dev/null || true && \
        cp -v /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true; \
    elif [ "$TARGETARCH" = "arm64" ]; then \
        cp -v /lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true && \
        cp -v /lib/ld-linux-aarch64.so.1 /dummy/ 2>/dev/null || true; \
    fi && \
    for lib in \
        libc.so.6 \
        libm.so.6 \
        libpthread.so.0 \
        libdl.so.2 \
        librt.so.1 \
        libgcc_s.so.1 ; do \
        cp -v /lib/${LIBARCH}-linux-gnu/$lib /dummy/${LIBARCH}-linux-gnu/ 2>/dev/null || true; \
    done

#################################
# === Runtime Stage ===
#################################
FROM alpine:3.21

ARG TARGETARCH
ENV TARGETARCH=${TARGETARCH}

WORKDIR /sample-apps

# Copy shared libraries from builder
COPY --from=builder /dummy /lib

# Setup dynamic linker for glibc
RUN if [ "$TARGETARCH" = "amd64" ] || [ -z "$TARGETARCH" ]; then \
        mkdir -p /lib64 && \
        cp -v /lib/lib64/ld-linux-x86-64.so.2 /lib64/ 2>/dev/null || true; \
    fi

# Copy built binary and shared library
COPY --from=builder /sample-apps/build/sample_apps /sample-apps/
COPY --from=builder /sample-apps/build/libttsched.so /lib/

ENTRYPOINT ["/sample-apps/sample_apps"]
CMD ["-h"]
