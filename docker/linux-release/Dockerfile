# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build

ARG BUILD_TYPE=Release
ARG NUKED_SOURCE=linux-docker
ARG NUKED_VERSION

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends --yes \
        build-essential \
        ca-certificates \
        cmake \
        libsdl2-dev \
        librtmidi-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY doc ./doc
COPY 3rdparty/Utf8Main ./3rdparty/Utf8Main

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_INSTALL_PREFIX=/out \
        -DCMAKE_INSTALL_DOCDIR=share/doc/nuked-sc55 \
        -DNUKED_SOURCE="${NUKED_SOURCE}" \
        -DNUKED_VERSION="${NUKED_VERSION}" \
    && cmake --build /build --parallel \
    && cmake --install /build

FROM scratch

COPY --from=build /out/ /
