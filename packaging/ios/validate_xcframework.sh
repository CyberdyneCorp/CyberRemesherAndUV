#!/usr/bin/env bash
# Validate the artifact rather than an in-tree Swift package. The consumer is
# copied beside the staged package by build_xcframework.sh, so it has no path
# back to the repository's capi/include or CMake build directories.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTPUT_DIR="${1:-${ROOT_DIR}/build/ios-sdk}"
"${ROOT_DIR}/packaging/ios/build_xcframework.sh" "${OUTPUT_DIR}"

SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
FRAMEWORK_DIR="${OUTPUT_DIR}/CyberRemesher/CyberRemesherC.xcframework/ios-arm64-simulator"
swift build \
    --sdk "${SDK}" \
    -Xswiftc -target -Xswiftc arm64-apple-ios15.0-simulator \
    -Xlinker "-F${FRAMEWORK_DIR}" \
    -Xlinker -syslibroot -Xlinker "${SDK}" \
    --package-path "${OUTPUT_DIR}/CyberRemesherConsumer"
