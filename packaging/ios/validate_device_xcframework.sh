#!/usr/bin/env bash
# Run the release-shaped XCFramework consumer on a physical iOS device. Signing
# inputs remain caller-owned account state and are never committed to the SDK.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTPUT_DIR="${1:-${ROOT_DIR}/build/ios-sdk}"
: "${IOS_DEVICE_ID:?set IOS_DEVICE_ID to the connected xcrun devicectl device identifier}"
: "${IOS_PROVISIONING_PROFILE:?set IOS_PROVISIONING_PROFILE to a development .mobileprovision}"
: "${IOS_SIGNING_IDENTITY:?set IOS_SIGNING_IDENTITY to an installed Apple Development identity}"
IOS_BUNDLE_ID="${IOS_BUNDLE_ID:-com.cyberdynecorp.CyberRemesherConsumer}"

test -f "${IOS_PROVISIONING_PROFILE}"
"${ROOT_DIR}/packaging/ios/build_xcframework.sh" "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
CONSUMER_DIR="${OUTPUT_DIR}/CyberRemesherConsumer"
FRAMEWORK_DIR="${OUTPUT_DIR}/CyberRemesher/CyberRemesherC.xcframework/ios-arm64"
APP_DIR="${OUTPUT_DIR}/CyberRemesherConsumer-device.app"
PROFILE_PLIST="${OUTPUT_DIR}/profile.plist"
ENTITLEMENTS="${OUTPUT_DIR}/consumer-entitlements.plist"

security cms -D -i "${IOS_PROVISIONING_PROFILE}" > "${PROFILE_PLIST}"
TEAM_ID="$(plutil -extract TeamIdentifier.0 raw -o - "${PROFILE_PLIST}")"
PROFILE_APP_ID="$(plutil -extract Entitlements.application-identifier raw -o - "${PROFILE_PLIST}")"
EXPECTED_APP_ID="${TEAM_ID}.${IOS_BUNDLE_ID}"
case "${PROFILE_APP_ID}" in
    "${EXPECTED_APP_ID}"|"${TEAM_ID}".*) ;;
    *)
        echo "profile application identifier '${PROFILE_APP_ID}' does not permit '${EXPECTED_APP_ID}'" >&2
        exit 2
        ;;
esac

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
BUILD_ARGS=(
    --configuration release
    --sdk "${SDK}"
    -Xswiftc -target -Xswiftc arm64-apple-ios15.0
    -Xlinker "-F${FRAMEWORK_DIR}"
    -Xlinker -syslibroot -Xlinker "${SDK}"
    --package-path "${CONSUMER_DIR}"
)
swift build "${BUILD_ARGS[@]}"
BIN_DIR="$(swift build "${BUILD_ARGS[@]}" --show-bin-path)"

rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}"
cp "${BIN_DIR}/CyberRemesherConsumer" "${APP_DIR}/"
cp "${IOS_PROVISIONING_PROFILE}" "${APP_DIR}/embedded.mobileprovision"
cat > "${APP_DIR}/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>CyberRemesherConsumer</string>
<key>CFBundleIdentifier</key><string>${IOS_BUNDLE_ID}</string>
<key>CFBundleName</key><string>CyberRemesherConsumer</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSRequiresIPhoneOS</key><true/>
</dict></plist>
EOF

plutil -extract Entitlements xml1 -o "${ENTITLEMENTS}" "${PROFILE_PLIST}"
plutil -replace application-identifier -string "${EXPECTED_APP_ID}" "${ENTITLEMENTS}"
plutil -replace com.apple.developer.team-identifier -string "${TEAM_ID}" "${ENTITLEMENTS}"
if plutil -extract 'keychain-access-groups.0' raw -o /dev/null "${ENTITLEMENTS}" 2>/dev/null; then
    plutil -replace 'keychain-access-groups.0' -string "${EXPECTED_APP_ID}" "${ENTITLEMENTS}"
fi
plutil -replace get-task-allow -bool YES "${ENTITLEMENTS}"
codesign --force --sign "${IOS_SIGNING_IDENTITY}" --entitlements "${ENTITLEMENTS}" "${APP_DIR}"
codesign --verify --deep --strict "${APP_DIR}"

start="$(date +%s)"
xcrun devicectl device install app --device "${IOS_DEVICE_ID}" "${APP_DIR}"
launch_output="$(xcrun devicectl device process launch --console --terminate-existing \
    --device "${IOS_DEVICE_ID}" "${IOS_BUNDLE_ID}" 2>&1)"
elapsed="$(( $(date +%s) - start ))"
printf '%s\n' "${launch_output}"
grep -q "CYBER_IOS_CONSUMER_OK" <<<"${launch_output}"
printf 'physical device validation: device=%s elapsed=%ss\n' "${IOS_DEVICE_ID}" "${elapsed}"
