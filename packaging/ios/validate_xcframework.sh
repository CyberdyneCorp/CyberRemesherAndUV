#!/usr/bin/env bash
# Validate the artifact rather than an in-tree Swift package. The consumer is
# copied beside the staged package by build_xcframework.sh, so it has no path
# back to the repository's capi/include or CMake build directories.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTPUT_DIR="${1:-${ROOT_DIR}/build/ios-sdk}"
"${ROOT_DIR}/packaging/ios/build_xcframework.sh" "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
FRAMEWORK_DIR="${OUTPUT_DIR}/CyberRemesher/CyberRemesherC.xcframework/ios-arm64-simulator"
CONSUMER_DIR="${OUTPUT_DIR}/CyberRemesherConsumer"
BUILD_ARGS=(
    --sdk "${SDK}" \
    -Xswiftc -target -Xswiftc arm64-apple-ios15.0-simulator \
    -Xlinker "-F${FRAMEWORK_DIR}" \
    -Xlinker -syslibroot -Xlinker "${SDK}" \
    --package-path "${CONSUMER_DIR}"
)
swift build "${BUILD_ARGS[@]}"
BIN_DIR="$(swift build "${BUILD_ARGS[@]}" --show-bin-path)"
APP_DIR="${OUTPUT_DIR}/CyberRemesherConsumer.app"
rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}"
cp "${BIN_DIR}/CyberRemesherConsumer" "${APP_DIR}/"
cat > "${APP_DIR}/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>CyberRemesherConsumer</string>
<key>CFBundleIdentifier</key><string>com.cyberdynecorp.CyberRemesherConsumer</string>
<key>CFBundleName</key><string>CyberRemesherConsumer</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSRequiresIPhoneOS</key><true/>
</dict></plist>
EOF

SIMULATOR_UDID="${IOS_SIMULATOR_UDID:-$(xcrun simctl list devices available --json \
    | jq -r '.devices[][] | select(.name | startswith("iPhone")) | .udid' | head -1)}"
test -n "${SIMULATOR_UDID}"
xcrun simctl bootstatus "${SIMULATOR_UDID}" -b
xcrun simctl install "${SIMULATOR_UDID}" "${APP_DIR}"
launch_output="$(xcrun simctl launch --console-pty "${SIMULATOR_UDID}" \
    com.cyberdynecorp.CyberRemesherConsumer 2>&1)"
printf '%s\n' "${launch_output}"
grep -q "CYBER_IOS_CONSUMER_OK" <<<"${launch_output}"
