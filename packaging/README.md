# Packaging & Release

Build-and-packaging lane for CyberRemesher (implements OpenSpec change
`bootstrap-v1-platform`, task 14 / `build-and-packaging` capability). This
directory holds the release/packaging scripts and GitHub Actions workflows that
turn a tagged commit into signed, versioned, installable artifacts and a
published GitHub Release.

> **Status: best-effort / UNVERIFIED.** Every script here targets a platform
> toolchain (Xcode, MSVC/WiX, Android SDK/NDK, `appimagetool`, simulators) that
> is **not** available in the headless Linux CI used for the core build. The
> scripts are written to run for real on the matching GitHub-hosted runners but
> cannot be exercised in this repo's default CI job. Each file carries an
> `UNVERIFIED:` header saying what it needs. The core `cmake --preset
> cpu-headless` build these wrap is the same one CI already validates.

## Single version source

The semantic version has exactly one origin: the `project(CyberRemesher VERSION
x.y.z ...)` field in the repository-root `CMakeLists.txt`. It is:

- compiled into every binary as `CYBER_VERSION_STRING` (see
  `src/core/CMakeLists.txt`) and surfaced by `cyberremesh --version` and the
  About panel;
- read by `packaging/version.sh` (shell lanes) and `packaging/version.cmake`
  (CMake lanes) to build artifact filenames;
- checked against the pushed git tag by `version.sh --verify-tag` so a release
  can never ship binaries whose `--version` disagrees with the tag or filenames.

```console
$ packaging/version.sh                    # 0.1.0
$ packaging/version.sh --name macos dmg   # cyberremesher... -> cyberremesh-0.1.0-macos.dmg
$ packaging/version.sh --verify-tag v0.1.0
```

Artifact naming (all carry the version — AutoRemesher only uploaded unversioned
CI artifacts; this lane fixes that):

| Platform | Artifact |
|----------|----------|
| macOS    | `cyberremesh-<v>-macos.dmg` (signed + notarized) |
| Windows  | `cyberremesh-<v>-windows-x64.zip`, `cyberremesh-<v>-windows-x64.msi` |
| Linux    | `cyberremesh-<v>-linux-x86_64.AppImage` |
| iOS      | `cyberremesh-<v>-ios.ipa` |
| Android  | `cyberremesh-<v>-android.aab` |

## Layout

```
packaging/
  version.sh            single-source version + artifact-name helpers (shell)
  version.cmake         same, for CMake-driven packaging steps
  macos/                build_dmg.sh, notarize.sh, Info.plist.in
  windows/              build_zip.ps1, build_installer.ps1, installer.wxs
  linux/                appimage.sh, cyberremesher.desktop
  ios/                  build_archive.sh, ExportOptions.plist
  android/              build_aab.sh
  smoke/                cli_remesh_smoke.sh, launch_screenshot.sh,
                        mobile_boot.sh, assets/reference_cube.obj
```

## Workflows

- `.github/workflows/release.yml` — triggered on `v*` tag pushes. A `version`
  job resolves and verifies the version, a platform matrix builds + packages
  each artifact under its versioned name and uploads it, then a `release` job
  downloads all artifacts and publishes a GitHub Release with them attached.
- `.github/workflows/package-smoke.yml` — runs the packaged-form smoke tests:
  the desktop lanes download their package, extract/mount it, run
  `smoke/cli_remesh_smoke.sh` against the packaged CLI plus a launch-screenshot
  placeholder; the mobile lanes boot the artifact in a simulator/emulator via
  `smoke/mobile_boot.sh`. A smoke failure blocks publication.

These are additive to `.github/workflows/ci.yml` (unit/format/license/openspec
gates), which is unchanged.

## Required secrets (release signing)

Configured as GitHub Actions secrets; scripts degrade to unsigned dev artifacts
when they are absent so the pipeline is runnable without credentials.

| Secret | Used by |
|--------|---------|
| `MACOS_CODESIGN_IDENTITY`, `MACOS_NOTARY_KEYCHAIN_PROFILE` | macOS DMG sign/notarize |
| `WINDOWS_CERT_PFX`, `WINDOWS_CERT_PASSWORD` | Windows Authenticode signing |
| `IOS_TEAM_ID` (+ provisioning profile / distribution cert) | iOS archive/export |
| `ANDROID_KEYSTORE_PATH`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD` | Android AAB signing |

## Local dry run

On a matching host (macOS for DMG/iOS, Windows for zip/MSI, Linux for AppImage)
the scripts run directly, e.g.:

```console
$ packaging/linux/appimage.sh                 # -> dist/cyberremesh-<v>-linux-x86_64.AppImage
$ packaging/smoke/cli_remesh_smoke.sh ./build/cpu-headless/apps/cli/cyberremesh
```
