# iOS memory profile harness

`cyber_ios_memory_profile` runs a generated quad grid through the public C API and logs sampled resident-memory peak during remeshing. It is a measurement harness, not a production memory limit: device, OS, build configuration, and workload must accompany every reported result.

The default grid is `316 x 316` (99,856 input faces, approximately the 100k-face workload). Set `CYBER_PROFILE_GRID=1000` to run the one-million-face workload; values outside `2..1000` fall back to the default. The log line begins `CYBER_PROFILE` and records device model, input/output faces, status, sampled RSS before/peak/after, peak delta, and elapsed time. RSS is sampled every 5 ms, so it is a sampled peak rather than an allocator-instrumented absolute maximum.

## Build

Configure with the normal iOS preset and enable the app target:

```sh
cmake --preset ios -G Xcode -DCYBER_BUILD_APPS=ON \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<TEAM_ID>
xcodebuild -project build/ios/CyberRemesher.xcodeproj \
  -scheme cyber_ios_memory_profile -configuration Release \
  -destination 'generic/platform=iOS' build
```

Install and launch the resulting `cyber_ios_memory_profile.app` on a provisioned physical device. Capture the device console output for each workload and attach those logs to the mobile-profile evidence for the corresponding release or pull request. Do not extrapolate one device's peak to a universal mobile ceiling.
