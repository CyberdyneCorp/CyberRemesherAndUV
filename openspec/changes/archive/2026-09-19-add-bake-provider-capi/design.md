## Context

`cyber_bake` is a bake *call*. What CyberTexel needs is a bake *provider*: something it can
interrogate, drive with its own progress bar and cancel button, and read metadata off
without a second set of lookups. Everything underneath already exists —
`cyber::bake::bake()` has taken a `ProgressSink*` and a `CancelToken*` since the bake stage
was written, and #87/#88/#90 filled `BakeResult::encoding` and `BakeResult::padding` for
every map. None of it is reachable through the C ABI's bake entry point.

The constraints this design has to satisfy are not new either. `capi/` has a settled
vocabulary — `cyber_*`, opaque handles, integer result codes, two-call sizing on every
variable-length read — and an ABI contract stated in capitals at the top of the header:
minors are additive, appending a member to an existing struct is MAJOR because callers
stride arrays by `sizeof`, and appending an *enumerator* is MAJOR because reading an
out-of-range value through an enum type is UB on the caller's side.

## Goals / Non-Goals

**Goals**

- A consumer enumerates the producible maps, requests each one, and receives pixels plus
  the complete metadata, linking nothing but this repository's C ABI.
- Progress and cancellation reach the consumer, and cancellation hands back nothing.
- A map this build cannot produce is refused by name, never substituted.
- The surface can grow without a major bump.

**Non-Goals**

- A job handle the consumer polls. Considered and rejected below.
- RGBA channel packing (ORM/MER). Epic #86 puts that in CyberTexel's `texture-export`.
- Component-link selection. `surface-baking`'s "Component links and selective baking"
  requirement has **no implementation in `src/bake/`** — `bake()` takes no component-link
  input at all, from any entry point. The provider therefore takes the identical code path
  every existing map takes, and this change asserts nothing about component links.
- New map types. This change exposes the set that already exists.

## Decisions

### 1. Synchronous with a progress callback, not a polled handle

The issue names both options and says CyberTexel's `execution-backends` accepts either.
Synchronous wins on three counts:

- It is the shape `cyber_bake_field` and `cyber_export_bundle_write` already have, so a
  host that drives one drives all three the same way.
- A handle needs a lifetime, a free function, a thread, a completion state and a rule for
  what happens when the consumer drops it mid-bake. That is a scheduler, and the consumer
  already has one — CyberTexel's `execution-backends` exists precisely to own it.
- Cancellation is *already* cooperative and callback-driven underneath (`CancelToken` with
  an installed poll). A polled handle would wrap a cooperative cancel in an asynchronous
  one, so the latency would be unchanged and the mechanism doubled.

The cost is that the call blocks its thread. That is the consumer's thread to spend, and
the progress callback fires from it, which is documented.

### 2. Caller-owned pixel buffer with two-call sizing, not a new opaque image handle

`cyber_bake` returns a `CyberImage*`; reading it out costs `cyber_image_copy_pixels` into
the consumer's memory anyway. A texture-painting host has already allocated the texture it
wants the map in. Writing straight into it removes one full-resolution copy and one
lifetime the consumer has to get right.

Two-call sizing is the convention `cyber_mesh_copy_positions`, `cyber_seam_set_edges` and
`cyber_image_copy_pixels` already use. Here the sizing call is special in a useful way:
`pixels == NULL` means *validate and report the sizes*, and because the size is
`width * height * channels` and the channel count comes from the catalogue, the sizing
call needs no rays at all. It is a cheap dry run that answers "would this request be
accepted, and how big is the answer" — which is exactly what a host wants before it
allocates.

A short buffer is `CYBER_ERR_INVALID_ARG` naming the capacity and the requirement, not a
truncated map. Truncation here is silent corruption: a consumer would upload the top of
its texture and garbage below.

The id-to-colour table is the same shape — a caller-owned `CyberIdColor*` and a reported
total — because it is variable-length for exactly the same reason.

### 3. `structSize` as the first member of every descriptor

The header's ABI block says appending to a struct is MAJOR, and gives the reason: in-params
travel as arrays the caller strides by `sizeof`, and out-params are written into the
caller's buffer. Both hazards are about *not knowing how big the other side thinks the
struct is*.

The three provider descriptors state their own size and are passed **one at a time by
pointer, never as an array**. The library reads a member only when
`structSize >= offsetof(member) + sizeof(member)`, and writes an out-member under the same
rule. So a v1 consumer calling a v2 library gets v1 behaviour with the appended members at
their documented defaults, and a v2 consumer calling a v1 library is refused by
`cyber_abi_check` before it starts. This makes appending to these three structs — and only
these three — additive.

The rule is enforced from the first release rather than retrofitted, because a mechanism
that has never rejected anything has never been tested. A descriptor whose `structSize` is
below the v1 layout is refused naming both numbers, and a descriptor whose `structSize` is
*larger* is accepted with the surplus ignored — that is the v1-library-serving-a-v2-caller
case, and it has a test.

### 4. One catalogue in `cyber::bake`, three readers

`channelsFor()` and `fieldSupports()` were private switches in `bake.cpp`; the CLI had the
map names in its help text and in its parser; the C ABI had them in `toBakeMap`. Four
places, and the capability query would have been a fifth — the one that a consumer *trusts*.

So the catalogue becomes a single `constexpr` table in `cyber::bake`, and
`channelsFor` / `fieldSupports` read it. The C ABI's query walks it; the CLI's `--bake`
parser and its new `--list-bake-maps` walk it. A map added later is added once.

The names are `mesh-io`'s preset vocabulary (`normal`, `ao`, `object-position`,
`material-id`, …) rather than a new spelling, because a consumer that reads a preset and a
consumer that drives the provider must be able to join the two lists.

### 5. The catalogue's `encodingBasis` is the basis under default parameters

`BakeMap::BentNormal` reports `TangentNormal` or `ObjectNormal` depending on
`BakeParams::bentNormalSpace`, so a static table cannot state one basis for it and be
right. The catalogue's field is documented as the basis under **default parameters**, and
the authority is the per-request result descriptor, which carries the basis the bake
actually recorded. Stating it as authoritative would have been the subtle kind of wrong:
correct for twelve maps out of thirteen.

### 6. The field evaluator narrows the advertised set, which is where the refusal gets teeth

Every one of the thirteen maps is producible in every build of this repository — there is
no build option that removes one. A refusal path that only an out-of-range integer can
reach is a refusal path nobody has really tested.

But `pipeline-bridge` already has a genuine case: with a `CyberFieldEvaluator` and no
Target mesh, only `normal`, `ao`, `curvature` and `cavity` can be produced — the other nine
describe the Target *mesh* (a height above the low-poly, a hit point, a vertex colour, an
id column) and have no field counterpart. So the provider accepts the same evaluator the
bridge defines, and when one is attached without a Target the producible set is those four.
Asking for `thickness` there fails naming `thickness` and listing the four. That is the
acceptance bullet, reached by a real input rather than by a cast.

`fieldCapable` in the catalogue is what lets a consumer see that narrowing *before* it
asks, which is the whole point of a capability query.

### 7. `normalGreenPlusY` is reported, and is always 1

`mesh-io`'s presets carry a green-channel convention because target apps disagree
(+Y for OpenGL/Blender/Unity/glTF, -Y for DirectX/Unreal). The bake itself has exactly one
convention: it writes +Y. Reporting the field anyway, always 1, is deliberate — the
alternative is that a consumer reads the convention off a preset it may not have, or
assumes. Flipping the channel is an output-side transform that belongs to whoever writes
the file, and `mesh-io` already owns it; the provider states what it produced.

### 8. Refusals name the map and the set, in the error string

`cyber_last_error()` is the ABI's only channel for a message, and `CYBER_ERR_INVALID_ARG`
is the only status that fits. So the *contract* is the message content: it names the map
(by name when the code is known, as `map code N` when it is not) and lists the advertised
set. A consumer can put that string in front of a user unchanged, which is what "reported
by name rather than substituted" means in practice.

## Risks / Trade-offs

- **The descriptor-size mechanism is a second ABI discipline in one header.** Two rules
  for two families of structs is a thing a reader can get wrong. Mitigated by stating it
  in the header above the descriptors, in capitals, next to the reason it is safe here and
  not elsewhere (never an array).
- **A blocking call inside a host's UI thread.** Documented, and the progress callback
  gives a host the hook it needs to keep its own loop alive — but a host that ignores the
  advice freezes. The alternative (a job handle) moves that hazard rather than removing it.
- **The catalogue is `constexpr` and therefore compile-time.** A build that *could* lose a
  map (a future `-DCYBER_BUILD_*` that drops one) would need the table to become runtime.
  The query is a function returning a count and a descriptor, not an exported array, so
  that change is internal.
- **Two entry points now bake.** `cyber_bake` stays exactly as it is — removing it would be
  a major bump and it is what the existing bindings and the bundle writer use. The
  duplication is one adapter function; both call `cyber::bake::bake()`.
