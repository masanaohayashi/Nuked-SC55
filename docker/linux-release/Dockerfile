# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build

ARG BUILD_TYPE=Release

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends --yes \
        build-essential \
        ca-certificates \
        libasound2-dev \
        libcurl4-openssl-dev \
        libfontconfig1-dev \
        libfreetype6-dev \
        libgtk-3-dev \
        libwebkit2gtk-4.0-dev \
        make \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# LinuxMakefile is generated from Plugins/Nuked-SC55.jucer by Projucer. The
# release image uses that exporter because it is the project's source of truth
# for the JUCE standalone and VST3 targets.
COPY 3rdparty/JUCE ./3rdparty/JUCE
COPY 3rdparty/R2JUCE ./3rdparty/R2JUCE
COPY 3rdparty/Utf8Main ./3rdparty/Utf8Main
COPY Plugins/Builds/LinuxMakefile ./Plugins/Builds/LinuxMakefile
COPY Plugins/JuceLibraryCode ./Plugins/JuceLibraryCode
COPY Plugins/Source ./Plugins/Source
COPY src/backend ./src/backend
COPY src/common ./src/common

RUN test -f Plugins/Builds/LinuxMakefile/Makefile \
    && test -f Plugins/JuceLibraryCode/JuceHeader.h \
    && make -C Plugins/Builds/LinuxMakefile CONFIG="${BUILD_TYPE}" clean \
    && make -C Plugins/Builds/LinuxMakefile CONFIG="${BUILD_TYPE}" -j"$(nproc)" all \
    && install -Dm755 Plugins/Builds/LinuxMakefile/build/SC-55 /out/bin/SC-55 \
    && mkdir -p /out/lib/vst3 \
    && cp -R Plugins/Builds/LinuxMakefile/build/SC-55.vst3 /out/lib/vst3/SC-55.vst3

FROM scratch

COPY --from=build /out/ /
