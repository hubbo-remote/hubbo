# One image for all native and firmware work: the portable C++ core and its
# tests (CMake + GoogleTest), the hub-sim simulator, and the ESP32 firmware
# (PlatformIO). Nothing gets installed on the host.
#
# The image runs as a user whose uid/gid match the host user, so build output
# in the bind-mounted repo stays owned by you, not root. bin/hubbo passes
# HOST_UID/HOST_GID.
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build libgtest-dev \
      python3 python3-venv git ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

# Pinned. PlatformIO installs the ESP32 platform and toolchains on first use
# into PLATFORMIO_CORE_DIR, which is a named volume so they persist.
ARG PLATFORMIO_VERSION=6.1.18
RUN python3 -m venv /opt/platformio \
 && /opt/platformio/bin/pip install --no-cache-dir "platformio==${PLATFORMIO_VERSION}"

ARG HOST_UID=1000
ARG HOST_GID=1000
RUN groupadd -g "${HOST_GID}" dev \
 && useradd -m -u "${HOST_UID}" -g "${HOST_GID}" dev \
 && mkdir /platformio && chown dev:dev /platformio

ENV PATH=/opt/platformio/bin:$PATH \
    PLATFORMIO_CORE_DIR=/platformio

USER dev
WORKDIR /work
