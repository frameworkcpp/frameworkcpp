# SPDX-License-Identifier: AGPL-3.0-only

# Multi-arch toolchain image to compile applications against frameworkcpp.
# Built from the Release config on tags. The official docker `ubuntu` image
# ships manifests for amd64, arm/v7, arm64/v8, ppc64le, riscv64 and s390x.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

LABEL org.opencontainers.image.source="https://github.com/frameworkcpp/frameworkcpp"
LABEL org.opencontainers.image.licenses="AGPL-3.0-only"
LABEL org.opencontainers.image.description="Toolchain with frameworkcpp installed to compile applications against the framework"

# Toolchain needed to compile applications with the framework
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        liburing-dev \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Build and install the framework into /usr (headers + CMake config)
WORKDIR /tmp/frameworkcpp
COPY . .
RUN cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DFRAMEWORKCPP_BUILD_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr \
    && cmake --build build \
    && cmake --install build \
    && rm -rf build

WORKDIR /workspace