## ADDED Requirements

### Requirement: Bake provider surface for an external map consumer

The C ABI SHALL expose a BAKE PROVIDER surface: a capability query, a bake request
carrying the consumer's progress and cancellation callbacks, and a result record
describing the pixels. A consumer SHALL be able to enumerate the producible maps, request
each one, and receive pixels plus their complete metadata while linking nothing beyond
this repository's C ABI. The surface SHALL be reachable from the Python and Swift
bindings on the same terms as the rest of the ABI.

**The capability query.** The library SHALL report the map types THIS BUILD can produce,
and a consumer SHALL be able to read that set before it offers a map to a user. Each
advertised map SHALL carry: its map code, a stable machine name, the number of float
channels a texel holds, the colour space the texels are in, the encoding basis a bake of
it reports under default parameters, and whether a field evaluator alone can produce it.
The advertised names SHALL be the same names the other entry points accept for those
maps, so a consumer joining a named export preset to the advertised set never has to
translate between two spellings.

**The request.** A request SHALL name the EditMesh/Target pair, the map, the bake
parameters, and MAY carry a progress callback, a cancellation callback and one opaque
user pointer shared by both. Every parameter the existing bake entry points validate SHALL
be validated identically here, and the request SHALL honour the same projection cage,
texel ceiling, border padding, progress reporting and cooperative cancellation every
existing map already honours — it SHALL introduce no provider-specific exception to that
shared path.

**Caller-owned buffers with two-call sizing.** The pixels SHALL be written into a buffer
the CALLER owns, and so SHALL the id-to-colour table of an id map. A request that supplies
no pixel buffer SHALL validate the whole request and report the sizes WITHOUT baking, so
a consumer can size its allocation, and learn that a request would be refused, without
paying for a bake. A buffer too small for the result SHALL be refused, naming the capacity
supplied and the capacity required, rather than filled partway.

**Result metadata travels with the pixels.** The result SHALL carry the image's width,
height and channel count, the encoding basis, the up axis, the normal green-channel
convention, the bounding box an object-space position was rescaled over, the factor a
distance was multiplied by, the padding radius applied with the fill rule used and the
texels the band wrote, the id source and id-table size of an id map, and the number of
texels the UV layout covered. Nothing a consumer needs in order to interpret the pixels
SHALL be documented out of band.

**Descriptors state their own size.** Every descriptor of this surface SHALL carry its own
size as its first member, and SHALL be passed one at a time by pointer and never as an
array. The library SHALL read an input member, and write an output member, only when the
caller's stated size covers it. A member appended to such a descriptor in a later release
SHALL therefore keep its documented default for a caller compiled against the earlier
layout, and SHALL NOT be a breaking change. A stated size below the first published
layout SHALL be refused naming both sizes.

**A map this build cannot produce is refused by name.** A request for a map that is not in
the advertised set SHALL FAIL, and the failure SHALL name the requested map and list the
set actually advertised. It SHALL NOT return a neutral, blank or default-valued image: a
consumer that silently receives flat grey where it asked for curvature produces work that
is subtly wrong rather than visibly broken.

**Cancellation hands back nothing.** When cancellation is requested mid-bake the call
SHALL return promptly with a result code distinct from every other failure, and SHALL
leave the caller's pixel and id buffers exactly as they were. No partial map, no partly
grown padding band and no partial id table SHALL ever be handed back.

#### Scenario: A consumer enumerates and requests every advertised map

- **WHEN** a consumer reads the advertised map set and requests each map in it for an
  EditMesh/Target pair
- **THEN** every request SHALL succeed, SHALL fill the consumer's own buffer with the
  channel count the query advertised for that map, and SHALL report the encoding basis,
  up axis, normal green-channel convention and padding record of the map it produced

#### Scenario: The sizing call costs no bake

- **WHEN** a consumer issues a request with no pixel buffer
- **THEN** the call SHALL validate the request and report the width, height, channel count
  and required buffer length, and SHALL NOT cast a ray or write any pixel

#### Scenario: A short buffer is refused, not filled partway

- **WHEN** a consumer issues a request whose pixel buffer is smaller than the reported
  requirement
- **THEN** the call SHALL fail naming both the supplied and the required capacity, and
  SHALL write no pixel into that buffer

#### Scenario: An unproducible map is named, never substituted

- **WHEN** a consumer requests a map that is not in the advertised set
- **THEN** the call SHALL fail, the diagnostic SHALL name the requested map and list the
  advertised set, and no image SHALL be produced

#### Scenario: Cancelling mid-bake leaves the consumer's buffer untouched

- **WHEN** a consumer's cancellation callback reports cancellation while a bake is running
- **THEN** the call SHALL return the cancelled result code promptly and every byte of the
  consumer's pixel buffer SHALL hold what it held before the call

#### Scenario: Progress is reported while the map accumulates

- **WHEN** a consumer attaches a progress callback to a request for a ray-traced map
- **THEN** progress SHALL be reported repeatedly as texels accumulate, not once at the end

#### Scenario: An id map's table arrives with its pixels

- **WHEN** a consumer requests an id map and supplies an id-table buffer
- **THEN** the result SHALL report the id source and the total number of distinct ids, the
  buffer SHALL hold those rows ascending by id, and a colour picked out of the returned
  pixels SHALL resolve to exactly one of them at zero tolerance

#### Scenario: A descriptor from an older caller keeps the documented defaults

- **WHEN** a request states a descriptor size larger than this build's layout, as a caller
  compiled against a later header would
- **THEN** the call SHALL succeed, reading only the members this build's layout covers,
  and a stated size below the first published layout SHALL be refused naming both sizes

#### Scenario: The advertised names are the names the rest of the ABI accepts

- **WHEN** the advertised map names are compared with the map names an export preset
  declares and the headless CLI accepts
- **THEN** the three lists SHALL name the same maps with the same spellings
