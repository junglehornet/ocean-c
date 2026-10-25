#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$(mktemp -d /tmp/ocean-coverage.XXXXXX)}"
mkdir -p -- "$build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"
profile_dir="$(mktemp -d "$build_dir/profiles.XXXXXX")"
cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_C_COMPILER=clang -DBUILD_TESTING=ON \
    '-DCMAKE_C_FLAGS=-fprofile-instr-generate -fcoverage-mapping -DOCEAN_TEST_COVERAGE'
cmake --build "$build_dir" -j 2
LLVM_PROFILE_FILE="$profile_dir/%p-%m.profraw" OCEAN_PROFILE_DIR="$profile_dir" \
    ctest --test-dir "$build_dir" -LE pending --output-on-failure
llvm-profdata merge -sparse "$profile_dir"/*.profraw -o "$build_dir/coverage.profdata"
objects=()
for target in function_parser_test parser_features_test parser_edges_test scanner_test token_test dynarray_test ast_test hashmap_test; do
    objects+=(-object "$build_dir/$target")
done
llvm-cov report "$build_dir/ocean-c" "${objects[@]}" \
    -instr-profile="$build_dir/coverage.profdata" -ignore-filename-regex='/(tests|c-hashmap)/'
llvm-cov show "$build_dir/ocean-c" "${objects[@]}" \
    -instr-profile="$build_dir/coverage.profdata" -ignore-filename-regex='/(tests|c-hashmap)/' \
    -format=html -output-dir="$build_dir/coverage-html"
printf '\nCoverage report: %s/coverage-html/index.html\n' "$build_dir"
