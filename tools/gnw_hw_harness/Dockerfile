ARG BASE_IMAGE=sylverb/retro-go-sd-builder:v1.5
ARG QEMU_VERSION=8.2.2

FROM debian:bookworm-slim AS qemu-builder

ARG QEMU_VERSION=8.2.2
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl libglib2.0-dev libpixman-1-dev \
        libfdt-dev ninja-build pkg-config python3 python3-venv zlib1g-dev xz-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp
RUN curl -fsSLO "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" \
    && tar -xf "qemu-${QEMU_VERSION}.tar.xz" \
    && cd "qemu-${QEMU_VERSION}" \
    && ./configure --prefix=/opt/qemu --target-list=arm-softmmu \
         --disable-docs --disable-gtk --disable-sdl --disable-vnc \
         --disable-curl --disable-slirp --disable-werror --disable-tools \
    && ninja -C build qemu-system-arm \
    && install -D -m 0755 build/qemu-system-arm /opt/qemu/bin/qemu-system-arm

FROM ${BASE_IMAGE}

USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
        dosfstools gcc jq libc6-dev libfdt1 libglib2.0-0 libpixman-1-0 mtools zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=qemu-builder /opt/qemu /opt/qemu
COPY tools/gnw_hw_harness/container_entry.sh /opt/gnw_hw_harness/container_entry.sh
ENV PATH="/opt/qemu/bin:${PATH}"
USER builder
WORKDIR /opt/workdir
ENTRYPOINT ["/opt/gnw_hw_harness/container_entry.sh"]
