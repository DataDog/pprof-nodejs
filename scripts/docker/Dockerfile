# Image for running this project's test suite on Linux from a non-Linux dev
# machine. The native addon is built per-architecture inside the container;
# node-gyp needs python3 and a C++ toolchain. Node 24 is used so all of the
# OTEP-4947 thread-context tests run without needing to pass
# --experimental-async-context-frame.
FROM node:24-bookworm

RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
        python3 \
        build-essential \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/work
