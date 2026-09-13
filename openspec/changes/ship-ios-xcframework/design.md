## Context

The source SwiftPM package exposes the C ABI through a system-library target
and expects a host to provide local header and linker paths. CMake can build the
C ABI for iOS, but it does not emit a distributable XCFramework or prove clean
consumer integration. See proposal.md for motivation and the associated specs.

## Goals / Non-Goals

**Goals:**

- Build static, self-contained C ABI archives for iOS device and simulator and
  assemble them with the public header into an XCFramework.
- Keep the public Swift wrapper as source while replacing only its C ABI target
  with a binary target in the distribution package.
- Test a separate fixture package rather than reusing repository-relative paths.

**Non-Goals:**

- Ship an App Store application, signing profile, or TestFlight upload.
- Claim physical-device performance, memory, thermal or cancellation latency
  without a named hardware run.
- Include the optional native+Geogram solver in the initial iOS artifact; the
  initial profile is CPU/native and must declare that fact.

## Decisions

- Use a static `CyberRemesherC.xcframework`. It embeds the C ABI and its static
  implementation, avoiding an app-bundle dylib and rpath contract for hosts.
- Keep `swift/` as the source-development package and add a distribution
  package/fixture that depends on the binary XCFramework. This preserves local
  contributor ergonomics while proving the artifact path consumers use.
- Build device arm64 and simulator arm64 archives with CMake/Xcode, then use
  `xcodebuild -create-xcframework`; validate the generated Info.plist and
  headers before compiling the consumer.
- Make simulator compilation and the fixture test a required Apple CI gate.
  Hardware profiling is tracked as evidence, not fabricated from CI.

## Risks / Trade-offs

- [Cross-built archive has an unbundled system dependency] → use the native CPU
  solver profile and inspect linked artifacts before packaging.
- [SwiftPM remote binary checksum distribution needs release hosting] → build
  and validate a local-path binary target in CI now; release automation can
  attach the zip/checksum without changing the consumer API.
- [iOS CMake support differs from simulator SDK settings] → validate both
  configured slices through the same script on macOS CI.

## Migration Plan

The source package remains available for contributors. Consumers migrate by
selecting the documented binary-backed package; no C ABI or Swift API source
change is required. Rollback consists of removing the new binary distribution
while retaining source-package support.
