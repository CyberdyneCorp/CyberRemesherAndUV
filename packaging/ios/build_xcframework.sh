#!/usr/bin/env bash
# Build the native CPU C ABI as a self-contained iOS XCFramework and stage the
# matching SwiftPM package.  The output is deliberately local: a release job
# zips it and publishes its checksum, while this script makes the exact inputs
# and slice checks reproducible before release hosting exists.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTPUT_DIR="${1:-${ROOT_DIR}/build/ios-sdk}"
DEVICE_BUILD="${OUTPUT_DIR}/device-build"
SIMULATOR_BUILD="${OUTPUT_DIR}/simulator-build"
FRAMEWORK_DIR="${OUTPUT_DIR}/CyberRemesherC.xcframework"
PACKAGE_DIR="${OUTPUT_DIR}/CyberRemesher"
CONSUMER_DIR="${OUTPUT_DIR}/CyberRemesherConsumer"
HEADER_DIR="${OUTPUT_DIR}/headers"

build_slice() {
    local build_dir="$1"
    local sdk="$2"
    cmake --preset ios -S "${ROOT_DIR}" -B "${build_dir}" \
        -DCMAKE_OSX_SYSROOT="${sdk}" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
        -DCYBER_ENABLE_METAL=OFF \
        -DCYBER_BUILD_RENDER=OFF \
        -DCYBER_BUILD_CAPI_SHARED=OFF
    cmake --build "${build_dir}" --target cyber_capi -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
}

combine_archives() {
    local build_dir="$1"
    local output_archive="$2"
    local archives=()
    while IFS= read -r archive; do
        archives+=("${archive}")
    done < <(find "${build_dir}" -type f -name '*.a' -print | LC_ALL=C sort)
    test "${#archives[@]}" -gt 0
    xcrun libtool -static -o "${output_archive}" "${archives[@]}"
}

make_framework() {
    local archive="$1"
    local framework="$2"
    mkdir -p "${framework}/Headers" "${framework}/Modules"
    cp "${archive}" "${framework}/CyberRemesherC"
    cp "${HEADER_DIR}/cyber_capi.h" "${framework}/Headers/"
    cat > "${framework}/Modules/module.modulemap" <<'EOF'
framework module CyberRemesherC {
  header "cyber_capi.h"
  export *
}
EOF
    cat > "${framework}/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.cyberdynecorp.CyberRemesherC</string>
<key>CFBundleName</key><string>CyberRemesherC</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>0.8.0</string>
<key>CFBundleVersion</key><string>0.8.0</string>
</dict></plist>
EOF
}

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}" "${HEADER_DIR}"

build_slice "${DEVICE_BUILD}" iphoneos
build_slice "${SIMULATOR_BUILD}" iphonesimulator
mkdir -p "${OUTPUT_DIR}/device" "${OUTPUT_DIR}/simulator"
combine_archives "${DEVICE_BUILD}" "${OUTPUT_DIR}/device/libCyberRemesherC.a"
combine_archives "${SIMULATOR_BUILD}" "${OUTPUT_DIR}/simulator/libCyberRemesherC.a"

cp "${ROOT_DIR}/capi/include/cyber_capi.h" "${HEADER_DIR}/"
make_framework "${OUTPUT_DIR}/device/libCyberRemesherC.a" \
    "${OUTPUT_DIR}/device/CyberRemesherC.framework"
make_framework "${OUTPUT_DIR}/simulator/libCyberRemesherC.a" \
    "${OUTPUT_DIR}/simulator/CyberRemesherC.framework"

xcodebuild -create-xcframework \
    -framework "${OUTPUT_DIR}/device/CyberRemesherC.framework" \
    -framework "${OUTPUT_DIR}/simulator/CyberRemesherC.framework" \
    -output "${FRAMEWORK_DIR}"

test -f "${FRAMEWORK_DIR}/Info.plist"
plutil -extract AvailableLibraries xml1 -o - "${FRAMEWORK_DIR}/Info.plist" | grep -q '<key>SupportedPlatform</key>'
for framework in "${FRAMEWORK_DIR}"/*/CyberRemesherC.framework; do
    test -f "${framework}/CyberRemesherC"
    test -f "${framework}/Headers/cyber_capi.h"
    test -f "${framework}/Modules/module.modulemap"
done

mkdir -p "${PACKAGE_DIR}/Sources"
cp -R "${ROOT_DIR}/swift/Sources/CyberRemesher" "${PACKAGE_DIR}/Sources/"
cp -R "${FRAMEWORK_DIR}" "${PACKAGE_DIR}/"
mkdir -p "${PACKAGE_DIR}/Sources/CCyberRemesher"
cat > "${PACKAGE_DIR}/Sources/CCyberRemesher/module.modulemap" <<'EOF'
module CCyberRemesher [system] {
  header "../../CyberRemesherC.xcframework/ios-arm64-simulator/CyberRemesherC.framework/Headers/cyber_capi.h"
  export *
}
EOF
sed 's/@VERSION@/0.8.0/g' "${ROOT_DIR}/packaging/ios/Package.swift.in" > "${PACKAGE_DIR}/Package.swift"
cp -R "${ROOT_DIR}/packaging/ios/consumer" "${CONSUMER_DIR}"

echo "Created ${FRAMEWORK_DIR}, ${PACKAGE_DIR} and ${CONSUMER_DIR}"
