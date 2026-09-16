## 1. Artifact construction

- [x] 1.1 Add deterministic iOS device and simulator C ABI archive builds using the native CPU profile.
- [x] 1.2 Assemble a versioned XCFramework with public headers and validate its slice layout.

## 2. Consumer surface

- [x] 2.1 Add a binary-backed SwiftPM distribution manifest without repository-relative include/linker paths.
- [x] 2.2 Add a clean consumer fixture that checks ABI compatibility, in-memory remesh, progress and explicit cancellation.

## 3. Automation and documentation

- [x] 3.1 Add an Apple CI gate that builds both slices and the simulator fixture.
- [x] 3.2 Document installation, solver profile, supported architectures and the physical-device evidence boundary.
- [x] 3.3 Validate the artifact, fixture and OpenSpec change.
- [x] 3.4 Run `validate_device_xcframework.sh` with a team-provided development
      profile and record physical-device elapsed time, sampled resident memory,
      pre-start cancellation, and repeated-run thermal evidence. On 2026-09-13
      the staged CPU-only consumer completed four runs on iPad Air 13-inch
      (M3, `iPad15,5`), iPadOS 26.5.2: 16.05–17.82 ms remesh, 7.24–7.29 MB
      sampled RSS peak, 0.042–0.045 ms cancellation, and nominal thermal state
      throughout. The two-triangle smoke fixture is not a general workload
      limit; the account-owned profile is intentionally not stored here.
