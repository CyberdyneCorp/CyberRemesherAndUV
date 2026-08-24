# engine-bindings — seam-driven unwrap

Delta for `add-seam-driven-unwrap-binding`.

## ADDED Requirements

### Requirement: Seam-driven unwrap binding
The C ABI and the Python binding SHALL expose an unwrap that takes a seam set
the caller built — by marking edges directly or by committing routed seam paths
— and parameterizes the mesh along it, writing the per-corner UV attribute in
place and reporting through the same result structure the automatic atlas uses.
A cancellable variant SHALL be provided on the same terms as the automatic
atlas, observing cancellation before any UV is written.

The binding SHALL also expose the sew operation over the same seam set.

#### Scenario: Routed seams reach an unwrap
- **WHEN** a caller commits a routed seam path into a seam set and then unwraps along that set through the binding
- **THEN** the mesh SHALL carry UVs cut at the committed path, with no C++ code required

#### Scenario: Null seam set is refused
- **WHEN** the seam-driven unwrap is called with a null seam set
- **THEN** it SHALL return the invalid-argument status and name the argument, rather than silently falling back to automatic seaming

#### Scenario: Cancelled before any write
- **WHEN** a caller cancels a seam-driven unwrap
- **THEN** the call SHALL report cancellation and the mesh SHALL be left exactly as it was
