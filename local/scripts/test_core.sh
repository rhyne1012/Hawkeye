#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
test_binary="$(mktemp "${TMPDIR:-/tmp}/flight-replay-test.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT
"${CC:-cc}" -std=c11 -D_GNU_SOURCE -D__EMSCRIPTEN__ -I"$repo_dir/src" \
  -I"$repo_dir/lib/c_library_v2" -I"$repo_dir/lib/c_library_v2/common" \
  "$repo_dir/local/tests/test_wasm_seek.c" "$repo_dir/src/wasm/wasm_replay.c" \
  "$repo_dir/src/wasm/ulog_extractor.c" "$repo_dir/src/wasm/ulog_timeline.c" \
  "$repo_dir/src/ulog_replay_apply.c" -lm -o "$test_binary"
"$test_binary" "$@"

"${CC:-cc}" -std=c11 -D_GNU_SOURCE -D__EMSCRIPTEN__ -I"$repo_dir/src" \
  -I"$repo_dir/lib/c_library_v2" -I"$repo_dir/lib/c_library_v2/common" \
  "$repo_dir/tests/test_sparse_replay.c" "$repo_dir/src/wasm/wasm_replay.c" \
  "$repo_dir/src/wasm/ulog_extractor.c" "$repo_dir/src/wasm/ulog_timeline.c" \
  "$repo_dir/src/ulog_replay_apply.c" -lm -o "$test_binary"
(cd "${TMPDIR:-/tmp}" && "$test_binary")
