# Dev environment for building the recompiler toolchain without needing
# Homebrew (or any native macOS toolchain) at all. Mirrors the exact
# combination already verified to build this project cleanly: Ubuntu 24.04
# + Clang 18 + CMake + Ninja. See tools/docker/README.md for usage — this
# image intentionally contains no COPY of the repo; you clone/build inside
# a bind-mounted volume so your work persists outside the container.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        clang-18 \
        lld-18 \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18
ENV CXX=clang++-18

WORKDIR /workspace

CMD ["/bin/bash"]
