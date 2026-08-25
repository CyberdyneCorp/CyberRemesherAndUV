#pragma once

#include "cyber/core/mesh.hpp"

// Subdivision modes for the whole-mesh subdivide command (manual-retopology
// spec, "Whole-mesh commands").
//
// Two modes share ONE topology pass — Mesh::linearSubdivide — because the
// Catmull-Clark topology is the same either way (n-gon -> n quads around the
// centroid); only the vertex POSITIONS differ. Linear leaves every new vertex
// on the source facets, so it adds resolution and no curvature. Catmull-Clark
// then repositions the vertex- and edge-points with the smooth rules, so the
// cage converges to its limit surface without needing a Target to reproject
// onto. Linear stays the default: it is what every existing caller asked for,
// and it is the only mode that leaves the cage exactly on the sculpt's facets.
namespace cyber::retopo {

enum class SubdivisionMode {
    // Vertices stay put, edge-points at the midpoint, face-points at the
    // centroid. No smoothing.
    Linear = 0,
    // Catmull-Clark smooth rules, with the sharp-crease rule on creases.
    CatmullClark = 1,
};

// One subdivision step. Returns a new mesh; every element id is reassigned
// (Mesh::linearSubdivide builds from scratch), in both modes.
//
// CREASES. Catmull-Clark uses the sharp (crease) rule on any edge that is
// tagged as a feature edge OR is not shared by exactly two faces — boundary
// edges (one face) and non-manifold edges (three or more) included. This is
// the same "feature or boundary" predicate relaxQuadMesh() freezes on in
// src/core/src/pipeline.cpp, deliberately: the two stages must agree on what a
// crease is, or a subdivision that rounds a crease off is followed by a relax
// that pins the rounded result. Consequences:
//   - an open patch keeps its border on the boundary curve instead of
//     shrinking inward, because the border is a crease;
//   - a vertex with two incident creases follows the crease curve (the
//     1/8-3/4-1/8 cubic B-spline rule), so a crease stays a crease;
//   - a vertex with three or more incident creases, or a valence-2 border
//     vertex (a patch corner), is a corner and does not move at all;
//   - a mesh with NO feature tags subdivides fully smoothly. Call
//     Mesh::tagFeatureEdges() first to keep hard edges hard.
//
// ATTRIBUTES are propagated by the shared topology pass and are identical in
// both modes: Catmull-Clark only writes positions.
[[nodiscard]] Mesh subdivide(const Mesh& mesh, SubdivisionMode mode);

}  // namespace cyber::retopo
