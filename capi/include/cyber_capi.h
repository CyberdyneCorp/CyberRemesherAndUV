/*
 * CyberRemesher C ABI (capi module, task 13.1).
 *
 * A stable, pure-C facade over the C++20 remeshing pipeline. This is the
 * single surface that language bindings (Python, Swift, C#, ...) link
 * against: opaque handles, integer status codes, plain-old-data parameter
 * structs and C function pointers for progress/cancellation. No C++ types,
 * exceptions or name mangling cross this boundary.
 */
#ifndef CYBER_CAPI_H
#define CYBER_CAPI_H

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque mesh handle wrapping the internal cyber::Mesh. Only ever passed by
 * pointer; created by cyber_mesh_load_obj / cyber_remesh, released with
 * cyber_mesh_free. */
typedef struct CyberMesh CyberMesh;

/* Return code for every fallible entry point. CYBER_OK is guaranteed 0 so
 * callers may test `if (status)` for failure. */
typedef enum CyberStatus {
    CYBER_OK = 0,
    CYBER_ERR_IO,            /* file missing, unreadable, or unwritable */
    CYBER_ERR_INVALID_ARG,   /* a required pointer argument was NULL */
    CYBER_ERR_INVALID_PARAM, /* remesh parameters were unusable (NaN, etc.) */
    CYBER_ERR_EMPTY,         /* mesh had no geometry */
    CYBER_ERR_RUNTIME,       /* pipeline failed */
    CYBER_ERR_CANCELLED      /* the operation was cooperatively cancelled */
} CyberStatus;

/* Engine semantic version (mirrors the CMake project() version). */
void cyber_version(int* major, int* minor, int* patch);

/* Human-readable, static string for a status code. Never NULL. */
const char* cyber_status_string(CyberStatus status);

/* Thread-local description of the most recent failure on the calling thread,
 * or an empty string if the last call succeeded. Never NULL. The pointer is
 * valid until the next capi call on this thread. */
const char* cyber_last_error(void);

/* ---- mesh I/O -------------------------------------------------------- */

/* Loads a Wavefront OBJ. On success *out receives a newly allocated handle
 * the caller must release with cyber_mesh_free. */
CyberStatus cyber_mesh_load_obj(const char* path, CyberMesh** out);

/* Writes a mesh to a Wavefront OBJ (a sibling .mtl may be written too). */
CyberStatus cyber_mesh_save_obj(const CyberMesh* mesh, const char* path);

/* Releases a handle. NULL is ignored. */
void cyber_mesh_free(CyberMesh* mesh);

/* ---- remeshing ------------------------------------------------------- */

/* Canonical user-facing remesh parameters (POD mirror of the C++ struct).
 * Fill with cyber_default_params, then override fields as needed. */
typedef struct CyberRemeshParams {
    int targetQuads;           /* desired output quad count */
    float edgeScale;           /* multiplier on the derived edge length */
    float sharpEdgeDegrees;    /* dihedral threshold for feature edges */
    float smoothNormalDegrees; /* normal-smoothing angle */
    float adaptivity;          /* 0 uniform .. 1 fully curvature-adaptive */
    int pureQuads;             /* non-zero: forbid residual triangles */
    int holeFillMaxBoundary;   /* max boundary edges of holes to fill; 0 off */
    int quadMethod;            /* 0 = field-aligned matching,
                                * 1 = Instant-Meshes position-field extractor,
                                * 2 = integer-parametrization extractor,
                                * 3 = QuadCover seamless-UV isoline extractor (default;
                                *     falls back to field-aligned where no solver is present) */
} CyberRemeshParams;

/* Quadrangulator selection values for CyberRemeshParams.quadMethod. */
#define CYBER_QUAD_FIELD_ALIGNED 0
#define CYBER_QUAD_INSTANT_MESHES 1
#define CYBER_QUAD_INTEGER 2
/* QuadCover seamless-UV isoline extractor (Task F). QuadriFlow-class irregular/CV. The
 * seamless-UV solve runs in-process when built with -DCYBER_WITH_QUADCOVER=ON, or
 * out-of-process when the CYBER_QUADCOVER_CLI environment variable points at a built
 * autoremesher_cli. When neither is present the engine falls back to field-aligned. */
#define CYBER_QUAD_QUADCOVER 3

/* Fills params with the engine defaults. No-op on NULL. */
void cyber_default_params(CyberRemeshParams* params);

/* Cancellation callback: return non-zero to request cooperative cancel. */
typedef int (*CyberCancelCb)(void* user);

/* Progress callback: fraction in [0,1] and a short stage label. */
typedef void (*CyberProgressCb)(float fraction, const char* stage, void* user);

/* Runs the automatic remeshing pipeline. `in` is never modified. On success
 * *out receives a newly allocated result handle (release with
 * cyber_mesh_free). Either callback may be NULL; `user` is passed through to
 * both. */
CyberStatus cyber_remesh(const CyberMesh* in, const CyberRemeshParams* params,
                         CyberProgressCb progress, CyberCancelCb cancel, void* user,
                         CyberMesh** out);

/* ---- statistics ------------------------------------------------------ */

/* Topology summary of a mesh. */
typedef struct CyberStats {
    size_t vertices;
    size_t quads;
    size_t triangles;
    size_t other;         /* faces that are neither triangles nor quads */
    size_t islands;       /* connected components */
    size_t islandsFailed; /* islands the pipeline could not remesh (0 for
                             meshes not produced by cyber_remesh) */
} CyberStats;

/* Computes topology statistics for a mesh. */
CyberStatus cyber_mesh_stats(const CyberMesh* mesh, CyberStats* out);

/* ---- UV atlas -------------------------------------------------------- */

/* Automatic UV-atlas parameters (POD mirror of cyber::uv::AtlasOptions).
 * Fill with cyber_default_atlas_params, then override as needed. */
typedef struct CyberAtlasParams {
    float maxChartAngleDegrees; /* normal-coherence bound for chart growth */
    float packMargin;           /* gap around each island, in UV units */
    int textureSize;            /* resolution for the texel-density readout */
    int reorientCharts;         /* non-zero: rotate each chart to its minimum-
                                 * area bounding box before packing */
    int mergeCharts;            /* non-zero: merge adjacent charts sharing a
                                 * normal cone (fewer seams, same flatness) */
    float maxChartDistortion;   /* looser merge cap: keep merging while the
                                 * union's max conformal error stays <= this
                                 * (0 disables the second pass) */
} CyberAtlasParams;

/* Aggregate atlas quality/packing report. */
typedef struct CyberAtlasResult {
    int chartCount;             /* number of charts (islands) */
    size_t seamEdges;           /* edges cut to form the charts */
    float maxAngleDistortion;   /* worst conformal error across charts, [0,1) */
    float rmsAngleDistortion;   /* RMS conformal error across charts */
    int flippedCharts;          /* charts with mirrored net UV winding */
    int fallbackCharts;         /* charts unwrapped by planar-projection fallback */
    float packedArea;           /* fraction of the unit square covered */
    float texelDensity;         /* texels per UV unit at the packed scale */
} CyberAtlasResult;

/* Fills params with the engine defaults. No-op on NULL. */
void cyber_default_atlas_params(CyberAtlasParams* params);

/* Automatic UV atlas: seams `mesh` into normal-coherent charts, LSCM-unwraps
 * each chart, packs them into the unit square and writes the per-corner UV
 * attribute IN PLACE (so a subsequent cyber_mesh_save_obj emits vt / f v/vt).
 * `params` may be NULL (defaults); `out` may be NULL. Returns CYBER_ERR_RUNTIME
 * when the engine was built without the UV module. */
CyberStatus cyber_uv_atlas(CyberMesh* mesh, const CyberAtlasParams* params,
                           CyberAtlasResult* out);

/* Outcome of a single-island re-unwrap. */
typedef struct CyberReunwrapResult {
    size_t faces;                /* faces in the island that was re-unwrapped */
    int needsWholeMeshUnwrap;    /* refused: the mesh carries no UVs at all (see below) */
    int iterations;              /* conformal solve iterations */
} CyberReunwrapResult;

/* Re-unwraps ONE island in place (add-stage-dependent-x-gesture): the island containing
 * `face_id`, keeping it inside the UV footprint it already occupied and leaving every other
 * island's UVs untouched. This is the localized counterpart to cyber_uv_atlas, which
 * re-charts and REPACKS the whole mesh -- using that to service a gesture aimed at one
 * region would re-lay-out the entire model.
 *
 * The island is fitted back UNIFORMLY and centred. Stretching it to exactly fill its old box
 * would scale u and v by different factors whenever the aspect changed, and that shear
 * destroys the conformality the solve just computed.
 *
 * `params` MUST match what the mesh was unwrapped with: the island partition is derived from
 * the same seam source cyber_uv_atlas uses (authored seams when set, else autoSeams with
 * these params), and a different partition means the "island" re-unwrapped is not the one the
 * caller pointed at. NULL selects the same defaults cyber_uv_atlas would.
 *
 * Sets needsWholeMeshUnwrap and REFUSES when the mesh has no UVs yet, rather than creating
 * the column: UVs are a single per-corner attribute, so writing one island into a fresh
 * column leaves every OTHER island's corners at (0,0) -- a full, non-NULL UV stream in which
 * untouched islands read as a real layout collapsed at the origin. The caller runs a
 * whole-mesh unwrap instead; that policy is the caller's, and refusing here makes this
 * impossible to misuse into it.
 *
 * Returns CYBER_ERR_INVALID_ARG for a dead face or a face in no island. Any failure leaves
 * every UV in the mesh exactly as it was. `out` may be NULL. */
CyberStatus cyber_uv_reunwrap_island(CyberMesh* mesh, uint32_t face_id,
                                     const CyberAtlasParams* params,
                                     CyberReunwrapResult* out);

/* Repacks every island into the axis-aligned UV region [min_u,min_v]-[max_u,max_v]
 * (add-uv-packing-aids). Pass 0,0,1,1 for the whole unit square.
 *
 * Preserves each island's internal UVs: only a UNIFORM scale and a translation are applied per
 * island, so packing never changes how a shell is parameterized, and a non-square region does
 * not shear anything. A zero-area region is refused rather than collapsing every island onto a
 * point.
 *
 * `params` selects margin/strategy and the island partition, exactly as for the re-unwrap:
 * pass what the mesh was unwrapped with. Requires existing UVs. `out` may be NULL. */
CyberStatus cyber_uv_pack_region(CyberMesh* mesh, float min_u, float min_v, float max_u,
                                 float max_v, const CyberAtlasParams* params,
                                 CyberAtlasResult* out);

/* Distributes islands whose bounding boxes overlap so that none do, preserving each island's
 * size and orientation (translations only). The double-tap overlap aid. */
CyberStatus cyber_uv_distribute_islands(CyberMesh* mesh, const CyberAtlasParams* params);

/* Mirrored islands: writes the id of ONE face from each island whose net signed UV area is
 * negative. Faces rather than island indices, because an island index is meaningless to a
 * caller that has not recomputed the same partition — a face id is addressable.
 *
 * Two-call size-then-fill: pass NULL to learn the count. */
CyberStatus cyber_uv_flipped_islands(const CyberMesh* mesh, const CyberAtlasParams* params,
                                     uint32_t* out_face_ids, size_t capacity,
                                     size_t* out_count);

/* Flips (mirrors) the island containing `face_id` about its own bounding-box centre, reversing
 * its winding. About its own centre so the flip does not also translate the island out of the
 * region it was packed into; flipping twice is the identity. */
CyberStatus cyber_uv_flip_island(CyberMesh* mesh, uint32_t face_id,
                                 const CyberAtlasParams* params);

/* --- Multiple UV sets (add-uv-sets) ------------------------------------------
 *
 * A mesh carries one or more NAMED UV sets. The ACTIVE set is the ordinary per-corner UV
 * attribute every other UV entry point reads and writes, so nothing else in the UV surface needs
 * to know sets exist and none of it can read the wrong one. Switching sets swaps the columns.
 *
 * Names must be non-empty and must not contain ':' (the internal separator for a stored set). */

/* Number of sets, and the name of one by index (ascending, so enumeration is deterministic).
 * `cyber_mesh_uv_set_name` follows the usual size-then-fill shape: pass NULL to learn the length,
 * INCLUDING the NUL terminator. */
size_t cyber_mesh_uv_set_count(const CyberMesh* mesh);
CyberStatus cyber_mesh_uv_set_name(const CyberMesh* mesh, size_t index, char* out, size_t capacity,
                                   size_t* out_length);

/* The active set's name. Same size-then-fill shape. */
CyberStatus cyber_mesh_uv_set_active(const CyberMesh* mesh, char* out, size_t capacity,
                                     size_t* out_length);

/* Creates `name` as a COPY of the active set, which stays active. A copy rather than an empty set:
 * a new set is nearly always a variant of the layout you already have, and an empty one would read
 * downstream as a real layout collapsed at the origin. Requires an existing UV layout. */
CyberStatus cyber_mesh_uv_set_create(CyberMesh* mesh, const char* name);

/* Makes `name` active, storing the previously active set under its own name. */
CyberStatus cyber_mesh_uv_set_activate(CyberMesh* mesh, const char* name);

/* Deletes a set. REFUSES the active one, which would otherwise leave the mesh with no active
 * set. */
CyberStatus cyber_mesh_uv_set_delete(CyberMesh* mesh, const char* name);

/* Renames a set, active or stored. */
CyberStatus cyber_mesh_uv_set_rename(CyberMesh* mesh, const char* from, const char* to);

/* Serializes EVERY set, including the active one, as the document's UV-set sidecar. The active
 * set is included even though the OBJ payload also carries it as `vt`: the payload's copy exists
 * for export and for readers that know nothing about sets, and a sidecar missing the active set
 * would only be complete once the payload had been parsed too.
 *
 * Size-then-fill: pass NULL to learn the length. */
CyberStatus cyber_mesh_uv_sets_serialize(const CyberMesh* mesh, uint8_t* out, size_t capacity,
                                         size_t* out_length);

/* Restores sets from a sidecar. Rejected — leaving every UV untouched — when the blob is
 * truncated, carries an unknown version, or was written for a different corner count: a UV set
 * applied to the wrong topology shears every island, which looks plausible and is wrong.
 *
 * REPLACES the mesh's stored sets rather than merging, so sets from a previously loaded document
 * cannot survive into this one. */
CyberStatus cyber_mesh_uv_sets_deserialize(CyberMesh* mesh, const uint8_t* bytes, size_t length);

/* UDIM tiles (add-uv-sets-and-stacking). Occupied tiles in standard numbering (1001 + u +
 * 10*v), ascending and deduplicated — the list an exporter needs to name its texture files.
 *
 * A tile assignment is never STORED: a tile IS a region of UV space, so it is derived from the
 * UVs by construction. Persisting it beside the UVs would create a second source of truth that
 * could disagree with the geometry, and the disagreement would only appear at export.
 *
 * Two-call size-then-fill. Empty (not an error) when the mesh has no UVs. */
CyberStatus cyber_uv_udim_tiles(const CyberMesh* mesh, const CyberAtlasParams* params,
                                int32_t* out_tiles, size_t capacity, size_t* out_count);

/* Moves the island containing `face_id` into UDIM tile `tile`, by a WHOLE number of tiles so its
 * position within the tile and its size are preserved. */
CyberStatus cyber_uv_assign_island_tile(CyberMesh* mesh, uint32_t face_id, int32_t tile,
                                        const CyberAtlasParams* params);

/* Islands whose UVs span more than one tile — they would be split across texture files on
 * export, which is almost never intended. One representative face id per straddling island. */
CyberStatus cyber_uv_straddling_islands(const CyberMesh* mesh, const CyberAtlasParams* params,
                                        uint32_t* out_face_ids, size_t capacity,
                                        size_t* out_count);

/* Stacks mirrored island pairs onto shared UV space (add-uv-sets-and-stacking), so a left/right
 * pair costs one texture region instead of two.
 *
 * The plane is given as a point and a normal in world space — the caller passes the document's
 * own symmetry plane, so stacking cannot disagree with the axis the artist enabled.
 *
 * Pairs are matched by GEOMETRY (reflected face centroids), never by island order, which is an
 * artifact of face-id iteration. `tolerance` is relative to each island's own size, so one value
 * means the same thing on a fingertip and a torso. Writes the number of islands stacked to
 * `out_stacked` (may be NULL); 0 means nothing matched, which is the correct answer for a
 * non-symmetric mesh rather than a wrong pairing. */
/* Island UV transform (add-uv-island-editing): moves, rotates and scales the island containing
 * `face_id` in UV space, in ONE atomic call.
 *
 * Combined rather than three calls because a multitouch pinch produces translation, rotation and
 * scale simultaneously — applying them as separate operations would journal three steps for one
 * gesture and would compose about drifting pivots.
 *
 * Order is scale, then rotate, then translate, all about the island's UV centroid. Rotation and
 * scale do NOT commute, so the order is fixed here rather than left to the caller. */
CyberStatus cyber_uv_transform_island(CyberMesh* mesh, uint32_t face_id, float translate_u,
                                      float translate_v, float radians, float scale,
                                      const CyberAtlasParams* params);

/* Moves the UV VERTEX nearest `at_u,at_v` within the island containing `face_id` by
 * `delta_u,delta_v` (add-uv-imported-preview: UV2D per-vertex mode).
 *
 * A "UV vertex" is a CLUSTER of coincident corners, not a mesh vertex. UVs are per-corner, so
 * several corners usually share one UV position; moving only one would tear the island at a point
 * the artist never cut. Every corner within `tolerance` of the position moves together.
 *
 * This preserves SEAMS without knowing anything about them: corners across a seam sit at different
 * UVs by definition, so they fall outside each other's tolerance and move independently.
 *
 * `out_moved` (may be NULL) receives the number of corners moved. ZERO is not an error — a drag that
 * matched no vertex simply is not an edit — so the caller checks the count rather than the status. */
CyberStatus cyber_uv_move_island_uv_vertex(CyberMesh* mesh, uint32_t face_id, float at_u,
                                          float at_v, float delta_u, float delta_v,
                                          float tolerance, const CyberAtlasParams* params,
                                          size_t* out_moved);

/* Snaps a grid-topology island onto an axis-aligned regular UV grid ("Build Quad on a grid
 * island" / grid straightening). `tolerance` is relative to the island's coordinate range. */
CyberStatus cyber_uv_grid_straighten_island(CyberMesh* mesh, uint32_t face_id, float tolerance,
                                            const CyberAtlasParams* params);

/* Partial symmetrization: pulls each of the island's UVs toward the midpoint of itself and its
 * nearest mirrored counterpart, by `strength` in [0,1]. At 1 the island becomes symmetric about
 * the axis through `axis_point` with direction `axis_dir`. */
CyberStatus cyber_uv_symmetrize_island(CyberMesh* mesh, uint32_t face_id, float axis_point_u,
                                       float axis_point_v, float axis_dir_u, float axis_dir_v,
                                       float strength, const CyberAtlasParams* params);

/* Copies per-corner UVs from the island containing `src_face_id` onto the island containing
 * `dst_face_id`.
 *
 * Requires matching topology (same face count and per-face corner counts, in island order) and
 * copies by that order — this is deliberately the ORDER-based clone, for islands that are
 * genuine topological duplicates. It is NOT how mirrored pairs are stacked; see
 * cyber_uv_stack_mirrored_islands, which corresponds by reflected position because index
 * matching across a mirror scrambles the shell. Returns CYBER_ERR_INVALID_ARG when the islands
 * do not match, leaving every UV untouched. */
CyberStatus cyber_uv_clone_island(CyberMesh* mesh, uint32_t src_face_id, uint32_t dst_face_id,
                                  const CyberAtlasParams* params);

/* Stitches (merges) islands along `edge_ids`: each edge stops being a seam and the corners it
 * separates are welded to their shared average, so the boundary fits together. The handle's
 * authored seam set is updated, so a subsequent unwrap sees the sewn topology. */
CyberStatus cyber_uv_stitch_islands(CyberMesh* mesh, const uint32_t* edge_ids, size_t count,
                                    const CyberAtlasParams* params);

CyberStatus cyber_uv_stack_mirrored_islands(CyberMesh* mesh, float plane_point_x,
                                            float plane_point_y, float plane_point_z,
                                            float plane_normal_x, float plane_normal_y,
                                            float plane_normal_z, float tolerance,
                                            const CyberAtlasParams* params, size_t* out_stacked);

/* --- Accessors used by the language bindings (Python, Swift) ------------- */

/* Creates an empty mesh handle (release with cyber_mesh_destroy/free). */
CyberMesh* cyber_mesh_create(void);
/* Alias of cyber_mesh_free (the bindings use the create/destroy pairing). */
void cyber_mesh_destroy(CyberMesh* mesh);
/* Number of live vertices / faces. */
size_t cyber_mesh_vertex_count(const CyberMesh* mesh);
size_t cyber_mesh_face_count(const CyberMesh* mesh);
/* Copies compacted vertex positions (x,y,z per vertex) into `out`, writing at
 * most `max_floats`; returns the number of floats written. Pass out=NULL to
 * query the required count (3 * vertex_count). */
size_t cyber_mesh_copy_positions(const CyberMesh* mesh, float* out, size_t max_floats);
/* Alias of cyber_default_params. */
void cyber_remesh_params_default(CyberRemeshParams* params);

/* --- Render-data accessors (viewport renderers) --------------------------
 *
 * Everything below reads from a per-handle compacted render cache built
 * lazily on first access:
 *   - vertices are compacted to the same deterministic order used by
 *     cyber_mesh_copy_positions (live vertices in id order), so positions,
 *     normals, colors and triangle indices are all mutually consistent;
 *   - faces are fan-triangulated deterministically around their first
 *     corner: a live face (v0 v1 ... vn-1), taken in id order, emits
 *     (v0,v1,v2), (v0,v2,v3), ... — an n-gon yields n-2 triangles, so a
 *     quad mesh yields exactly 2 triangles per quad, and repeated calls on
 *     an unchanged handle return identical buffers.
 *
 * These accessors are not thread-safe with each other on the same handle
 * (the first call builds the cache); serialize per-handle access. */

/* Number of triangles the live faces fan-triangulate to (an n-gon counts
 * n-2). Returns 0 on NULL. */
size_t cyber_mesh_triangle_count(const CyberMesh* mesh);

/* Copies the triangulated index buffer (3 uint32 per triangle, indexing the
 * compacted vertex order of cyber_mesh_copy_positions) into `out`, writing
 * whole triangles only, at most `max_indices` values; returns the number of
 * indices written. Pass out=NULL to query the required count
 * (3 * triangle_count). */
size_t cyber_mesh_copy_triangle_indices(const CyberMesh* mesh, uint32_t* out,
                                        size_t max_indices);

/* Copies per-vertex unit normals (x,y,z per vertex, compacted vertex order)
 * into `out`, at most `max_floats`; returns the number of floats written.
 * When the mesh carries imported per-corner normals they are averaged per
 * vertex; otherwise normals are computed engine-side from face normals
 * (Newell). A vertex with no usable normal (isolated, degenerate) gets
 * (0,0,1). Pass out=NULL to query the required count (3 * vertex_count). */
size_t cyber_mesh_copy_normals(const CyberMesh* mesh, float* out, size_t max_floats);

/* Non-zero when the mesh carries per-vertex colors (e.g. an OBJ with
 * "v x y z r g b" polypaint data). */
int cyber_mesh_has_colors(const CyberMesh* mesh);

/* Copies per-vertex linear RGB colors (r,g,b per vertex, compacted vertex
 * order) into `out`, at most `max_floats`; returns the number of floats
 * written, or 0 when the mesh has no colors. Pass out=NULL to query the
 * required count (3 * vertex_count, or 0 without colors). */
size_t cyber_mesh_copy_colors(const CyberMesh* mesh, float* out, size_t max_floats);

/* Number of unique undirected face edges (a quad contributes 4 edges, never
 * its triangulation diagonal — wireframe overlays draw the authored
 * topology, not the fan triangulation). Deterministic: edges are emitted
 * per live face in id order, each kept on first sighting. Returns 0 on
 * NULL. */
size_t cyber_mesh_edge_count(const CyberMesh* mesh);

/* ID-SPACE CAPACITY: one past the highest id an element may currently have.
 *
 * Stable ids are sparse — an edit that removes elements leaves holes, so the
 * live COUNT is not a bound on the ids. A caller enumerating elements by
 * probing ids must run to the capacity; bounding the probe by the live count
 * (even generously) silently misses the high ids of an edited mesh, which is
 * exactly how a dissolve-then-scan pass loses edges that are still there.
 * Returns 0 on NULL. */
size_t cyber_mesh_vertex_capacity(const CyberMesh* mesh);
size_t cyber_mesh_edge_capacity(const CyberMesh* mesh);

/* Copies the unique-edge index buffer (2 uint32 per edge, indexing the
 * compacted vertex order of cyber_mesh_copy_positions) into `out`, writing
 * whole edges only, at most `max_indices` values; returns the number of
 * indices written. Pass out=NULL to query the required count
 * (2 * edge_count). */
size_t cyber_mesh_copy_edge_indices(const CyberMesh* mesh, uint32_t* out,
                                    size_t max_indices);

/* ---- zero-copy render buffer views ------------------------------------
 *
 * Direct pointers into the handle's internal render cache — no copy. On
 * Apple unified memory a renderer may hand these straight to the GPU (e.g.
 * memcpy-free MTLBuffer setup or bounded per-frame uploads).
 *
 * LIFETIME: a returned pointer remains valid until the mesh handle is
 * mutated by any mutating C API call or released with
 * cyber_mesh_free/cyber_mesh_destroy, whichever comes first. The mutating
 * entry points are the cyber_retopo_* editing ops below; every one of them
 * invalidates the render cache, so pointer views obtained before the call
 * are dead afterwards and must be re-fetched (the next accessor rebuilds
 * the cache lazily). The compacted vertex ORDER also changes when vertices
 * are added or removed — anything keyed on compacted render indices is
 * stale after a mutation; address elements by their stable engine ids.
 * The pointers must not outlive the handle and must not be written
 * through.
 *
 * Each accessor stores the element count (floats / indices) in *out_count
 * (ignored when NULL) and returns NULL with *out_count = 0 when the mesh is
 * NULL, empty, or — for colors — carries no per-vertex colors. */
/* ---- meshlet clusters (add-meshlet-target-path, task 3.2) ----------------
 *
 * Clusters of the SAME compacted render streams the pointer views above
 * expose, for the Metal mesh-shader Target path: each meshlet's vertex
 * indices address exactly the array cyber_mesh_positions_ptr returns, so a
 * renderer binds one position buffer and one normal buffer for every
 * cluster.
 *
 * Built lazily into the handle's render cache and invalidated by the same
 * mutating calls, under the identical LIFETIME contract described above --
 * including that the compacted vertex ORDER changes on mutation, so a
 * meshlet fetched before an edit is doubly stale afterwards.
 *
 * Each cluster carries a bounding sphere and a normal cone. Both are
 * CONSERVATIVE by construction: the sphere contains every cluster vertex and
 * the cone contains every cluster triangle normal. A cluster whose normals
 * span more than a hemisphere reports cone_cutoff == 0, which means NEVER
 * backface-cull this cluster -- culling a visible cluster leaves a hole in
 * the model, so the ambiguous case fails safe. */
typedef struct CyberMeshlet {
    uint32_t vertex_offset;    /* into cyber_mesh_meshlet_vertices_ptr        */
    uint32_t vertex_count;
    uint32_t triangle_offset;  /* in TRIANGLES, into ..._meshlet_indices_ptr  */
    uint32_t triangle_count;
    float center[3];           /* bounding sphere, conservative               */
    float radius;
    float cone_axis[3];        /* average triangle normal                     */
    float cone_cutoff;         /* min dot(axis, n_i); 0 == do not cull        */
} CyberMeshlet;

/* The clusters themselves. *out_count receives the meshlet count. */
/* Per-TRIANGLE-CORNER UV coordinates over the same triangulation
 * cyber_mesh_triangle_indices_ptr describes: u,v per emitted corner, so 6 floats per
 * triangle and exactly parallel to that index stream
 * (add-uv-stage-foundation).
 *
 * Per CORNER, not per vertex: UVs are a corner attribute, and a seam gives one vertex
 * several different UVs -- collapsing them to the vertex would weld every seam shut.
 *
 * Returns NULL with *out_count == 0 when the mesh has NEVER been unwrapped, which is
 * deliberately distinguishable from a layout whose coordinates happen to be zero.
 * Same lifetime rules as the other pointer views: valid until the next mutating call. */
const float* cyber_mesh_uvs_ptr(const CyberMesh* mesh, size_t* out_count);

/* Per-face UV distortion (add-uv-distortion-heatmaps). One entry per LIVE face, in the
 * same order cyber_mesh_solved_faces / the face walk uses, so a caller can index it
 * against face rings it reconstructed itself. */
typedef struct CyberFaceDistortion {
    float angle;   /* conformal error in [0,1); 0 is angle-preserving */
    float area;    /* |uvArea| / surfaceArea, unnormalized; 0 for a collapsed face */
    int flipped;   /* non-zero when the face's UV winding is reversed */
} CyberFaceDistortion;

/* Returns NULL with *out_count == 0 when the mesh has NO UV layout. Deliberately
 * distinguishable from zero distortion: no layout and a perfect layout are different
 * states, and reporting zeros for both would show a never-unwrapped mesh as flawless.
 *
 * Computed by the same cyber::uv::measureDistortion the atlas uses for its aggregate
 * figures, so the per-face view and CyberAtlasResult cannot disagree.
 *
 * Same lifetime rules as the other pointer views: valid until the next mutating call. */
const CyberFaceDistortion* cyber_mesh_uv_distortion_ptr(const CyberMesh* mesh,
                                                        size_t* out_count);

const CyberMeshlet* cyber_mesh_meshlets_ptr(const CyberMesh* mesh, size_t* out_count);
/* Global vertex indices, grouped per meshlet (index into the position and
 * normal pointer views). */
const uint32_t* cyber_mesh_meshlet_vertices_ptr(const CyberMesh* mesh, size_t* out_count);
/* Triangle corners as LOCAL offsets into the owning meshlet's vertex slice,
 * 3 per triangle, 8-bit because a meshlet holds at most 256 vertices. */
const uint8_t* cyber_mesh_meshlet_indices_ptr(const CyberMesh* mesh, size_t* out_count);

const float* cyber_mesh_positions_ptr(const CyberMesh* mesh, size_t* out_count);
const uint32_t* cyber_mesh_triangle_indices_ptr(const CyberMesh* mesh, size_t* out_count);
const uint32_t* cyber_mesh_edge_indices_ptr(const CyberMesh* mesh, size_t* out_count);
const float* cyber_mesh_normals_ptr(const CyberMesh* mesh, size_t* out_count);
const float* cyber_mesh_colors_ptr(const CyberMesh* mesh, size_t* out_count);

/* ---- overlay render state (retopology phase 3, task 3.4) ----------------
 *
 * Per-handle visibility and tag state for the gesture grammar's hide/tag
 * verbs. Pure RENDER-CACHE filters: they never touch topology or stable
 * element ids (spatial queries and editing ops still see hidden faces),
 * but every setter invalidates the render cache and its pointer views, so
 * the next accessor rebuilds filtered buffers. Ids reference stable
 * elements; ids that are dead at build time are skipped silently. */

/* Stable ids of every live face, in id order (the universe the visibility
 * gestures complement against — "invert visibility" hides exactly the
 * faces not currently hidden). copy_positions convention: returns the
 * TOTAL live-face count and fills at most max_faces entries of out_faces
 * (which may be NULL when max_faces is 0, for size queries). */
size_t cyber_mesh_live_faces(const CyberMesh* mesh, uint32_t* out_faces,
                             size_t max_faces);

/* Stable vertex ids around one face, in ring order. copy_positions
 * convention: returns the face's vertex COUNT (its valence) and fills at
 * most max_verts entries of out_verts (which may be NULL when max_verts is
 * 0, for size queries). Returns 0 for a dead or out-of-range face id. */
size_t cyber_mesh_face_vertices(const CyberMesh* mesh, uint32_t face,
                                uint32_t* out_verts, size_t max_verts);

/* Replaces the hidden-face set (faces may be NULL when face_count is 0 —
 * show all). Hidden faces are dropped from the triangle, edge, and normal
 * streams; vertices used exclusively by hidden faces are dropped from the
 * compacted position/color streams. */
CyberStatus cyber_mesh_set_hidden_faces(CyberMesh* mesh, const uint32_t* faces,
                                        size_t face_count);

/* Number of face ids currently in the hidden set (dead ids included until
 * the next set call — the set stores ids verbatim). 0 for NULL. */
size_t cyber_mesh_hidden_face_count(const CyberMesh* mesh);

/* Replaces the tagged-edge list (edges may be NULL when edge_count is 0 —
 * clear). Tagged edges surface through the accessor below for the
 * overlay's colored loop-tag pass. */
CyberStatus cyber_mesh_set_tagged_edges(CyberMesh* mesh, const uint32_t* edges,
                                        size_t edge_count);

/* Compacted index pairs (2 per tagged, live, visible edge) in tag order —
 * same LIFETIME contract as the pointer views above. NULL with
 * *out_count = 0 when the mesh is NULL or nothing is tagged. */
const uint32_t* cyber_mesh_tagged_edge_indices_ptr(const CyberMesh* mesh,
                                                   size_t* out_count);

/* ---- Regional prescribed-boundary solve (add-weave-regional-solve) ------- */

/* Selects a REGION for the next cyber_remesh: only these faces are rewritten,
 * every other face keeps its exact geometry, ring and element id, and the
 * solved patch meets the frozen cage on a prescribed boundary.
 *
 * Rides the handle, exactly like the orientation guides. `count == 0` clears.
 * Face ids must be alive and free of repeats; the mesh is untouched on failure.
 *
 * A region solve skips triangulate/weld/orient, islands, hole-fill and
 * pure-quads, so it is NOT equivalent to a whole-mesh solve even when the
 * region names every face -- that case is REFUSED rather than aliased. It also
 * forces the field-aligned quadrangulator, the only vertex-preserving path.
 *
 * What it guarantees: prescribed vertices survive with the same id at a bitwise
 * identical position, and the prescribed interface edge set is preserved.
 * What it does NOT guarantee: that the interface is free of singularities. That
 * is measured and reported (cyber_mesh_region_report), never enforced. */
/* Sets the EXTERNAL surface the next REGION solve projects its interior onto
 * (add-region-external-reference). Without it the region path builds its reference
 * from the mesh it is REWRITING, which for a Weave Fill is the cage plus a grown
 * seed band -- so the solve reprojects onto its own approximation and loses any
 * Target detail finer than that band (measured: 0.42 quads mean, 1.26 quads MAX
 * interior deviation on rippled geometry).
 *
 * `reference` is BORROWED and must outlive the solve. NULL clears. Passing `mesh`
 * itself is refused: that is already the default, so accepting it would look like
 * it did something. Ignored by a whole-mesh solve, whose input is the surface.
 *
 * The surface is built lazily and CACHED on the reference handle, and dropped
 * whenever that handle is mutated -- construction is ~636us per 1k faces, so a
 * multi-million-triangle Target costs seconds and a fill cannot pay that per call. */
/* Authored per-vertex density scales -- the density BRUSH
 * (add-weave-density-radial-symmetry). `scales[i]` multiplies the target edge length
 * at vertex i, so > 1 is coarser and < 1 is finer; entries past the array's end read
 * as 1.0, and count == 0 clears. Every value must be finite and > 0, validated before
 * anything is stored.
 *
 * MULTIPLIED into the solver's curvature-derived scales and clamped to the same
 * [0.3, 3.0] band, so an authored coarse patch does not discard detail preservation
 * the artist did not ask to lose, and the composition is order-independent.
 *
 * Read by a REGION solve only. The whole-mesh path remeshes per island and island
 * extraction RENUMBERS vertices, so these indices would not survive it -- honouring a
 * brush there needs an id mapping first. */
CyberStatus cyber_mesh_set_density_scales(CyberMesh* mesh, const float* scales, size_t count);

/* Hand-drawn UV SEAM edges for the next cyber_uv_atlas (add-uv-seam-authoring). The
 * atlas cuts along exactly these instead of generating its own; count == 0 clears.
 *
 * Authored seams REPLACE the automatic ones rather than being added to them: cutting
 * where the artist did not ask is a worse failure than an under-cut layout, which the
 * distortion report and the per-face heatmap already surface.
 *
 * Every id is validated before anything is stored, so a rejected call leaves the handle
 * exactly as it was. Rides the handle like cyber_mesh_set_solve_region. */
CyberStatus cyber_mesh_set_seam_edges(CyberMesh* mesh, const uint32_t* edge_ids,
                                      size_t count);

/* PROPOSED UV seams (add-auto-seam-proposals): the seams chart growth would choose,
 * treating whatever cyber_mesh_set_seam_edges holds as edges it will NOT cut across and
 * which are always included in the result.
 *
 * So a proposal answers "given the artist's cuts, where else would I cut", and accepting
 * one can never delete a seam they drew. It only ever ADDS.
 *
 * Two-call size-then-fill, like the other id readbacks: pass NULL to learn the count.
 * Proposes but does NOT apply -- the mesh is unchanged, and the caller decides. */
CyberStatus cyber_mesh_propose_seams(const CyberMesh* mesh, uint32_t* out_ids,
                                     size_t capacity, size_t* out_count);

CyberStatus cyber_mesh_set_region_reference(CyberMesh* mesh, const CyberMesh* reference);

CyberStatus cyber_mesh_set_solve_region(CyberMesh* mesh, const uint32_t* face_ids, size_t count);

/* Per-vertex prescribed TOTAL valence overrides for the next region solve, so a
 * deliberately authored pole on the interface is not reported as irregular.
 * Parallel arrays; count == 0 clears. */
CyberStatus cyber_mesh_set_region_valence(CyberMesh* mesh, const uint32_t* vertex_ids,
                                          const int32_t* valences, size_t count);

/* Faces the last region solve produced (live minus frozen). Writes at most
 * `capacity` ids and always reports the true count via out_count. */
CyberStatus cyber_mesh_solved_faces(const CyberMesh* mesh, uint32_t* out_ids, size_t capacity,
                                    size_t* out_count);

/* Interface vertices of the last region solve, ascending. */
CyberStatus cyber_mesh_interface_vertices(const CyberMesh* mesh, uint32_t* out_ids,
                                          size_t capacity, size_t* out_count);

/* Interface vertices whose valence differs from the prescription. Reported, not
 * refused -- see cyber_mesh_set_solve_region. */
CyberStatus cyber_mesh_interface_irregular(const CyberMesh* mesh, uint32_t* out_ids,
                                           size_t capacity, size_t* out_count);

typedef struct CyberRegionReport {
    uint32_t interface_vertex_count;
    uint32_t interface_irregular_count;
    uint32_t interface_triangles;
    int32_t interior_index_budget;
    int32_t index_residual;
} CyberRegionReport;

/* Summary of the last region solve on this handle. CYBER_ERR_INVALID_ARG when
 * the handle did not come from a region solve. */
CyberStatus cyber_mesh_region_report(const CyberMesh* mesh, CyberRegionReport* out);

/* Number of live faces incident to a vertex. The valence primitive the region
 * guarantees are asserted with -- absent from this header until now. */
CyberStatus cyber_mesh_vertex_face_count(const CyberMesh* mesh, uint32_t vertex_id,
                                         uint32_t* out_count);

/* ID-PRESERVING deep copy. The only other cross-ABI mesh copy is a payload
 * (OBJ) round-trip, which renumbers every element -- so it cannot be used to
 * snapshot a mesh for an id-based comparison. Side-channel state (hidden faces,
 * tagged edges, guides, solve region) is copied too. */
CyberStatus cyber_mesh_duplicate(const CyberMesh* mesh, CyberMesh** out);

/* World-space orientation guides that steer the NEXT cyber_remesh on this
 * handle (add-weave-guide-field-steering): each of `count` samples has a
 * world point (points_xyz, 3 floats) and a world tangent direction (dirs_xyz,
 * 3 floats). The remesher projects each sample onto its nearest output face
 * and pins the cross field there, so quad flow follows the guides. Supplying
 * guides forces the field-aligned quadrangulator (the only one that consumes
 * the cross field). Pass count 0 (pointers may be NULL) to clear. Unlike
 * tagged edges these are consumed by remeshing, not rendering. */
CyberStatus cyber_mesh_set_orientation_guides(CyberMesh* mesh, const float* points_xyz,
                                              const float* dirs_xyz, size_t count);

/* ---- spatial queries (retopology phase 3) ------------------------------
 *
 * All entry points in this section are READ-ONLY: they never mutate a mesh
 * handle, so the render cache and the zero-copy pointer views above stay
 * valid across them. */

/* Element ids used by the spatial and gesture APIs are the engine's stable
 * per-element ids (cyber::VertexId/EdgeId/FaceId values) — NOT compacted
 * render-buffer indices. They stay stable across unrelated mutations. */
#define CYBER_INVALID_ID 0xFFFFFFFFu

/* Opaque snapshot snapper over an immutable Target mesh: a BVH for
 * closest-surface/raycast queries plus a vertex table for the vertex-snap
 * modifier (wraps cyber::retopo::SurfaceSnapper). Snapshot semantics: it
 * does NOT observe later changes to the source mesh; recreate it if the
 * Target ever changes. Free with cyber_snapper_free. */
typedef struct CyberSnapper CyberSnapper;

/* Builds a snapper from `target`. Fails with CYBER_ERR_EMPTY when the mesh
 * has no faces. */
CyberStatus cyber_snapper_create(const CyberMesh* target, CyberSnapper** out);
void cyber_snapper_free(CyberSnapper* snapper);

/* Closest point on the Target surface to `query` (x,y,z). Writes the hit
 * position to out_point and the owning face id to out_face (either may be
 * NULL). Returns 1 on a hit, 0 when the snapper is NULL/empty. */
int cyber_snapper_snap_to_surface(const CyberSnapper* snapper, const float query[3],
                                  float out_point[3], uint32_t* out_face);

/* Nearest Target vertex within `radius` of `query`. Returns 1 and fills
 * out_point/out_vertex (either may be NULL) when one exists, else 0. */
int cyber_snapper_snap_to_vertex(const CyberSnapper* snapper, const float query[3],
                                 float radius, float out_point[3], uint32_t* out_vertex);

/* First Target surface hit along the ray origin + t*direction, t in
 * (0, max_distance]. `direction` need not be normalized (it is normalized
 * internally; a zero direction misses). Returns 1 on a hit filling
 * out_point/out_t/out_face (each may be NULL), else 0. */
int cyber_snapper_raycast(const CyberSnapper* snapper, const float origin[3],
                          const float direction[3], float max_distance,
                          float out_point[3], float* out_t, uint32_t* out_face);

/* ---- EditMesh element queries (world space, brute force at cage scale) --
 *
 * Deterministic: distance ties break toward the lower element id. */

/* Nearest live vertex within `max_distance` of `query`. Returns 1 and fills
 * out_vertex/out_position (either may be NULL) when found, else 0. */
int cyber_mesh_nearest_vertex(const CyberMesh* mesh, const float query[3],
                              float max_distance, uint32_t* out_vertex,
                              float out_position[3]);

/* Nearest live vertex within `max_distance` of `query`, skipping
 * `exclude_vertex` (pass a dead/out-of-range id, e.g. UINT32_MAX, to skip
 * nothing). Exists for merge-snap detection while a vertex is being
 * dragged: the dragged vertex sits at the query point, so the unfiltered
 * query could only ever return it. Returns 1 and fills
 * out_vertex/out_position (either may be NULL) when found, else 0. */
int cyber_mesh_nearest_vertex_excluding(const CyberMesh* mesh, const float query[3],
                                        float max_distance, uint32_t exclude_vertex,
                                        uint32_t* out_vertex, float out_position[3]);

/* Nearest live edge (closest point on its segment) within `max_distance` of
 * `query`. Returns 1 and fills out_edge/out_point (either may be NULL) when
 * found, else 0. */
int cyber_mesh_nearest_edge(const CyberMesh* mesh, const float query[3],
                            float max_distance, uint32_t* out_edge,
                            float out_point[3]);

/* Endpoint vertex ids of a live edge. Returns 1 on success, 0 when the mesh
 * is NULL or the edge id is not alive. */
int cyber_mesh_edge_endpoints(const CyberMesh* mesh, uint32_t edge,
                              uint32_t out_vertices[2]);

/* 1 when the live edge borders exactly one face, 0 when interior, -1 when
 * the mesh is NULL or the edge id is not alive. */
int cyber_mesh_is_boundary_edge(const CyberMesh* mesh, uint32_t edge);

/* Position of a live vertex. Returns 1 on success, 0 when the mesh is NULL
 * or the vertex id is not alive. */
int cyber_mesh_vertex_position(const CyberMesh* mesh, uint32_t vertex,
                               float out_position[3]);

/* Live faces adjacent to a live edge, with each face's ring size (vertex
 * count) — the Build Quad/Build Triangle edge-drag semantics dispatch on
 * whether the picked boundary edge borders a quad or a triangle (task
 * 4.1). Fills out_faces/out_sizes (either may be NULL) in deterministic
 * (radial) order. Returns the adjacent live face count (0-2 for manifold
 * meshes), or -1 when the mesh is NULL or the edge id is not alive. */
int cyber_mesh_edge_faces(const CyberMesh* mesh, uint32_t edge,
                          uint32_t out_faces[2], size_t out_sizes[2]);

/* Shortest edge path between two live vertices (Dijkstra over live edges,
 * Euclidean weights; deterministic — Path Distribute's "closest path
 * between the stroke's first and last vertex", task 4.1). Follows the
 * copy_positions convention: returns the TOTAL path vertex count
 * (endpoints inclusive, from -> to order) and fills at most max_vertices
 * entries of out_vertices (may be NULL when max_vertices is 0, for size
 * queries). Returns 0 when either vertex is dead, from == to, or no path
 * exists. */
size_t cyber_mesh_shortest_vertex_path(const CyberMesh* mesh, uint32_t from,
                                       uint32_t to, uint32_t* out_vertices,
                                       size_t max_vertices);

/* Boundary chain through `edge` (retopology phase 4, task 4.2: Extend
 * Boundary's boundary auto-select; see retopo/boundary.hpp): walks the
 * boundary edges (edges with exactly one incident face) through the seed
 * in both directions, stopping when the chain closes, ends, or meets a
 * non-manifold pinch (3+ boundary edges at one vertex). Read-only.
 * Follows the copy_positions convention: returns the TOTAL ordered vertex
 * count and fills at most max_vertices entries of out_vertices (may be
 * NULL when max_vertices is 0, for size queries). Writes 1 to *out_closed
 * (may be NULL) when the chain is a closed loop. Returns 0 when `edge` is
 * dead or not a boundary edge. */
size_t cyber_mesh_boundary_loop(const CyberMesh* mesh, uint32_t edge,
                                uint32_t* out_vertices, size_t max_vertices,
                                int* out_closed);

/* ---- quad-loop topology queries (retopology phase 3, task 3.4) ----------
 *
 * Deterministic loop walks over the EditMesh (see retopo/loops.hpp), the
 * topology behind the "line across a face ring" / "line along a loop"
 * gestures. Read-only: the render cache is untouched. Both follow the
 * copy_positions convention: they return the TOTAL element count and fill
 * at most max_edges entries of `out_edges` (which may be NULL when
 * max_edges is 0, for size queries). A dead seed edge returns 0. */

/* Edge loop through `edge`: the chain continuing through each regular
 * (valence-4 interior) vertex along the topologically opposite edge, in
 * walk order; stops at boundaries/poles or when the loop closes. */
size_t cyber_mesh_edge_loop(const CyberMesh* mesh, uint32_t edge, uint32_t* out_edges,
                            size_t max_edges);

/* ---- loop metrics (retopology phase 4, task 4.3) -------------------------
 *
 * Measurements of the edge loop through a seed edge, backing the Loop Info
 * inspector (manual-retopology spec: "Loop Info inspection (vertex/edge
 * counts, boundary length, snapping state in O(loop) time)"). Read-only:
 * the render cache is untouched. One loop walk plus one pass over its
 * edges — no global scan. */
typedef struct CyberLoopMetrics {
    /* Edges in the loop and distinct vertices they touch. */
    uint32_t edge_count;
    uint32_t vertex_count;
    /* 1 when the walk wrapped around (a closed loop has no endpoints). */
    int closed;
    /* Summed edge length along the loop. */
    float length;
    /* Terminal vertices of an OPEN chain, in walk order; both are
     * CYBER_INVALID_ID and has_endpoints is 0 for a closed loop. */
    int has_endpoints;
    uint32_t endpoint_a;
    uint32_t endpoint_b;
    /* Loop edges that are mesh boundary edges (one incident face). */
    uint32_t boundary_edge_count;
    /* Snapping state against the Target. Measured only when a snapper is
     * supplied (snap_measured 1); "snapped" means within 2% of the loop's
     * mean edge length of the Target surface, so the verdict is
     * scale-free. */
    int snap_measured;
    uint32_t snapped_vertex_count;
    float max_snap_distance;
} CyberLoopMetrics;

/* Fills *out_metrics for the edge loop through `edge`. `snapper` may be
 * NULL (no Target loaded): the snapping fields then stay unmeasured.
 * Returns CYBER_ERR_INVALID_ARG on NULL mesh/out_metrics, and CYBER_OK
 * with edge_count 0 when `edge` is dead. */
CyberStatus cyber_mesh_loop_metrics(const CyberMesh* mesh, uint32_t edge,
                                    const CyberSnapper* snapper,
                                    CyberLoopMetrics* out_metrics);

/* Quad ring through `edge`: the consecutive "across" edges crossing each
 * quad to its opposite edge until the ring closes or hits a boundary /
 * non-quad. Writes 1 to *out_closed (may be NULL) when the ring closed. */
size_t cyber_mesh_quad_ring(const CyberMesh* mesh, uint32_t edge, uint32_t* out_edges,
                            size_t max_edges, int* out_closed);

/* ---- mesh editing (retopology phase 3, task 3.3) -------------------------
 *
 * Mutating entry points over an EditMesh handle, wrapping the engine's
 * retopo operators (actions/relax/move/tweak/erase). Every function here
 * invalidates the handle's render cache (see the LIFETIME note above).
 *
 * `snapper` arguments are optional (NULL disables Target snapping); when
 * given, created/moved vertices are projected onto the Target surface
 * before the operation commits (continuous shrink-wrap snapping). Pin
 * arrays are optional (NULL/0 = no pins); pinned vertices are immune to
 * relax and move but stay movable by explicit tweak. */

/* Creates a face over 3 or 4 NEW vertices at `points_xyz` (x,y,z per
 * point, ring order), each snapped to the Target first when `snapper` is
 * non-NULL. Writes the new face id to *out_face (may be NULL). Fails with
 * CYBER_ERR_INVALID_ARG on a degenerate polygon (wrong count / repeated
 * corners), leaving the mesh unchanged. */
CyberStatus cyber_retopo_create_face(CyberMesh* mesh, const float* points_xyz,
                                     size_t point_count, const CyberSnapper* snapper,
                                     uint32_t* out_face);

/* Tweak: drops a live vertex at `target`, snapped to the Target surface
 * when `snapper` is non-NULL. Tweak ignores pins by design (pinned
 * vertices stay movable by explicit tweak). */
CyberStatus cyber_retopo_tweak_vertex(CyberMesh* mesh, uint32_t vertex,
                                      const float target[3], const CyberSnapper* snapper);

/* Move with surface-geodesic falloff: displaces `seed_vertex` by
 * `displacement`, with a smooth falloff over geodesic (through-the-surface)
 * distance up to `radius`. Vertices of disconnected components are NEVER
 * affected, no matter how close in space. Pinned vertices resist; moved
 * vertices reproject onto the Target when `snapper` is non-NULL. */
CyberStatus cyber_retopo_move(CyberMesh* mesh, uint32_t seed_vertex,
                              const float displacement[3], float radius,
                              const uint32_t* pinned, size_t pinned_count,
                              const CyberSnapper* snapper);

/* Relax: tangential Laplacian smoothing inside the brush (`center`,
 * `radius`; radius <= 0 relaxes the whole mesh). `strength` is the 0..1
 * per-iteration blend. Explicit pins are honoured; auto_pin_corners
 * (non-zero) additionally pins low-valence grid corners so regular patch
 * shapes survive. Vertices reproject onto the Target when `snapper` is
 * non-NULL. */
CyberStatus cyber_retopo_relax(CyberMesh* mesh, const float center[3], float radius,
                               float strength, int iterations, int auto_pin_corners,
                               const uint32_t* pinned, size_t pinned_count,
                               const CyberSnapper* snapper);

/* Erase: removes every face whose centroid lies within the pressure-scaled
 * radius of `center` (radius grows with stylus pressure in [0,1]: half the
 * base radius at 0 up to 1.5x at 1), then any vertices left isolated.
 * Writes the number of removed faces to *out_removed (may be NULL). */
CyberStatus cyber_retopo_erase(CyberMesh* mesh, const float center[3], float base_radius,
                               float pressure, size_t* out_removed);

/* Deletes the listed faces (dead/out-of-range ids are skipped), then any
 * vertices left isolated. Writes the number of faces actually removed to
 * *out_removed (may be NULL). */
CyberStatus cyber_retopo_delete_faces(CyberMesh* mesh, const uint32_t* faces,
                                      size_t face_count, size_t* out_removed);

/* Inserts a COMPLETE edge loop around the quad ring through `edge` (the
 * "line across a face ring" gesture, task 3.4): every ring edge is split
 * at `t` (0..1; 0.5 = midpoints) and every ring quad is split between
 * consecutive midpoints. The single-quad case degenerates to the one-quad
 * insertLoop. Writes the number of NEW faces to *out_new_faces (may be
 * NULL). Fails with CYBER_ERR_INVALID_ARG (mesh unchanged) when `edge` is
 * dead or borders no quad. */
CyberStatus cyber_retopo_insert_loop(CyberMesh* mesh, uint32_t edge, float t,
                                     size_t* out_new_faces);

/* Creates a CONNECTED block of quads over a lattice of
 * (rows + 1) * (cols + 1) NEW vertices (the one-stroke grid gesture,
 * task 3.4): `points_xyz` is the lattice row-major (x,y,z per point), each
 * point snapped to the Target first when `snapper` is non-NULL; lattice
 * points are shared between neighboring cells (one welded block, not
 * rows * cols disconnected quads). Writes the number of created faces to
 * *out_faces (may be NULL). Fails with CYBER_ERR_INVALID_ARG (mesh
 * unchanged) on degenerate lattices (coincident points after snapping). */
CyberStatus cyber_retopo_create_grid(CyberMesh* mesh, const float* points_xyz,
                                     size_t rows, size_t cols,
                                     const CyberSnapper* snapper, size_t* out_faces);

/* Dissolves interior edges (the "scribble over an edge" gesture): each
 * listed edge with exactly two live faces is removed and its faces merged
 * into one (a triangle pair becomes a quad). Dead, boundary, and
 * would-be-degenerate edges are skipped, as are edges invalidated by an
 * earlier dissolve in the same call. Writes the number of edges actually
 * dissolved to *out_dissolved (may be NULL); dissolving none is CYBER_OK
 * with 0. */
CyberStatus cyber_retopo_dissolve_edges(CyberMesh* mesh, const uint32_t* edges,
                                        size_t edge_count, size_t* out_dissolved);

/* Merges vertex `remove` into vertex `keep` (the "vertex-to-vertex line"
 * gesture: the line's start vertex snaps onto its end vertex). Faces
 * degenerated by the merge are deleted; with `at_midpoint` non-zero the
 * surviving vertex moves to the pair's midpoint, otherwise it stays at
 * `keep`'s position. Fails with CYBER_ERR_INVALID_ARG (mesh unchanged)
 * when either vertex is dead or keep == remove. */
CyberStatus cyber_retopo_merge_vertices(CyberMesh* mesh, uint32_t keep, uint32_t remove,
                                        int at_midpoint);

/* Rotates an interior edge (the "circle over an edge" gesture): a triangle
 * pair flips its shared diagonal; a quad pair re-splits one ring corner
 * over, turning the pair's loop-flow direction. Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) when the edge is dead, boundary,
 * or the rotation would fold the mesh. */
CyberStatus cyber_retopo_rotate_edge(CyberMesh* mesh, uint32_t edge);

/* ---- build tools (retopology phase 4, task 4.1) --------------------------
 *
 * Drag-from-topology creation ops behind the Build Quad / Build Triangle
 * tools (wrapping the engine's build_tools/actions operators). Same
 * contract as the ops above: snapper optional, render cache invalidated,
 * argument failures leave the mesh unchanged. */

/* Ring-slot sentinel for cyber_retopo_build_face: this slot creates a NEW
 * vertex from points_xyz instead of reusing an existing one. */
#define CYBER_BUILD_NEW_VERTEX 0xFFFFFFFFu

/* Builds ONE face over a mixed ring of existing vertices and new points
 * (Build Quad / Build Triangle, task 4.1): `vertex_ids[i]` is either a
 * live vertex id (reused — this is how the new face welds onto existing
 * topology) or CYBER_BUILD_NEW_VERTEX, in which case a new vertex is
 * created at points_xyz[i*3..] (snapped to the Target first when `snapper`
 * is non-NULL). `points_xyz` may be NULL when every slot reuses an
 * existing vertex. Writes the new face id to *out_face (may be NULL) and
 * the FINAL ring vertex ids — existing and newly created alike — to
 * out_ring_vertices[count] (may be NULL). Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) on bad counts (only 3 or 4
 * supported), dead ids, repeated ring vertices, or degenerate/coincident
 * ring positions after snapping. */
CyberStatus cyber_retopo_build_face(CyberMesh* mesh, size_t count,
                                    const uint32_t* vertex_ids,
                                    const float* points_xyz,
                                    const CyberSnapper* snapper,
                                    uint32_t* out_face,
                                    uint32_t* out_ring_vertices);

/* Grows the single triangle on a BOUNDARY edge into a quad (Build Quad's
 * triangle-edge drag, task 4.1): splits `edge` and drops the new ring
 * vertex at `point_xyz` (snapped to the Target first when `snapper` is
 * non-NULL). The face id is preserved. Writes the new vertex id to
 * *out_vertex (may be NULL). Fails with CYBER_ERR_INVALID_ARG (mesh
 * unchanged) when the edge is dead, interior, or its face is not a
 * triangle (growing a quad would leave an n-gon). */
CyberStatus cyber_retopo_grow_boundary_edge(CyberMesh* mesh, uint32_t edge,
                                            const float point_xyz[3],
                                            const CyberSnapper* snapper,
                                            uint32_t* out_vertex);

/* Path Distribute (task 4.1): repositions an ordered chain of live
 * vertices so they sit evenly (by arc length) along the chain's own
 * polyline; the two endpoints stay fixed and every moved vertex snaps to
 * the Target when `snapper` is non-NULL. `vertices` must be an ordered
 * chain of at least 3 DISTINCT live vertices where consecutive entries
 * are joined by live edges (e.g. a cyber_mesh_shortest_vertex_path
 * result). Positions only — topology is untouched. Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) on short/dead/broken chains. */
CyberStatus cyber_retopo_distribute_path(CyberMesh* mesh, const uint32_t* vertices,
                                         size_t count, const CyberSnapper* snapper);

/* Surface Cut (task 4.1): knife cut along the straight segment a -> b as
 * seen from `view_dir` (the cut plane spans the segment and the view
 * direction). Every edge crossing the plane WITHIN the segment's extent
 * is split (new vertices snapped to the Target when `snapper` is
 * non-NULL), every face then carrying two non-adjacent cut vertices is
 * split between them, and with `triangulate_ngons` non-zero every touched
 * face left with more than four sides is auto-triangulated (the spec's
 * n-gon rule). Writes the number of edges/faces split to
 * *out_split_edges / *out_split_faces (either may be NULL). Cutting
 * nothing is CYBER_OK with 0 counts. Fails with CYBER_ERR_INVALID_ARG
 * (mesh unchanged) on a degenerate segment or view direction. */
CyberStatus cyber_retopo_surface_cut(CyberMesh* mesh, const float a[3],
                                     const float b[3], const float view_dir[3],
                                     int triangulate_ngons,
                                     const CyberSnapper* snapper,
                                     size_t* out_split_edges,
                                     size_t* out_split_faces);

/* ---- camera-as-manipulator placement ops (task 4.2) ---------------------
 *
 * The Patch Clone / Extend Boundary / Draw Strip / Transform Vertices
 * mutations (retopo/build_tools.hpp + boundary.hpp). Affine transforms are
 * passed as 12 floats: the COLUMN-MAJOR 3x3 linear part (col0 xyz, col1
 * xyz, col2 xyz) followed by the translation (xyz) — the layout of
 * cyber::retopo::Affine. All follow the runMeshEdit contract: render cache
 * invalidated on success, argument failures leave the mesh unchanged. */

/* Patch Clone (task 4.2): duplicates `faces` (shared vertices cloned
 * once) transformed by `xf`, each new vertex snapped to the Target when
 * `snapper` is non-NULL. Non-zero `flip` reverses every cloned face's
 * winding (the flip option: a mirroring `xf` inverts orientation).
 * `out_new_faces` (may be NULL) receives the new face ids — it must hold
 * `face_count` entries; *out_new_face_count (may be NULL) receives the
 * count (== face_count on success). Fails with CYBER_ERR_INVALID_ARG
 * (mesh unchanged) on an empty list, dead ids, or repeated faces. */
CyberStatus cyber_retopo_patch_clone(CyberMesh* mesh, const uint32_t* faces,
                                     size_t face_count, const float xf[12], int flip,
                                     const CyberSnapper* snapper,
                                     uint32_t* out_new_faces,
                                     size_t* out_new_face_count);

/* Extend Boundary, quad strips (task 4.2): extrudes the ordered boundary
 * vertex chain by `offset` in `rings` successive rows of quads, each row
 * welded onto the previous (the first onto the chain itself) and snapped
 * to the Target when `snapper` is non-NULL. Non-zero `closed` treats the
 * chain as a closed loop (a wrap quad joins the last and first columns).
 * Quad winding is corrected against the existing face on the chain's
 * first edge. `out_outer_chain` (may be NULL, `count` entries) receives
 * the OUTERMOST ring's vertex ids in chain order — feed it back as the
 * next call's chain to stack rings with different offsets;
 * *out_new_faces (may be NULL) receives the number of new faces. Fails
 * with CYBER_ERR_INVALID_ARG (mesh unchanged) on chains shorter than 2,
 * dead/repeated vertices, consecutive vertices not joined by a live edge,
 * rings < 1, or a zero offset. */
CyberStatus cyber_retopo_extend_boundary_grid(CyberMesh* mesh, const uint32_t* chain,
                                              size_t count, int closed,
                                              const float offset[3], int rings,
                                              const CyberSnapper* snapper,
                                              uint32_t* out_outer_chain,
                                              size_t* out_new_faces);

/* Extend Boundary, triangle fan (task 4.2): closes the ordered boundary
 * chain onto a single apex at the chain centroid + `apex_offset` (snapped
 * to the Target when `snapper` is non-NULL) with a fan of triangles, one
 * per chain edge (plus the wrap edge when `closed`). Winding is corrected
 * against the existing face on the chain's first edge. Writes the apex
 * vertex id to *out_apex and the new face count to *out_new_faces (either
 * may be NULL). Chain validation matches
 * cyber_retopo_extend_boundary_grid. */
CyberStatus cyber_retopo_extend_boundary_fan(CyberMesh* mesh, const uint32_t* chain,
                                             size_t count, int closed,
                                             const float apex_offset[3],
                                             const CyberSnapper* snapper,
                                             uint32_t* out_apex,
                                             size_t* out_new_faces);

/* Draw Strip (task 4.2): a quad strip welded onto the boundary edge
 * start_a/start_b whose stations follow `path_xyz` (x,y,z per point —
 * world-space stroke samples, typically resampled at quad-size arc
 * length). Each station's rail pair spans `width` along
 * cross(view_dir, tangent) with sign continuity (curved strokes never
 * flip rails); rail vertices snap to the Target when `snapper` is
 * non-NULL; winding is corrected against the start edge's face. Writes
 * the number of new faces to *out_new_faces (may be NULL). Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) on an empty path, width <= 0, a
 * degenerate view direction, or when start_a/start_b are dead or not
 * joined by a live BOUNDARY edge. */
CyberStatus cyber_retopo_draw_strip(CyberMesh* mesh, const float* path_xyz,
                                    size_t point_count, float width,
                                    const float view_dir[3], uint32_t start_a,
                                    uint32_t start_b, const CyberSnapper* snapper,
                                    size_t* out_new_faces);

/* Transform Vertices (task 4.2): applies `xf` to every vertex in
 * `vertices` in place. When `snapper` is non-NULL each transformed vertex
 * is then reprojected onto the Target, and the RE-SNAP REPORT counts the
 * vertices whose reprojection moved them by more than `resnap_epsilon`
 * (*out_resnapped) with the maximum such distance (*out_max_distance;
 * either may be NULL). Fails with CYBER_ERR_INVALID_ARG (mesh unchanged)
 * on an empty list, dead ids, or repeated vertices. */
CyberStatus cyber_retopo_transform_vertices(CyberMesh* mesh, const uint32_t* vertices,
                                            size_t count, const float xf[12],
                                            const CyberSnapper* snapper,
                                            float resnap_epsilon,
                                            size_t* out_resnapped,
                                            float* out_max_distance);

/* ---- symmetry (retopology phase 4, task 4.4) ----------------------------
 *
 * Planar mirror symmetry (manual-retopology spec, "Multi-axis and radial
 * symmetry"). One plane per call: multi-axis symmetry is the caller
 * applying these ops once per enabled axis, which is exactly what mirroring
 * on X+Y+Z means geometrically and keeps the engine surface minimal.
 *
 * RADIAL symmetry has NO engine support: live radial authoring is the shell
 * replicating the authored operation per sector, and radial BAKE (which
 * needs seam welding at the sector boundaries) is deliberately not
 * implemented here rather than shipped tolerance-fragile. */

/* A mirror plane plus its weld/working-side policy. `normal` need not be
 * unit length but must be non-degenerate. `weld_tolerance` is the distance
 * within which a vertex counts as ON the plane (center-line vertices snap
 * onto it and are shared by both halves rather than duplicated).
 * `working_side_positive` selects which half is the authored one: the half
 * the plane normal points into (1) or away from (0). */
typedef struct CyberSymmetry {
    float origin[3];
    float normal[3];
    float weld_tolerance;
    int working_side_positive;
} CyberSymmetry;

/* Snaps every vertex within `weld_tolerance` of the plane exactly onto it
 * (the spec's "center-line vertices SHALL snap to the symmetry plane"),
 * writing the count to *out_snapped (may be NULL). Topology is untouched. */
CyberStatus cyber_retopo_snap_symmetry_plane(CyberMesh* mesh, const CyberSymmetry* symmetry,
                                             size_t* out_snapped);

/* Apply-symmetry: BAKES the mirror into real geometry. Every face lying
 * wholly on the working side gains a mirrored twin with reversed winding;
 * on-plane vertices weld to themselves so the seam stays manifold. Mirrored
 * vertices reproject onto the Target when `snapper` is non-NULL. Writes the
 * number of faces added to *out_added_faces (may be NULL). */
CyberStatus cyber_retopo_apply_symmetry(CyberMesh* mesh, const CyberSymmetry* symmetry,
                                        const CyberSnapper* snapper,
                                        size_t* out_added_faces);

/* Result of cyber_retopo_resymmetrize. */
typedef struct CyberResymmetrizeReport {
    /* Vertices welded onto the plane before matching. */
    uint32_t snapped;
    /* Off-side vertices moved exactly onto their mirror counterpart. */
    uint32_t matched;
    /* Off-side vertices with no counterpart within match_tolerance — the
     * "where it exists" clause: one-sided geometry survives untouched. */
    uint32_t unmatched;
    /* Largest displacement applied (0 when the mesh was already symmetric). */
    float max_correction;
} CyberResymmetrizeReport;

/* Re-symmetrize: mirrors the working half onto the other half IN PLACE.
 * Adds and removes nothing — topology correspondence is preserved exactly;
 * only positions of the non-working half move. `match_tolerance` is the
 * world-space radius within which an off-side vertex counts as the
 * counterpart of a working-side vertex's mirror image (<= 0 falls back to
 * weld_tolerance); callers scale it to the mesh so the verdict is
 * scale-free. *out_report may be NULL. */
CyberStatus cyber_retopo_resymmetrize(CyberMesh* mesh, const CyberSymmetry* symmetry,
                                      float match_tolerance,
                                      CyberResymmetrizeReport* out_report);

/* ---- EditMesh batch commands (retopology phase 4, task 4.5) -------------
 *
 * Whole-mesh commands (manual-retopology spec, "EditMesh batch commands"):
 * snap-all to Target, subdivide, subdivide+reproject and triangulate.
 * relax-all needs no new entry point — cyber_retopo_relax with radius <= 0
 * already relaxes the whole mesh honoring the same PinSet.
 *
 * ELEMENT-ID STABILITY (the contract callers MUST read):
 *
 *   - cyber_retopo_snap_all only moves positions: every vertex, edge and
 *     face id survives, so caller-side annotations keyed on stable ids
 *     (pins, loop tags, hidden faces) stay valid.
 *
 *   - cyber_retopo_subdivide REBUILDS THE MESH FROM SCRATCH (Mesh::
 *     linearSubdivide returns a new mesh). EVERY vertex, edge and face id
 *     is reassigned, so ALL caller-side annotations keyed on ids are
 *     orphaned and must be dropped or rebuilt by the caller. The handle's
 *     own hidden-face / tagged-edge render state is cleared here for the
 *     same reason.
 *
 *   - cyber_retopo_triangulate mutates IN PLACE (Mesh::splitFace per ear):
 *     vertex and edge ids survive and so do the ids of the faces it splits,
 *     but each n-gon gains NEW face ids for its extra triangles. Vertex and
 *     edge annotations therefore stay valid; FACE annotations (hidden
 *     faces) become partial, so the handle's hidden set is cleared while
 *     the tagged-edge set is kept. */

/* Projects every live vertex onto the Target surface (`snapper` must be
 * non-NULL — there is nothing to snap to otherwise). Vertices listed in
 * `pinned` are left exactly where they are (spec: pinned vertices are
 * immune to the smoothing/reprojection commands). Writes how many vertices
 * actually moved further than a hair (*out_moved) and the largest such
 * displacement (*out_max_distance); either may be NULL. */
CyberStatus cyber_retopo_snap_all(CyberMesh* mesh, const CyberSnapper* snapper,
                                  const uint32_t* pinned, size_t pinned_count,
                                  size_t* out_moved, float* out_max_distance);

/* Linear (Catmull-Clark topology, NO smoothing) subdivision into quads. The
 * engine has no smooth/limit-surface subdivision, so this is linear
 * subdivision plus optional REPROJECTION: when `snapper` is non-NULL every
 * vertex of the subdivided mesh is projected onto the Target surface, which
 * is what "subdivide+reproject" means (the reprojection is what recovers
 * curvature — linear subdivision alone only adds vertices along the
 * existing facets). Writes the resulting face count to *out_faces (may be
 * NULL). See the ELEMENT-ID STABILITY note above: all ids are reassigned. */
CyberStatus cyber_retopo_subdivide(CyberMesh* mesh, const CyberSnapper* snapper,
                                   size_t* out_faces);

/* Catmull-Clark SMOOTH subdivision (task 4.6a): same quad topology as
 * cyber_retopo_subdivide, but the original vertices and edge points move to
 * their limit-approaching positions, so a Target-less cage genuinely smooths
 * instead of merely densifying. When a snapper is given every resulting
 * vertex then reprojects onto the Target (smooth-then-conform). Writes the
 * resulting face count to *out_faces (may be NULL). ELEMENT-ID STABILITY:
 * all ids are reassigned, exactly as the linear variant. */
CyberStatus cyber_retopo_subdivide_smooth(CyberMesh* mesh, const CyberSnapper* snapper,
                                          size_t* out_faces);

/* Fan-triangulates every face with more than three sides. Writes the
 * resulting face count to *out_faces (may be NULL). See the ELEMENT-ID
 * STABILITY note above: vertex/edge ids survive, face ids partially. */
CyberStatus cyber_retopo_triangulate(CyberMesh* mesh, size_t* out_faces);

/* ---- gesture stroke interpretation (retopology phase 3, design D5) ------
 *
 * Two-stage recognizer: a cheap geometric shape classifier over the raw
 * screen-space stroke polyline, then a mesh-context resolver against the
 * EditMesh. Produces an INTERPRETATION RECORD — ranked candidate actions
 * with confidences and the mesh elements each would touch. Read-only: it
 * never mutates the mesh handle. */

/* Stage-1 shape classes. */
typedef enum CyberStrokeShape {
    CYBER_SHAPE_UNKNOWN = 0,
    CYBER_SHAPE_HOLD_POINT,  /* stationary press (tap/hold) */
    CYBER_SHAPE_LINE,        /* open, straight */
    CYBER_SHAPE_CLOSED_LOOP, /* closed polygon with corners (quad draw) */
    CYBER_SHAPE_CIRCLE,      /* closed, round */
    CYBER_SHAPE_SCRIBBLE,    /* open, many reversals/self-crossings */
    CYBER_SHAPE_CROSS,       /* an X drawn in one stroke */
    CYBER_SHAPE_LASSO,       /* closed, irregular */
    CYBER_SHAPE_GRID         /* open square wave (one-stroke grid) */
} CyberStrokeShape;

/* What the resolver found under the stroke. */
typedef enum CyberStrokeContext {
    CYBER_CONTEXT_EMPTY_SURFACE = 0,
    CYBER_CONTEXT_FACE,
    CYBER_CONTEXT_EDGE,
    CYBER_CONTEXT_BOUNDARY_EDGE,
    CYBER_CONTEXT_VERTEX
} CyberStrokeContext;

/* Candidate actions of the gesture grammar (interpretation only; applying
 * an action is a separate, journaled mutation path). */
typedef enum CyberStrokeAction {
    CYBER_ACTION_NONE = 0,
    CYBER_ACTION_CREATE_QUAD,
    CYBER_ACTION_INSERT_LOOP,
    CYBER_ACTION_TAG_LOOP,
    CYBER_ACTION_DISSOLVE_EDGE,
    CYBER_ACTION_DELETE_FACES,
    CYBER_ACTION_MERGE_VERTICES,
    CYBER_ACTION_ROTATE_EDGE,
    CYBER_ACTION_TWEAK_VERTEX,
    CYBER_ACTION_HIDE_REGION,
    CYBER_ACTION_TOGGLE_VISIBILITY,
    CYBER_ACTION_CREATE_GRID,
    /* Appended (not inserted) so existing action values keep their numbers. */
    CYBER_ACTION_CREATE_TRIANGLE,
    /* Bridge the two open rims a gap-crossing stroke connects. The record's
     * elements are the two corresponding rim VERTICES, not the extent of the
     * fill: the caller walks both rims outward from that pair. */
    CYBER_ACTION_BRIDGE_RIMS
} CyberStrokeAction;

/* Kind of a referenced mesh element. */
typedef enum CyberElementKind {
    CYBER_ELEMENT_VERTEX = 0,
    CYBER_ELEMENT_EDGE,
    CYBER_ELEMENT_FACE
} CyberElementKind;

/* Opaque interpretation record. Free with cyber_stroke_interpretation_free. */
typedef struct CyberStrokeInterpretation CyberStrokeInterpretation;

/* Interprets one completed stroke.
 *
 *   edit_mesh    the EditMesh to resolve context against, or NULL (stage 1
 *                still runs; every context rule sees an empty scene).
 *   view_proj    column-major 4x4 world->clip matrix (simd_float4x4 memory
 *                order); may be NULL only when edit_mesh is NULL.
 *   samples_xyt  x,y,t triplets: normalized viewport coordinates (0..1,
 *                origin top-left) and seconds since the stroke began.
 *   sample_count number of triplets (>= 1).
 *   aspect       viewport width/height (0 or negative means 1) so circles
 *                and angles are measured undistorted.
 *
 * Deterministic: identical inputs produce identical records. */
CyberStatus cyber_stroke_interpret(const CyberMesh* edit_mesh, const float* view_proj,
                                   const float* samples_xyt, size_t sample_count,
                                   float aspect, CyberStrokeInterpretation** out);

void cyber_stroke_interpretation_free(CyberStrokeInterpretation* interpretation);

/* Stage-1 result: shape class and its heuristic confidence (0..1). */
CyberStrokeShape cyber_stroke_interpretation_shape(
    const CyberStrokeInterpretation* interpretation);
float cyber_stroke_interpretation_shape_confidence(
    const CyberStrokeInterpretation* interpretation);

/* Stage-2 under-stroke context. */
CyberStrokeContext cyber_stroke_interpretation_context(
    const CyberStrokeInterpretation* interpretation);

/* Ranked candidates, best first. Index 0 is the chosen interpretation; the
 * rest are the one-tap alternatives. */
size_t cyber_stroke_interpretation_candidate_count(
    const CyberStrokeInterpretation* interpretation);
CyberStrokeAction cyber_stroke_interpretation_action(
    const CyberStrokeInterpretation* interpretation, size_t candidate);
float cyber_stroke_interpretation_confidence(
    const CyberStrokeInterpretation* interpretation, size_t candidate);

/* Mesh elements candidate `candidate` would touch, in deterministic order.
 * Element ids are engine element ids (see CYBER_INVALID_ID note above).
 * Returns 1 and fills out_kind/out_id (either may be NULL) when the indices
 * are in range, else 0. */
size_t cyber_stroke_interpretation_element_count(
    const CyberStrokeInterpretation* interpretation, size_t candidate);
int cyber_stroke_interpretation_element(const CyberStrokeInterpretation* interpretation,
                                        size_t candidate, size_t element,
                                        CyberElementKind* out_kind, uint32_t* out_id);

/* Estimated corner points of a CLOSED stroke, in normalized viewport
 * coordinates (0..1, origin top-left), ordered as a simple ring — the quad
 * the shell should build when applying a CREATE_QUAD candidate (the shell
 * unprojects them onto the Target). Exactly 4 corners for closed shapes
 * (detected stroke corners when the stroke reads as a quad, a stable
 * inscribed quad otherwise); 0 for open shapes. Returns 1 and fills
 * out_xy when `corner` is in range, else 0. */
size_t cyber_stroke_interpretation_corner_count(
    const CyberStrokeInterpretation* interpretation);
int cyber_stroke_interpretation_corner(const CyberStrokeInterpretation* interpretation,
                                       size_t corner, float out_xy[2]);

/* Quad-cell counts of a GRID stroke's estimated lattice (task 3.4): the
 * corner list then holds (rows + 1) * (cols + 1) lattice points, row-major.
 * Returns 1 and fills out_rows/out_cols (either may be NULL) for a grid
 * stroke, else 0. */
int cyber_stroke_interpretation_grid_size(const CyberStrokeInterpretation* interpretation,
                                          size_t* out_rows, size_t* out_cols);

/* ---- surface baking (group 11) --------------------------------------- */

/* Bakeable map types (surface-baking spec). */
typedef enum CyberBakeMap {
    CYBER_BAKE_NORMAL = 0,    /* tangent-space normal map (RGB, encoded [0,1]) */
    CYBER_BAKE_AO,            /* ambient occlusion / openness (1 channel) */
    CYBER_BAKE_DISPLACEMENT,  /* signed height along the low-poly normal (1 ch) */
    CYBER_BAKE_POSITION,      /* Target hit position, world space (RGB) */
    CYBER_BAKE_COLOR          /* Target vertex color at the hit (RGB) */
} CyberBakeMap;

typedef struct CyberBakeParams {
    int width;           /* output resolution */
    int height;
    float cageDistance;  /* rays start at surface + normal*cageDistance, cast inward 2x */
    int aoSamples;       /* hemisphere rays per texel for AO */
    float aoRadius;      /* an AO ray hit beyond this does not occlude */
} CyberBakeParams;

/* Fills params with the engine defaults. No-op on NULL. */
void cyber_default_bake_params(CyberBakeParams* params);

/* Opaque baked image: row-major float pixels, `channels` per texel. Release
 * with cyber_image_free. */
typedef struct CyberImage CyberImage;

/* Bakes `map` from `high` (the Target / high-poly) onto the per-corner UV
 * layout of `low` (the EditMesh / low-poly). `low` MUST carry UVs (load an OBJ
 * with vt coordinates). On success *out receives a new CyberImage. */
CyberStatus cyber_bake(const CyberMesh* low, const CyberMesh* high, CyberBakeMap map,
                       const CyberBakeParams* params, CyberImage** out);

void cyber_image_free(CyberImage* image);
int cyber_image_width(const CyberImage* image);
int cyber_image_height(const CyberImage* image);
int cyber_image_channels(const CyberImage* image);
/* Copies the row-major float pixels (width*height*channels) into `out`, at most
 * `max_floats`; returns the number of floats written. Pass out=NULL to query
 * the required count. */
size_t cyber_image_copy_pixels(const CyberImage* image, float* out, size_t max_floats);
/* Writes the image to an 8-bit PNG (tonemapped). */
CyberStatus cyber_image_save_png(const CyberImage* image, const char* path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CYBER_CAPI_H */
