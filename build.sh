#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build AETHER with fm1-base (base/, a git submodule): ./build.sh [--host | --web | --release X.Y]
# The firmware needs the JieLi toolchain and the AC79 SDK (see README); --host needs only cc and python3.
set -e
cd "$(dirname "$0")"
[ -f base/tools/build.py ] || git submodule update --init --recursive
exec "${PYTHON:-python3}" base/tools/build.py app "$@"
