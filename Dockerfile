# Build environment from the original KurohaXR README ("Building" section),
# packaged as a container so it also works on macOS / Apple Silicon.
FROM --platform=linux/amd64 ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        clang-18 lld-18 llvm-18 llvm-18-dev make wget unzip ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Pinned SDK release (the README uses "latest"; pinning keeps builds reproducible).
ARG SDK_VERSION=v0.43
RUN wget -q -O /tmp/sdk.zip \
        https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip \
 && unzip -q -d /opt /tmp/sdk.zip \
 && rm /tmp/sdk.zip

ENV LLVM_CONFIG=/usr/bin/llvm-config-18 \
    PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

WORKDIR /work
