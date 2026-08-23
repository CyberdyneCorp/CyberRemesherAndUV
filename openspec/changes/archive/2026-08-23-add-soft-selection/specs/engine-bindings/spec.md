# engine-bindings — soft selection reachability

Delta for `add-soft-selection`.

## ADDED Requirements

### Requirement: Soft selection is reachable from every binding
The C ABI SHALL expose gradient selection (line, sphere, painted), the
selection operations (clear, invert, expand, contract, smooth, save, load),
and weighted transform/relax; the Python and Swift bindings SHALL reach the
same surface under the existing binding-parity rules.

#### Scenario: Parity across bindings
- **WHEN** the binding-parity check runs
- **THEN** every soft-selection capability reachable from Python SHALL be reachable from the C ABI and Swift package
