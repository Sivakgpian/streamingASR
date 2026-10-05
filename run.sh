#!/usr/bin/env bash
# Build (if needed) and run the WAV reader in one step, like `python3 main.py`.
#
#   ./run.sh                       # reads tests/data/en1.wav
#   ./run.sh path/to/file.wav      # reads your file
#   PRESET=clang-debug ./run.sh    # use another build preset
set -euo pipefail

cd "$(dirname "$0")"

preset="${PRESET:-clang-release}"
file="${1:-tests/data/en1.wav}"

# Configure once; after that only build. Ninja recompiles only changed files.
if [[ ! -f "build/${preset}/build.ninja" ]]; then
    cmake --preset "${preset}" > /dev/null
fi
cmake --build --preset "${preset}" | grep -vE '^\[|^ninja: no work to do' || true

"./build/${preset}/examples/sasr_wav_info" "${file}"
