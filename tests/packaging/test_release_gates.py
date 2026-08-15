#!/usr/bin/env python3
"""Release-lane regressions (build-and-packaging spec).

Nothing here needs a build, a runner or a network: these are the gates that keep
the rest of the suite meaningful, and they lived only in YAML and shell where no
test could see them. Each check names the defect it guards:

* the PyPI lane could only ever emit a `py3-none-any` wheel with three .py files
  and no engine — no package-data, no platform tag, and no step that copied the
  built library into the package;
* the license gate walked only `thirdparty/`, so the MIT AutoRemesher + BSD-3
  Geogram tree compiled into every shipped binary was invisible to it and absent
  from THIRD_PARTY_NOTICES.md;
* the release workflow published binaries without running a single test;
* whether an artifact carries the vendored Geogram field or the native fallback
  was decided by the runner image, and nothing recorded which one it got;
* the "portable" AppImage bundled no libraries, so it died on a clean desktop
  with `libtbb.so.12: cannot open shared object file`;
* the fuzz campaigns left no harness and no corpus, and no lane ran a sanitizer;
* three README claims described behaviour the tree does not have.

Text and file-shape assertions only, so it runs everywhere an interpreter does.
"""

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github/workflows"

FAILURES: "list[str]" = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


def _load_module(rel: str, name: str):
    spec = importlib.util.spec_from_file_location(name, REPO / rel)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# --------------------------------------------------------------------------
# The Python wheel must contain the native library
# --------------------------------------------------------------------------


def test_wheel_ships_the_native_library() -> None:
    pyproject = read("python/cyberremesh/pyproject.toml")
    check("the wheel declares native package-data",
          "[tool.setuptools.package-data]" in pyproject and '"*.so"' in pyproject,
          "setuptools drops the shared library without it")
    check("the wheel is tagged platform-specific",
          "has_ext_modules" in read("python/cyberremesh/setup.py"),
          "a py3-none-any wheel hides the library from auditwheel and cibuildwheel")

    workflow = read(".github/workflows/publish-python.yml")
    check("the wheel lane stages the built library into the package",
          "stage_native_lib.py" in workflow)
    check("the wheel lane's CMake source dir is absolute",
          "cmake -S {project}" in workflow and "cmake -S . " not in workflow,
          "before-all runs in package-dir, which holds no CMakeLists.txt")
    # An import-only command passed on a wheel with no engine in it: importing
    # this package deliberately dlopens nothing.
    check("the wheel lane's test command calls into the engine",
          "cyberremesh.version()" in workflow,
          "import alone loads no library, so the gate was vacuous")


def test_stage_native_lib_finds_and_names_the_library() -> None:
    stage = _load_module("packaging/publish/stage_native_lib.py", "cyber_stage_native_lib")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "build/capi").mkdir(parents=True)
        (root / "pkg").mkdir()
        # The real build tree ships the versioned soname plus symlinks; the wheel
        # must carry a real file under the plain name `_ffi._lib_filenames` tries.
        real = root / "build/capi/libcyber_capi.so.0.5.0"
        real.write_bytes(b"\x7fELF-not-really")
        os.symlink(real, root / "build/capi/libcyber_capi.so")

        found = stage.find_native_lib(root / "build")
        check("the staging script finds the built library", found is not None)
        if found is not None:
            check("it stages under the plain unversioned name", found[1] == "libcyber_capi.so",
                  f"chose {found[1]}")

        rc = stage.main(["stage", str(root / "build"), str(root / "pkg")])
        staged = root / "pkg/libcyber_capi.so"
        check("the staging script copies the library into the package",
              rc == 0 and staged.is_file() and not staged.is_symlink()
              and staged.read_bytes() == real.read_bytes())

        # A build that produced nothing must fail the lane, not ship an empty wheel.
        (root / "empty").mkdir()
        check("an empty build tree fails the staging step",
              stage.main(["stage", str(root / "empty"), str(root / "pkg")]) != 0)


# --------------------------------------------------------------------------
# Licence gate and notices
# --------------------------------------------------------------------------


def test_license_audit_passes() -> None:
    proc = subprocess.run([sys.executable, str(REPO / "tools/license_audit.py")],
                          capture_output=True, text=True, check=False)
    check("tools/license_audit.py passes", proc.returncode == 0,
          (proc.stdout + proc.stderr).strip()[-400:])


def test_license_audit_sees_code_outside_thirdparty() -> None:
    """The gate walked only thirdparty/, so a second vendored tree was invisible."""
    audit = _load_module("tools/license_audit.py", "cyber_license_audit")
    manifest = {"dependencies": [], "vendoredOnDemand": [
        {"name": "AutoRemesher", "license": "MIT", "path": "examples/reference/autoremesher-src",
         "redistributed": True},
    ]}

    # An undeclared subtree the QuadCover build compiles must be an error.
    errors: "list[str]" = []
    audit._check_vendored_on_demand(manifest, errors)
    check("an undeclared vendored subtree fails the audit",
          any("geogram" in e for e in errors), f"errors were {errors}")

    # Something linked into shipped binaries but missing from the notices too.
    errors = []
    audit._check_notices({"dependencies": [
        {"name": "NotInTheNotices", "license": "MIT", "redistributed": True}]}, errors)
    check("a missing third-party notice fails the audit", len(errors) == 1, str(errors))


def test_notices_cover_everything_that_ships() -> None:
    notices = read("THIRD_PARTY_NOTICES.md")
    for name in ("AutoRemesher", "Geogram", "Eigen", "isotropicremesher",
                 "tinyobjloader", "happly", "tinygltf"):
        check(f"THIRD_PARTY_NOTICES names {name}", name in notices)
    check("THIRD_PARTY_NOTICES reproduces the Inria copyright",
          "Copyright (c) 2000-2022 Inria" in notices,
          "BSD-3 requires the notice verbatim in binary redistributions")
    check("THIRD_PARTY_NOTICES reproduces the Dust3D copyright",
          "Dust3D Project" in notices)

    # The notices have to reach the user, not just the repo.
    check("the AppImage ships the notices",
          "THIRD_PARTY_NOTICES.md" in read("packaging/linux/appimage.sh"))
    check("the dmg ships the notices",
          "THIRD_PARTY_NOTICES.md" in read("packaging/macos/build_dmg.sh"))
    check("the Windows zip ships the notices",
          "THIRD_PARTY_NOTICES.md" in read("packaging/windows/build_zip.ps1"))


# --------------------------------------------------------------------------
# Release lane
# --------------------------------------------------------------------------


def test_release_runs_the_suite_before_publishing() -> None:
    release = read(".github/workflows/release.yml")
    check("release.yml has a test job", re.search(r"^  test:$", release, re.M) is not None)
    check("the test job runs ctest", "ctest --preset cpu-headless" in release)
    check("packaging depends on the test job",
          re.search(r"^  package:\n    needs: \[version, test\]$", release, re.M) is not None,
          "a tag on an untested commit went straight to packaging and publish")


def test_release_chooses_its_field_solver() -> None:
    cmake = read("cmake/QuadCoverSolver.cmake")
    check("CYBER_REQUIRE_QUADCOVER exists", "CYBER_REQUIRE_QUADCOVER" in cmake)
    check("it turns the silent fallback into a configure error",
          re.search(r"if\(CYBER_REQUIRE_QUADCOVER\)\s*\n\s*message\(FATAL_ERROR", cmake)
          is not None)
    check("the build records which field solver it contains",
          "cyber_field_solver.txt" in cmake)
    check("the Linux release lane requires the vendored field",
          "-DCYBER_REQUIRE_QUADCOVER=ON" in read("packaging/linux/appimage.sh"))
    check("the release lane installs the field solver's dependencies",
          "libtbb-dev" in read(".github/workflows/release.yml"))
    for script in ("packaging/linux/appimage.sh", "packaging/macos/build_dmg.sh",
                   "packaging/windows/build_zip.ps1"):
        check(f"{script} records the field solver in the artifact",
              "cyber_field_solver.txt" in read(script) and "BUILD_INFO" in read(script))


def test_appimage_bundles_its_runtime_dependencies() -> None:
    script = read("packaging/linux/appimage.sh")
    check("the AppImage bundles the resolved shared libraries",
          "bundle_runtime_libs" in script and "ldd" in script,
          "AppDir/usr/lib was empty, so a clean desktop could not start the CLI")
    check("the AppRun exposes the bundled libraries",
          "LD_LIBRARY_PATH" in script)
    check("packaging fails when a dependency was not bundled",
          "assert_dependencies_bundled" in script)
    check("the packaged smoke test asserts library resolution",
          "not found" in read("packaging/smoke/cli_remesh_smoke.sh"))


# --------------------------------------------------------------------------
# Sanitizer and fuzz lanes
# --------------------------------------------------------------------------


def test_a_sanitizer_lane_exists() -> None:
    path = WORKFLOWS / "hardening.yml"
    check("a hardening workflow exists", path.is_file())
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    check("it runs the suite under ASan/UBSan", "cpu-headless-debug" in text)
    check("it covers the shipped quadrangulator too",
          "CYBER_WITH_QUADCOVER=${{ matrix.quadcover }}" in text,
          "the sanitizer preset otherwise builds a different field solver from the "
          "one that ships")
    check("it runs a thread sanitizer", "-fsanitize=thread" in text)
    check("it is not on every push", "schedule:" in text and "workflow_dispatch:" in text)


def test_a_lane_compiles_every_gpu_backend() -> None:
    """Regression: every lane built cpu-headless, where the GPU options are OFF.

    cuda_backend.cu, opencl_backend.cpp and metal_backend.mm were therefore never
    compiled by this repository, so a GPU fix could be reverted with CI green, and
    the compute-acceleration spec's parity-lane requirement had nothing behind it.
    """
    ci = read(".github/workflows/ci.yml")
    for option in ("CYBER_ENABLE_CUDA", "CYBER_ENABLE_OPENCL", "CYBER_ENABLE_METAL"):
        check(f"a CI lane configures {option}", option in ci,
              "the backend's translation unit is never fed to a compiler")
    check("the CUDA/OpenCL lane installs a toolkit and the OpenCL C++ headers",
          "nvidia-cuda-toolkit" in ci and "opencl-clhpp-headers" in ci)
    check("it builds the backend targets by name",
          "cyber_accel_cuda" in ci and "cyber_accel_opencl" in ci and
          "cyber_accel_metal" in ci)

    # The hardware half. A hosted runner has no device, and a CPU OpenCL ICD does
    # not stand in for one (the backend takes CL_DEVICE_TYPE_GPU devices only), so
    # the real parity lane has to be opt-in on a runner that owns the hardware.
    hardening = read(".github/workflows/hardening.yml")
    check("a hardware parity lane exists", "gpu-parity:" in hardening)
    check("it is opt-in on a runner label rather than queued at a hosted runner",
          "gpu_runner" in hardening and "if: inputs.gpu_runner != ''" in hardening)
    check("it runs the suite against the compiled-in backends",
          "CYBER_ENABLE_CUDA=ON" in hardening and "ctest --test-dir build/gpu-parity" in hardening)


def test_abi_behaviour_changes_are_in_the_changelog() -> None:
    """Regression: `cyber_mesh_edge_faces` changed its return contract on a
    released ABI (entries written, clamped to 2, instead of the true valence) and
    the change was recorded only in header prose — nothing told a binding author
    to move a non-manifold test to the new `cyber_mesh_edge_face_count`."""
    changelog = read("CHANGELOG.md")
    header = read("capi/include/cyber_capi.h")
    unreleased = changelog.split("## 0.2.5")[0]
    check("cyber_mesh_edge_face_count is declared", "cyber_mesh_edge_face_count" in header)
    check("the new entry point is in the changelog",
          "cyber_mesh_edge_face_count" in unreleased)
    check("the changed return contract is in the changelog",
          "cyber_mesh_edge_faces" in unreleased)


def test_fuzz_harnesses_and_corpus_are_checked_in() -> None:
    fuzz = REPO / "tests/fuzz"
    for target in ("fuzz_png.cpp", "fuzz_mesh_io.cpp", "replay_main.cpp",
                   "libfuzzer_entry.cpp", "README.md"):
        check(f"tests/fuzz/{target} exists", (fuzz / target).is_file())
    for corpus in ("png", "mesh"):
        seeds = list((fuzz / "corpus" / corpus).glob("*")) if (fuzz / "corpus" / corpus).is_dir() \
            else []
        check(f"the {corpus} corpus has seeds", len(seeds) >= 8, f"{len(seeds)} seed(s)")
        check(f"the {corpus} seeds stay small",
              all(s.stat().st_size <= 1 << 20 for s in seeds),
              "the replay is bounded; a huge seed defeats that")
    tests_cmake = read("tests/CMakeLists.txt")
    check("the corpus replay is registered as a ctest case",
          "fuzz_corpus_replay" in tests_cmake)
    check("the replay has a timeout",
          re.search(r"fuzz_corpus_replay PROPERTIES TIMEOUT", tests_cmake) is not None,
          "an unbounded replay would hang CI instead of failing it")


# --------------------------------------------------------------------------
# Documentation that has to match the tree
# --------------------------------------------------------------------------


def test_documented_gates_exist() -> None:
    workflows = "\n".join(p.read_text(encoding="utf-8") for p in WORKFLOWS.glob("*.yml"))
    readme = read("README.md")
    claims_tidy = "clang-tidy` are enforced in CI" in readme or \
                  ".clang-tidy` are enforced in CI" in readme
    check("README does not claim an unimplemented clang-tidy gate",
          not claims_tidy or "clang-tidy" in workflows)
    check("README does not claim the GPL reference sources were never copied",
          "never copied" not in readme,
          "the AutoRemesher tree is MIT and IS compiled into the shipped binaries")
    check("the Python README states the real skip code",
          "77" in read("python/cyberremesh/README.md"),
          "it said the API test exits 0; test_api.py returns CTest's SKIP code 77")
    check("the publish README describes the staging step",
          "stage_native_lib" in read("packaging/publish/README.md"))


def main() -> int:
    print(f"release gates (repo: {REPO})")
    test_wheel_ships_the_native_library()
    test_stage_native_lib_finds_and_names_the_library()
    test_license_audit_passes()
    test_license_audit_sees_code_outside_thirdparty()
    test_notices_cover_everything_that_ships()
    test_release_runs_the_suite_before_publishing()
    test_release_chooses_its_field_solver()
    test_appimage_bundles_its_runtime_dependencies()
    test_a_sanitizer_lane_exists()
    test_a_lane_compiles_every_gpu_backend()
    test_abi_behaviour_changes_are_in_the_changelog()
    test_fuzz_harnesses_and_corpus_are_checked_in()
    test_documented_gates_exist()

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("\nrelease gate checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
