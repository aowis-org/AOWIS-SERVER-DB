#!/usr/bin/env bash
set -euo pipefail

# Compute a source-content fingerprint independent of file timestamps. ZIP
# extraction can otherwise leave source files older than Ninja's existing .o
# files, allowing a changed DB guard to be silently skipped during relinking.
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
model_dir="${project_dir}/external/AOWIS-SERVER-MODEL"

for tool in find sort sha256sum xargs; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Required tool is missing: %s\n' "$tool" >&2
        exit 1
    fi
done

if [[ ! -d "${model_dir}/include" || ! -d "${model_dir}/src" ]]; then
    printf 'Model submodule is missing; run ./git_submodule_init.sh first.\n' >&2
    exit 1
fi

# Include all inputs that affect the DB or its Model dependency. Use NUL
# delimiters throughout to handle unusual filenames safely.
{
    printf '%s\0' \
        "${project_dir}/CMakeLists.txt" \
        "${model_dir}/CMakeLists.txt" \
        "${project_dir}/tools/check_model_codec.py" \
        "${project_dir}/docs/model_codec_fields.json" \
        "${project_dir}/tools/build_source_fingerprint.sh"
    find "${project_dir}/include" "${project_dir}/src" "${project_dir}/tests" \
         "${model_dir}/include" "${model_dir}/src" -type f \
         \( -name '*.cpp' -o -name '*.cc' -o -name '*.c' -o -name '*.h' \
            -o -name '*.hpp' -o -name '*.hxx' -o -name '*.inl' \) -print0
} | LC_ALL=C sort -z | xargs -0 -r sha256sum -z | sha256sum | cut -d' ' -f1
