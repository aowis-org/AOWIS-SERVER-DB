#!/usr/bin/env bash
set -euo pipefail

# Resolve the project directory independently of the caller's current directory.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "${0##*/}" >&2
    exit 2
fi

model_path="external/AOWIS-SERVER-MODEL"
if [[ "$(git ls-files --stage -- "$model_path")" != '160000 '* ]]; then
    printf 'Missing submodule gitlink for %s. Register it first:\n' "$model_path" >&2
    printf '  git submodule add https://github.com/aowis-org/AOWIS-SERVER-MODEL.git %s\n' "$model_path" >&2
    exit 1
fi

git submodule sync -- "$model_path"
# Always check out the exact commit pinned by the DB repository.
git submodule update --init --recursive -- "$model_path"
printf 'Initialized pinned Model submodule at %s\n' "$model_path"
