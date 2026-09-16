## Contours: rings to a tube

RetopoFlow's Contours is the tool artists reach for on arms, legs, horns and
tentacles: draw a few strokes ACROSS a tubular region, get a clean quad tube.
Nothing in this repo does it. `extrudeCylinder` lofts ONE band from a ring the
caller already has, which is the last 10% of the problem.

### Why a plane cut, not a projected stroke

`drawStripPath` builds rails around the stroke itself, which is right for
PolyStrips — the stroke IS the ribbon's spine. Contours is the opposite: the
stroke is a *sampling gesture*, and the ring it names wraps all the way around
the form, including the far side the artist never drew on and cannot see.

So the stroke is used to derive a cutting plane, and the ring comes from the
Target's intersection with that plane. That is what makes the far side appear,
and it is why a ring is a closed loop even though the stroke is an open arc.

The plane is the least-squares fit through the stroke's samples. A stroke drawn
across a tube is close to planar by construction, and fitting rather than
constructing the plane from (say) the stroke's endpoints and the view direction
means a wobbly hand-drawn arc still lands on the section the artist meant.
Degenerate cases — a stroke that is effectively a point, or perfectly straight
so the fit is rank-deficient — are rejected by name rather than producing an
arbitrary plane.

### Getting the ring

Cut the Target with the plane: classify each triangle's vertices by signed
distance, emit a segment for each straddling triangle, chain segments into
polylines through shared endpoints. A closed tube gives a closed polyline.

A plane through a limb usually cuts the body somewhere else too, so the cut
returns SEVERAL components. Pick the one whose centroid is nearest the stroke's
own centroid — the artist pointed at it. Components are ranked, not merged;
merging would loft a tube between an arm and a thigh.

### Why span reconciliation is the hard part

Ring i and ring i+1 come from different plane cuts and have no reason to carry
the same vertex count, and a loft needs them equal. Every ring is therefore
resampled to the SAME `spans` count by arc length, which also makes the quads
even, which is what the tool is for.

Two consistency problems remain, and both produce output that looks plausible
and is wrong:

- **Seam drift.** Each ring's resampling has an arbitrary start. Ring i+1's
  start is chosen as the sample nearest ring i's start, so the tube's seam runs
  along it instead of spiralling.
- **Winding flip.** A cut can come back in either orientation. Ring i+1 is
  reversed when its traversal direction opposes ring i's, measured by the dot
  of their ring normals. Without this, one band in the middle of a tube inverts
  and the normals fight.

Both are checked by tests that assert a property of the OUTPUT (no spiral, one
consistent orientation), not by asserting the intermediate numbers.

### Ordering

Rings are lofted in the order the caller supplied them, which is the order the
artist drew them. Sorting by position along a fitted axis was considered and
rejected: it guesses at intent, and an artist who draws a ring out of order to
fix a gap gets a tube that reorders itself under them.

## Bridge

`bridgeLoops` is implemented in C++, required by `manual-retopology`, and
callable by nobody. The stroke grammar already recognises the gesture — "line
between two boundary loops with equal vertex count" — so the recogniser can
name an action the ABI cannot perform.

Pinning was audited alongside it and is NOT a gap, which is worth recording
because the first pass of this audit called it one. `PinSet` is C++-internal,
but the CAPABILITY is exposed in exactly the host-supplied form this design
would have argued for: `cyber_retopo_relax`,
`cyber_retopo_selection_relax` and `cyber_retopo_selection_transform_pinned`
all take a `pinned` array, and Swift already passes it. The engine deliberately
holds no pin state — that would be a second source of truth across edits that
reassign element ids, which `cyber_mesh_topology_generation` exists to let a
host detect.

## Binding parity, in both directions

`test_swift_abi_parity.py` proves Swift references nothing the header lacks. It
cannot prove the header exposes nothing Swift lacks, and that is the failure
that actually happened: 124 unbound entry points, none of them noticed.

The gate gains the second direction plus a checked-in list of deliberate
omissions. An unbound entry point that is not on the list fails CI. Adding one
to the list is a visible diff that says "we chose not to bind this", which is
what the `engine-bindings` spec has always asked for and never had.
