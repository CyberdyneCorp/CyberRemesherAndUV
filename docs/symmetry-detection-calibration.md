# Symmetry detection calibration

The detector is advisory-only. A `detected` result never mutates a mesh and is
not wired to forced symmetry. This record fixes the evidence behind its initial
`0.995` surface-confidence threshold and its ambiguity policy.

## Reproduction

Build the `cpu-headless` preset, then run:

```sh
PYTHONPATH=python/cyberremesh:tools/bench \
CYBER_CAPI_LIB="$PWD/build/cpu-headless/capi/libcyber_capi.dylib" python3 - <<'PY'
from pathlib import Path
import tempfile
import corpus
from cyberremesh import Mesh

with tempfile.TemporaryDirectory() as directory:
    for item in corpus.acceptance_meshes(Path(directory)):
        try:
            with Mesh.load(str(item["path"])) as mesh:
                report = mesh.detect_symmetry()
                print(item["name"], report.detected, report.axis,
                      report.confidence, report.unmatched_surface_points)
        except Exception as error:
            print(item["name"], type(error).__name__)
PY
```

## Recorded run

On the CPU-headless build at commit `9502494`, the symmetric primitive fixtures
`sphere`, `box_sharp`, `torus`, and `cylinder` each produced confidence above
`0.9997`, zero unmatched surface samples, and `detected = false` because they
have multiple equally strong planes. This is intentional: ambiguous geometry
must not select an axis. The open and multi-component fixtures also remained
non-detected; the `large_coordinates` asymmetric fixture had confidence
`0.628788` and 147 unmatched samples out of 396. The malformed
`invalid_index` fixture was rejected by import rather than analysed.

The focused C++ calibration fixtures add one unambiguous translated/rescaled
symmetric surface, one rotated symmetric surface, unequal tessellation, a
deliberate asymmetric accessory, and a partial scan. They are run by
`cyber_tests --test-case='*symmetry detection*'`.

## Scope

These measurements calibrate **detection and ambiguity only**. They do not
authorize automatic application. A future opt-in application path needs a
separate corpus result proving that its forced-axis choice is valid for every
accepted report.
