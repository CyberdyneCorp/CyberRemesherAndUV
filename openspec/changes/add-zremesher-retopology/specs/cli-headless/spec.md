# cli-headless — ZRemesher strategy

## ADDED Requirements

### Requirement: ZRemesher quad method on the CLI

The headless binary SHALL accept `--quad-method zremesher` alongside the
existing methods, together with flags for quality mode, adaptive sizing,
symmetry, guide mode and layout export. Unknown values SHALL be rejected with a
usage error rather than silently falling back, and the machine-readable report
SHALL name the method, the quality mode, the selected candidate and the layout
statistics.

#### Scenario: Method is selectable and reported

- **WHEN** the CLI is run with `--quad-method zremesher`
- **THEN** the run SHALL use the ZRemesher path and the JSON report SHALL name
  the method, quality mode, selected candidate and layout node/arc/patch counts

#### Scenario: Layout export from the CLI

- **WHEN** layout export is requested on the command line
- **THEN** the CLI SHALL write the layout report and polyline mesh to the
  requested paths and exit successfully
