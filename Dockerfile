# syntax=docker/dockerfile:1

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Allow APT to retain downloaded packages
RUN rm -f /etc/apt/apt.conf.d/docker-clean

# Persistent package caches across builds
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get -o Acquire::Retries=5 update && \
    apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        build-essential \
        gcc \
        gdb \
        make \
        git \
        vim \
        python3 \
        python3-venv \
        ca-certificates \
        pkg-config \
        libjson-c-dev \
        libcurl4-openssl-dev \
        curl

WORKDIR /workspace

CMD ["/bin/bash"]