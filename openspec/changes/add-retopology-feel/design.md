## Auto Relax is a region, not a mode

The spec describes app behaviour: "When Auto Relax is enabled, every
topology-modifying operation SHALL be followed by an automatic local relax". The
library's job is to make that possible and cheap, and there were two ways:

1. An engine-held mode flag that every build op honours internally.
2. An explicit region relax the host calls after an op, seeded from the vertices
   that op produced.

(1) was rejected. Every existing build entry point would start moving more than
its documentation says, silently, depending on hidden state. It is also the same
shape as engine-held pins, which this ABI already refuses on the grounds that
engine-held state becomes a second source of truth across edits that reassign
element ids. The host already knows which vertices an edit created — `build_face`
returns its ring, `contours` reports its vertices — so (2) costs it one call.

## Why topological, not spatial

`cyber_retopo_relax` already takes a spatial brush (`center`, `radius`). It is
the wrong shape for relaxing after an edit. A strip drawn down a thin limb is
long and narrow; a sphere large enough to cover it also reaches through to the
far side of the limb. The region is therefore every vertex within `rings` edge
hops of a seed — the same reasoning that already made `cyber_retopo_move`
vertex-seeded.

A test pins this directly: two parallel sheets 0.05 apart, spatially adjacent
and topologically unconnected, both disturbed. A relax seeded on the upper sheet
must leave the lower one bit-identical.

## Built on the existing kernel

`relaxSweep` already takes an `extraWeight(v)` callback, and a weight `<= 0`
leaves a vertex neither moved nor re-snapped. The region relax is a
breadth-first ring distance and a falloff passed through that callback — no
second relax kernel. Weight falls off smoothly with ring distance (full at the
seeds, zero one ring past `rings`) so the boundary between relaxed and untouched
topology shows no step in quad size.

## Loop slide: the hard part is choosing a side

Moving a vertex a fraction along an edge is trivial. Choosing, for every vertex
of the loop, a neighbour on the SAME side is not: pick independently and a slide
on a closed ring twists half the loop one way and half the other.

Sides come from orientation. Walking the loop a->b, the face whose winding
traverses a->b lies on the same side of the loop throughout a consistently
oriented mesh. In that quad [a, b, c, d] the rails are b-c and a-d. So `t > 0`
slides toward the faces traversing the walk direction and `t < 0` toward the
faces traversing it backwards.

`build_tools`' `faceTraverses` could not be reused: it only considers boundary
edges and returns false as soon as an edge has two faces, and a loop slide runs
through interior quads. The new `faceAlong` answers the two-sided question.

Targets are computed from the ORIGINAL positions before any vertex moves, so the
result does not depend on which loop edge seeds it. A test seeds the same loop
from its first and last edge and requires identical output.

`|t| >= 1` is refused rather than clamped: at 1 the loop lands on its neighbour
and every rail edge collapses to zero length.

## A border edge's loop is one edge — deliberately

`edgeLoopFrom` continues only through valence-4 vertices. A vertex on an open
border has valence 3, so the loop through a border edge is that single edge, and
a slide from it moves two vertices rather than the whole border row.

The alternative — making slide walk the boundary — was rejected for consistency:
the tag-loop gesture uses `edgeLoopFrom`, and tapping a border edge must not tag
one set of vertices and slide another. There should be one definition of "the
loop through this edge". A test pins the current definition, so if it ever
changes to include borders, slide visibly changes with it rather than silently.

This is a UX decision a host may reasonably want revisited; it is recorded here
rather than buried.
