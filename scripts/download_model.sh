#!/usr/bin/env bash
# Downloads the Whisper tiny.en model used by WhisperCppBackend
# (src/asr/whisper_cpp_backend.*, built with -DSASR_WITH_WHISPER=ON).
# Not committed (models/ is gitignored): run this once.
#
#   ./scripts/download_model.sh [output_dir]   # default: models/
set -euo pipefail

out_dir="${1:-models}"
url="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"
sha256="921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f"
out="${out_dir}/ggml-tiny.en.bin"

mkdir -p "${out_dir}"

if [[ -f "${out}" ]] && echo "${sha256}  ${out}" | sha256sum -c - > /dev/null 2>&1; then
    echo "already have ${out} (sha256 verified)"
    exit 0
fi

echo "downloading ${url}"
echo "         -> ${out}"
curl -fL --progress-bar -o "${out}.tmp" "${url}"

if ! echo "${sha256}  ${out}.tmp" | sha256sum -c -; then
    echo "error: downloaded file does not match the pinned sha256" >&2
    rm -f "${out}.tmp"
    exit 1
fi

mv "${out}.tmp" "${out}"
echo "ok: ${out}"
