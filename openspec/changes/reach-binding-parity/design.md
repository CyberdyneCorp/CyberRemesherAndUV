## One check, every binding

`test_swift_abi_parity.py` gained a header -> Swift direction in PR #83. Copying
it into a Python test would have produced two checks that drift the way the
bindings did, so the direction that matters lives once in
`tests/packaging/binding_parity.py` and each binding's test supplies only its
sources and its registration list.

Each binding keeps its own FORWARD check, because "references something that
does not exist" means different things: in Swift a compile error the macOS lane
eventually catches, in ctypes an `AttributeError` at first call, possibly inside
a user's process.

## Three ways a registration list lies

A pending-registration list is only useful if a reader can trust it without
re-checking it. It can be wrong three ways, and the gate now fails on all of
them:

- **unbound** — declared, bound by nothing, not registered. The original gap.
- **stale** — registered, no longer declared. The list has rotted into naming
  something that stopped existing.
- **redundant** — registered AND bound. Added in this change, and it earned its
  place immediately: binding the Swift finishing pipeline left 13 entries still
  marked pending, and the new check named every one.

## Stripping prose before scanning

A symbol named only in a comment or docstring is not bound, and counting it
would let a binding document its way to green. Swift already stripped comments.
The Python check TOKENIZES rather than regexes, because a `#` inside a string
and a docstring naming an entry point are exactly the two cases a regex gets
wrong — and both would make an unbound symbol look bound. A file that fails to
tokenize is scanned raw rather than skipped: a false "bound" is worse than a
false "unbound", and skipping silently is how a gate stops covering something.

## Tests that prove the marshalling, not the return code

A binding that marshals an argument wrongly usually still returns `CYBER_OK`,
so every new test asserts a property of the RESULT. Mutation testing drove three
of the design choices:

- **Off-axis rays.** A ray straight down the axis from `(0,0,3)` and the same
  call with origin and direction transposed both hit `(0,0,1)` at distance 2.
  The first version of the test used exactly that ray and passed a swapped
  binding.
- **Timing that changes the answer.** Every stroke shape is distinguishable by
  geometry alone, so a binding that zeroed the time channel passed every shape
  test. Timing's only effect is a stationary press's confidence — held past the
  engine's threshold versus a quick tap — so the test uses that.
- **Non-default parameters.** The C ABI treats a NULL params pointer as
  "defaults", so an unwrap test using defaults passed a binding that dropped the
  struct entirely. Doubling `textureSize` must double `texelDensity`, which only
  happens if the struct crossed.

## `build_face` returns the final winding, not slot order

The first Python test asserted that `build_face`'s returned ring matched the
slots passed in. It does not, and should not: the engine corrects winding
against the neighbouring face so the two normals agree, which can reverse and
rotate the ring. The test now asserts the weld — the reused vertices are in the
ring, exactly two vertices are new — and the docstrings in both bindings say to
look vertices up by id rather than by slot.
