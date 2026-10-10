#!/bin/sh
# Build the payload (src/RedDead2 60FPS_v<version>.elf) inside the Linux build container (see Dockerfile).
set -eu
cd "$(dirname "$0")"
docker build -t rdr2-60fps-build .
docker run --rm -v "$PWD/src:/work" rdr2-60fps-build make
ls -l src/*.elf
