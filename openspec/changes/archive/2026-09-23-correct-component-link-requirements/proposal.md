## Why

`surface-baking` has promised since its first version that each EditMesh component bakes only from its linked Target components, and that a component flagged "bake alone" bakes in isolation. Nothing implements that. `cyber::bakecage::ComponentLinks` is a real, tested model (link, unlink, bake-alone, nearest-component resolution), but `bake()` takes no link input, and the C ABI, the bindings and the app do not expose it. The curvature requirement repeats the claim. Every mesh-map PR in epic #86 found this independently and declined to assert it.

The living specs must describe what 0.10.0 ships. This repository's rule is that a requirement which is not built is corrected rather than left standing.

## What Changes

- **"Component links and selective baking" is removed** and replaced by **"Component link model"**, which states what exists: the link model and its resolution rules, with scenarios that its existing tests prove. It states plainly that the bake does not yet apply links, and that no map may claim link-restricted sampling until it does. Applying links is carried to **#103**.
- **"Curvature and cavity maps"** drops "component links" from the list of shared rules it claims to follow.

No code changes. No behaviour changes.

## Capabilities

### Modified Capabilities
- `surface-baking`: two requirements corrected to match the implementation.

## Impact

Documentation only. It removes a false guarantee from the release; #103 tracks the work that would make the original one true.
