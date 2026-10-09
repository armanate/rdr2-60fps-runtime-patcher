#!/bin/sh
# Build src/rdr2-60fps.elf inside the Linux build container (see Dockerfile).
set -eu
cd "$(dirname "$0")"
docker build -t rdr2-60fps-build .
docker run --rm -v "$PWD/src:/work" rdr2-60fps-build make
ls -l src/rdr2-60fps.elf
