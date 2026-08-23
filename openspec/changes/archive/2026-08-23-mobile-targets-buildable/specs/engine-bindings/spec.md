# engine-bindings — the worker-thread cap on the C ABI

Delta for `mobile-targets-buildable`.

## ADDED Requirements

### Requirement: Worker-thread cap on the C ABI
The C ABI SHALL expose the library's worker-thread cap: a setter that takes the
maximum number of workers (0 meaning uncapped) and a getter that reports the
current value. It SHALL be callable at any time from any thread, since a host
adjusts its thread budget between operations rather than once at startup.

A negative value SHALL be rejected as an invalid argument and SHALL leave the
current cap untouched, so a host never ends up with an unbounded engine because
it passed a bad number.

The documentation on the setter SHALL state that the cap changes speed and CPU
load only, never results, because that is what makes it safe for a host to move
mid-session.

#### Scenario: A host bounds the engine and reads the bound back
- **WHEN** a host sets the worker cap through the C ABI and then queries it
- **THEN** the query SHALL return the value that was set, and the active device's reported thread count SHALL reflect it

#### Scenario: A bad value changes nothing
- **WHEN** a negative cap is passed
- **THEN** the call SHALL return an invalid-argument status, set an error message, and leave the previous cap in force
