#!/usr/bin/env bash
# apt.llvm.org publishes LLVM 23 for Ubuntu amd64 and arm64 (not riscv64).
set -euo pipefail
. /etc/os-release
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo gpg --batch --yes --dearmor -o /usr/share/keyrings/ocean-llvm.gpg
printf 'deb [signed-by=/usr/share/keyrings/ocean-llvm.gpg] https://apt.llvm.org/%s/ llvm-toolchain-%s-23 main\n' "$VERSION_CODENAME" "$VERSION_CODENAME" | sudo tee /etc/apt/sources.list.d/ocean-llvm.list
sudo apt-get update
sudo apt-get install -y clang-23 lld-23 llvm-23
mkdir -p "$RUNNER_TEMP/ocean-tools"
for tool in clang wasm-ld ld.lld llvm-ar llvm-ranlib; do
  ln -sf "/usr/bin/$tool-23" "$RUNNER_TEMP/ocean-tools/$tool"
done
echo "$RUNNER_TEMP/ocean-tools" >> "$GITHUB_PATH"
