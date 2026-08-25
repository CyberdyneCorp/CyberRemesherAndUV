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
    CYBER_ERR_CANCELLED,     /* the operation was cooperatively cancelled */
    /* A versioned data file declares a version this engine does not support.
     * Distinct from a parse error: the file is well-formed, the CONTRACT does
     * not match. Appended in 0.6.0 — existing values keep their numbers. */
    CYBER_ERR_INCOMPATIBLE_VERSION,
    /* The mesh is well-formed but its topology is not what the operation is
     * defined on — Loop subdivision handed a quad, say. Distinct from
     * CYBER_ERR_INVALID_PARAM: nothing the caller passed is wrong, the
     * GEOMETRY is unsuitable, and the fix is a mesh edit (triangulate) rather
     * than a different argument. cyber_last_error() names the offending
     * element. Appended after 0.6.0 — existing values keep their numbers. */
    CYBER_ERR_UNSUPPORTED_TOPOLOGY
} CyberStatus;

/* Engine semantic version (mirrors the CMake project() version). */
void cyber_version(int* major, int* minor, int* patch);

/* Human-readable, static string for a status code. Never NULL. */
const char* cyber_status_string(CyberStatus status);

/* Thread-local description of the most recent failure on the calling thread,
 * or an empty string if the last call succeeded. Never NULL. The pointer is
 * valid until the next capi call on this thread. */
const char* cyber_last_error(void);

/* ---- compute backend ------------------------------------------------- */

/* Compute backends the engine can run its accelerated primitives on. Which
 * ones exist depends on how the library was BUILT (a CPU-only build compiles
 * none of the device backends) and on what the machine has at RUNTIME.
 * CYBER_BACKEND_AUTO is not a device: it is the automatic best-first choice,
 * the state the process starts in. Appended in 0.6.0. */
typedef enum CyberBackend {
    CYBER_BACKEND_AUTO = 0,
    CYBER_BACKEND_CPU = 1,
    CYBER_BACKEND_METAL = 2,
    CYBER_BACKEND_CUDA = 3,
    CYBER_BACKEND_OPENCL = 4
} CyberBackend;

/* Fills `out` with the backends this build compiled in AND detected on this
 * machine, best first, and returns their TOTAL count (which may exceed
 * max_backends; pass out=NULL with max_backends=0 to query the count).
 * CYBER_BACKEND_CPU is always present and always last, so the result is never
 * empty. CYBER_BACKEND_AUTO is never reported — it is not a device. */
size_t cyber_available_backends(CyberBackend* out, size_t max_backends);

/* Selects the process-wide backend for every later call. CYBER_BACKEND_AUTO
 * restores the automatic best-first choice (also honouring the CYBER_BACKEND
 * environment variable). Selecting a backend this build or this machine does
 * not have is reported as CYBER_ERR_INVALID_ARG and leaves the current
 * selection untouched, so a host never silently runs somewhere it did not
 * ask for; call cyber_available_backends first to offer a real choice.
 *
 * The selection is global, not per-handle: change it between operations, not
 * while one is running on another thread. */
CyberStatus cyber_set_backend(CyberBackend backend);

/* The backend calls run on right now — a concrete device, never
 * CYBER_BACKEND_AUTO, since AUTO always resolves to one. */
CyberBackend cyber_active_backend(void);

/* Human-readable name of the active backend's device ("CPU", the GPU model
 * string, ...). Never NULL; valid until the next call on this thread. */
const char* cyber_active_backend_name(void);

/* ---- worker threads --------------------------------------------------- */

/* Caps the worker threads any one parallel loop inside the engine may fan out
 * to. 0 (the default) means no cap: the loops size themselves from the
 * machine's hardware concurrency, which saturates every core — right for a
 * batch run, wrong inside an interactive app, where an uncapped bake fights the
 * host's own scheduler and drains a mobile device's battery.
 *
 * This is an API and not an environment variable because a host learns its
 * thread budget while it is already running, and setenv() in a live
 * multi-threaded process is not safe.
 *
 * Results do not depend on the cap: the loops split a range into contiguous
 * chunks and each chunk writes only its own indices, so a capped run produces
 * byte-identical output to an uncapped one. Only speed and CPU load change.
 *
 * Global, not per-handle, and safe to call from any thread at any time; loops
 * already running keep the fan-out they started with. Negative values are
 * rejected with CYBER_ERR_INVALID_ARG and leave the cap untouched. */
CyberStatus cyber_set_max_worker_threads(int max_threads);

/* The current cap, or 0 when none is set. */
int cyber_max_worker_threads(void);

/* ---- mesh I/O -------------------------------------------------------- */

/* Loads a mesh, dispatching on the file extension: .obj, .ply, .stl, .gltf,
 * .glb, .fbx. On success *out receives a newly allocated handle the caller
 * must release with cyber_mesh_free. An unknown extension fails with
 * CYBER_ERR_IO and cyber_last_error names the path. */
CyberStatus cyber_mesh_load(const char* path, CyberMesh** out);

/* Writes a mesh, dispatching on the file extension: .obj (a sibling .mtl may
 * be written too), .ply, .stl, .gltf, .glb. FBX is IMPORT-ONLY — writing it
 * needs the proprietary binary container, which no permissively licensed
 * library produces — so a .fbx path fails with CYBER_ERR_IO, and
 * cyber_last_error names the formats that can be written. */
CyberStatus cyber_mesh_save(const CyberMesh* mesh, const char* path);

/* Aliases kept for callers written before the loaders grew past OBJ. Despite
 * the names these have always dispatched on the extension; they behave exactly
 * as cyber_mesh_load / cyber_mesh_save. Prefer the names above. */
CyberStatus cyber_mesh_load_obj(const char* path, CyberMesh** out);
CyberStatus cyber_mesh_save_obj(const CyberMesh* mesh, const char* path);

/* Releases a handle. NULL is ignored. */
void cyber_mesh_free(CyberMesh* mesh);

/* ---- remeshing ------------------------------------------------------- */

/* Canonical user-facing remesh parameters (POD mirror of the C++ struct).
 * Fill with cyber_default_params, then override fields as needed. */
typedef struct CyberRemeshParams {
    int targetQuads;           /* desired output quad count */
    float edgeScale;           /* multiplier on the derived edge length */
    float sharpEdgeDegrees;    /* dihedral threshold for feature edges: drives both
                                * the isotropic stage and the parameterization */
    float smoothNormalDegrees; /* normal-smoothing angle */
    float adaptivity;          /* 0 uniform .. 1 fully curvature-adaptive; the
                                * quad-cover extractor stays uniform by design */
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

/* ---- guided remeshing (flow guides + painted density) ---------------- */

/* One flow guide: an ordered polyline on or near the input surface whose
 * tangent the cross field is softly biased toward. `points` is
 * 3 * point_count floats (x,y,z per point) and needs at least 2 points.
 * `strength` is clamped to [0,1]; `radius` is the world-space influence
 * radius and MUST be > 0 (a zero-radius guide could never be honored, so it
 * is rejected rather than silently ignored). */
typedef struct CyberFlowGuide {
    const float* points;
    size_t point_count;
    float strength;
    float radius;
} CyberFlowGuide;

/* Painted guidance. Supply at most ONE of vertex_density / face_density; its
 * length must match the input mesh's vertex or face count exactly. Values are
 * quads-per-unit-area multipliers clamped to [0.25, 4.0], so the local target
 * edge length becomes base / sqrt(density). */
typedef struct CyberGuidance {
    const CyberFlowGuide* guides;
    size_t guide_count;
    const float* vertex_density;
    size_t vertex_density_count;
    const float* face_density;
    size_t face_density_count;
} CyberGuidance;

/* Guidance warning channel: called once per clamp, per rejection, and per
 * island whose backend could not honor the supplied guidance. This is the
 * ABI's "loud" path — guidance is never silently dropped. */
typedef void (*CyberWarningCb)(const char* message, void* user);

/* Runs the pipeline with user guidance. Identical to cyber_remesh when
 * `guidance` is NULL (or carries neither guides nor density), including
 * byte-for-byte output. `warning` may be NULL, in which case the messages are
 * simply not delivered (fatal problems still fail the call).
 * Returns CYBER_ERR_INVALID_PARAM when a guide or density array is unusable;
 * *out is left NULL in that case. */
CyberStatus cyber_remesh_guided(const CyberMesh* in, const CyberRemeshParams* params,
                                const CyberGuidance* guidance, CyberProgressCb progress,
                                CyberCancelCb cancel, CyberWarningCb warning, void* user,
                                CyberMesh** out);

/* ---- isotropic (triangle) remeshing ---------------------------------- */

/* Adaptive isotropic remeshing parameters (POD mirror of the useful subset of
 * cyber::remesh::IsotropicOptions). Fill with cyber_default_isotropic_params,
 * then override.
 *
 * The painted-density field IsotropicOptions carries is deliberately NOT
 * mirrored here. A density paint is indexed against the mesh it was authored
 * on, and this entry point triangulates before it remeshes — so the array
 * would need its own pointer/count validation surface plus a statement about
 * which index domain it lives in (cyber_remesh_guided's CyberGuidance is that
 * surface, for the full pipeline). Painted density therefore stays reachable
 * only through cyber_remesh_guided until it gets that treatment here. */
typedef struct CyberIsotropicParams {
    float targetEdgeLength;    /* world-space edge length the mesh converges
                                * to; REQUIRED, must be finite and > 0 */
    int iterations;            /* split/collapse/flip/smooth rounds; > 0 */
    float adaptivity;          /* 0 uniform .. 1 fully curvature-adaptive */
    float smoothNormalDegrees; /* 0: flat closest-point projection. > 0:
                                * PN-triangle (curved) projection, with this
                                * as the crease threshold, so relaxed vertices
                                * follow the smooth surface instead of sinking
                                * onto coarse facets */
    float sharpEdgeDegrees;    /* dihedral threshold used to tag feature edges
                                * BEFORE remeshing, so creases and boundaries
                                * survive it. <= 0 skips the tagging pass and
                                * honors whatever the mesh already carries */
} CyberIsotropicParams;

/* Fills params with the engine defaults. No-op on NULL.
 *
 * targetEdgeLength is filled with 0, which cyber_mesh_isotropic_remesh
 * REJECTS: a world-space length has no scale-free default, so the caller has
 * to pick one against its own mesh (a bounding-box diagonal / N is the usual
 * choice). This is the one field the filler cannot answer for you. */
void cyber_default_isotropic_params(CyberIsotropicParams* params);

/* Adaptive isotropic remesh of `mesh`, IN PLACE. Per iteration: split long
 * edges, collapse short ones, flip toward valence 6, tangentially smooth and
 * project back onto the input surface. Edge lengths converge to
 * [4/5, 4/3] * the (adaptivity-scaled) target, so a target BELOW the mesh's
 * current edge length densifies it and one above decimates it — this is the
 * "give me more triangles to work with" operation, and the only way to reach
 * the isotropic stage without running the whole quad pipeline.
 *
 * Everything the C++ entry point makes the caller assemble is done here, so
 * this is a one-call operation:
 *
 *   - NON-TRIANGULATED INPUT IS TRIANGULATED, NOT REJECTED. The engine's
 *     isotropicRemesh requires triangles; rejecting a quad mesh would make
 *     the most obvious call (densify what cyber_remesh just produced) fail
 *     for a reason the caller can only fix by calling
 *     cyber_retopo_triangulate first. So the entry point fan-triangulates
 *     every face with more than three sides itself. The output is a TRIANGLE
 *     mesh either way: this operation does not preserve quads and cannot.
 *   - The projection reference is built from the input surface (after that
 *     triangulation, before any remeshing), which is what keeps the result on
 *     the shape you handed in rather than on a mesh drifting under its own
 *     smoothing.
 *   - Feature edges are tagged from CyberIsotropicParams.sharpEdgeDegrees;
 *     tagged edges are never collapsed, flipped, smoothed off or projected.
 *
 * Writes the resulting face count to *out_faces (may be NULL). `params` must
 * be non-NULL — there is no default target edge length to fall back on.
 *
 * ELEMENT-ID STABILITY (see the retopo block below for the full contract):
 * the passes add, remove and rewire elements freely, so EVERY vertex, edge
 * and face id is meaningless afterwards and all caller-side annotations keyed
 * on ids must be dropped. The handle's own hidden-face / tagged-edge / soft-
 * selection state is cleared here for that reason.
 *
 * COST: the call is uninterruptible — there is no cancellable twin yet — and
 * a target edge length far below the input's runs several extra convergence
 * iterations by design. Halving the target roughly quadruples the face count;
 * size the target before calling rather than expecting to abort.
 *
 * Fails with CYBER_ERR_INVALID_ARG on a NULL mesh or NULL params,
 * CYBER_ERR_EMPTY when the mesh has no faces, and CYBER_ERR_INVALID_PARAM
 * when targetEdgeLength is not finite and positive or iterations is < 1. All
 * three are decided before anything is touched, so on them the mesh is left
 * exactly as it was; a CYBER_ERR_RUNTIME can only come from the remesh itself
 * and leaves a partially rewritten mesh, like every other editing op here. */
CyberStatus cyber_mesh_isotropic_remesh(CyberMesh* mesh, const CyberIsotropicParams* params,
                                        size_t* out_faces);

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
                                 * (0 disables the second pass). NON-ZERO IS
                                 * EXPENSIVE: the pass trial-unwraps candidate
                                 * chart unions, which dominates the call and
                                 * runs for minutes on meshes of tens of
                                 * thousands of faces. 0 keeps it in the
                                 * milliseconds. */
} CyberAtlasParams;

/* Aggregate atlas quality/packing report. */
typedef struct CyberAtlasResult {
    int chartCount;           /* charts that occupy area in the packed atlas */
    size_t seamEdges;         /* edges cut to form the charts */
    float maxAngleDistortion; /* worst conformal error across charts, [0,1) */
    float rmsAngleDistortion; /* RMS conformal error across charts */
    int flippedCharts;        /* charts with mirrored net UV winding */
    int fallbackCharts;       /* charts unwrapped by planar-projection fallback */
    float packedArea;         /* fraction of the unit square the chart GEOMETRY
                               * covers, i.e. the summed UV face areas */
    float texelDensity;       /* texels per UV unit at the packed scale */
    int droppedCharts;        /* degenerate islands that cover nothing;
                               * chartCount + droppedCharts = island count */
    float packedBoxArea;      /* fraction covered by the charts' bounding boxes
                               * (packer tightness) */
} CyberAtlasResult;

/* Fills params with the engine defaults. No-op on NULL. */
void cyber_default_atlas_params(CyberAtlasParams* params);

/* Automatic UV atlas: seams `mesh` into normal-coherent charts, LSCM-unwraps
 * each chart, packs them into the unit square and writes the per-corner UV
 * attribute IN PLACE (so a subsequent cyber_mesh_save_obj emits vt / f v/vt).
 * `params` may be NULL (defaults); `out` may be NULL. Returns CYBER_ERR_RUNTIME
 * when the engine was built without the UV module. */
CyberStatus cyber_uv_atlas(CyberMesh* mesh, const CyberAtlasParams* params, CyberAtlasResult* out);

/* Same as cyber_uv_atlas, plus cooperative progress and cancellation.
 *
 * USE THIS ONE FROM ANY INTERACTIVE HOST. With the default params the atlas
 * runs a chart-merge pass that trial-unwraps the union of candidate chart
 * pairs (CyberAtlasParams.maxChartDistortion); on a mesh of tens of thousands
 * of faces that pass runs for minutes, and cyber_uv_atlas offers no way out of
 * it. Setting maxChartDistortion to 0 bounds the unwrap to milliseconds
 * instead, at the cost of more charts.
 *
 * On a cooperative cancel the status is CYBER_ERR_CANCELLED and `mesh` IS
 * LEFT EXACTLY AS IT WAS — cancellation is observed before any UV is written,
 * so no partial atlas is ever handed back. Cancel latency is one trial unwrap.
 * Either callback may be NULL; `user` is passed through to both. */
CyberStatus cyber_uv_atlas_cancellable(CyberMesh* mesh, const CyberAtlasParams* params,
                                       CyberProgressCb progress, CyberCancelCb cancel, void* user,
                                       CyberAtlasResult* out);

/* --- Accessors used by the language bindings (Python, Swift) ------------- */

/* Creates an empty mesh handle (release with cyber_mesh_destroy/free). */
CyberMesh* cyber_mesh_create(void);
/* Alias of cyber_mesh_free (the bindings use the create/destroy pairing). */
void cyber_mesh_destroy(CyberMesh* mesh);

/* Deep-copies `mesh` into a fresh independent handle written to *out (release
 * with cyber_mesh_destroy/free). The copy is exact and in-memory — no float
 * narrowing, unlike a save/load round trip — and carries the whole handle:
 * geometry, the remesh statistics, the hidden-face and tagged-edge overlays,
 * the soft-selection weight field and its saved slots.
 *
 * ELEMENT IDS ARE PRESERVED, so a vertex/edge/face id means the same thing in
 * both handles and any id-keyed caller state stays valid against the copy —
 * which is what makes this usable as an undo snapshot or a before/after
 * baseline. The render cache is not copied; the copy rebuilds it lazily.
 * Fails with CYBER_ERR_INVALID_ARG on a null mesh or a null `out`. */
CyberStatus cyber_mesh_clone(const CyberMesh* mesh, CyberMesh** out);

/* Number of live vertices / faces. */
size_t cyber_mesh_vertex_count(const CyberMesh* mesh);
size_t cyber_mesh_face_count(const CyberMesh* mesh);
/* Copies compacted vertex positions (x,y,z per vertex) into `out`, writing at
 * most `max_floats`; returns the number of floats written. Pass out=NULL to
 * query the required count (3 * vertex_count).
 *
 * EVERY live vertex, in id order — this stream is NOT filtered by
 * cyber_mesh_set_hidden_faces, because it is the one
 * cyber_mesh_set_positions writes back. The render accessors below use the
 * narrower RENDER VERTEX ORDER, which differs while anything is hidden; mixing
 * the two silently pairs positions with the wrong vertices, so a renderer
 * wants cyber_mesh_copy_render_positions instead. */
size_t cyber_mesh_copy_positions(const CyberMesh* mesh, float* out, size_t max_floats);

/* Writes vertex positions back, the exact inverse of
 * cyber_mesh_copy_positions: `positions` holds x,y,z per vertex in the SAME
 * compacted order (live vertices in id order) and `float_count` must equal
 * 3 * vertex_count exactly — a partial write would silently pair positions
 * with the wrong vertices, so a mismatch is rejected instead.
 *
 * Positions only: topology, ids and every overlay are untouched, so this
 * restores a snapshot taken with cyber_mesh_copy_positions as long as no
 * vertex was added or removed in between. Invalidates the render cache and
 * its pointer views. Fails with CYBER_ERR_INVALID_ARG (mesh unchanged) on a
 * null mesh, a null `positions` with a non-zero count, or a count mismatch. */
CyberStatus cyber_mesh_set_positions(CyberMesh* mesh, const float* positions, size_t float_count);
/* Alias of cyber_default_params. */
void cyber_remesh_params_default(CyberRemeshParams* params);

/* --- Render-data accessors (viewport renderers) --------------------------
 *
 * Everything below reads from a per-handle compacted render cache built
 * lazily on first access:
 *   - RENDER VERTEX ORDER: live, VISIBLE vertices in id order — a vertex
 *     used exclusively by faces hidden with cyber_mesh_set_hidden_faces is
 *     dropped. Positions, normals, colors, triangle and edge indices all
 *     share it, so they are mutually consistent. It matches
 *     cyber_mesh_copy_positions exactly while nothing is hidden, and is
 *     shorter once something is: take the positions of this order from
 *     cyber_mesh_copy_render_positions (or cyber_mesh_positions_ptr), never
 *     from cyber_mesh_copy_positions;
 *   - faces are fan-triangulated deterministically around their first
 *     corner: a live face (v0 v1 ... vn-1), taken in id order, emits
 *     (v0,v1,v2), (v0,v2,v3), ... — an n-gon yields n-2 triangles, so a
 *     quad mesh yields exactly 2 triangles per quad, and repeated calls on
 *     an unchanged handle return identical buffers.
 *
 * These accessors are not thread-safe with each other on the same handle
 * (the first call builds the cache); serialize per-handle access. */

/* Copies the positions of the RENDER VERTEX ORDER (x,y,z per visible
 * vertex) into `out`, at most `max_floats`; returns the number of floats
 * written. This is the position stream the buffers below index — the
 * visibility-filtered sibling of cyber_mesh_copy_positions, which stays
 * unfiltered for cyber_mesh_set_positions. Pass out=NULL to query the
 * required count. */
size_t cyber_mesh_copy_render_positions(const CyberMesh* mesh, float* out, size_t max_floats);

/* Number of triangles the live faces fan-triangulate to (an n-gon counts
 * n-2). Returns 0 on NULL. */
size_t cyber_mesh_triangle_count(const CyberMesh* mesh);

/* Copies the triangulated index buffer (3 uint32 per triangle, indexing the
 * render vertex order) into `out`, writing
 * whole triangles only, at most `max_indices` values; returns the number of
 * indices written. Pass out=NULL to query the required count
 * (3 * triangle_count). */
size_t cyber_mesh_copy_triangle_indices(const CyberMesh* mesh, uint32_t* out, size_t max_indices);

/* Copies per-vertex unit normals (x,y,z per vertex, render vertex order)
 * into `out`, at most `max_floats`; returns the number of floats written.
 * When the mesh carries imported per-corner normals they are averaged per
 * vertex; otherwise normals are computed engine-side from face normals
 * (Newell). A vertex with no usable normal (isolated, degenerate) gets
 * (0,0,1). Pass out=NULL to query the required count (3 * vertex_count). */
size_t cyber_mesh_copy_normals(const CyberMesh* mesh, float* out, size_t max_floats);

/* Non-zero when the mesh carries per-vertex colors (e.g. an OBJ with
 * "v x y z r g b" polypaint data). */
int cyber_mesh_has_colors(const CyberMesh* mesh);

/* Copies per-vertex linear RGB colors (r,g,b per vertex, render vertex
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

/* Copies the unique-edge index buffer (2 uint32 per edge, indexing the
 * render vertex order) into `out`, writing
 * whole edges only, at most `max_indices` values; returns the number of
 * indices written. Pass out=NULL to query the required count
 * (2 * edge_count). */
size_t cyber_mesh_copy_edge_indices(const CyberMesh* mesh, uint32_t* out, size_t max_indices);

/* ---- zero-copy render buffer views ------------------------------------
 *
 * Direct pointers into the handle's internal render cache — no copy. On
 * Apple unified memory a renderer may hand these straight to the GPU (e.g.
 * memcpy-free MTLBuffer setup or bounded per-frame uploads).
 *
 * LIFETIME: a returned pointer remains valid until the mesh handle is
 * mutated by any mutating C API call or released with
 * cyber_mesh_free/cyber_mesh_destroy, whichever comes first. The mutating
 * entry points are the cyber_retopo_* editing ops below plus
 * cyber_mesh_set_positions and cyber_conform; every one of them
 * invalidates the render cache, so pointer views obtained before the call
 * are dead afterwards and must be re-fetched (the next accessor refreshes
 * the cache lazily). The compacted vertex ORDER also changes when vertices
 * are added or removed — anything keyed on compacted render indices is
 * stale after a mutation; address elements by their stable engine ids.
 * The pointers must not outlive the handle and must not be written
 * through.
 *
 * COST OF RE-FETCHING: an edit that only MOVES vertices — the drag path:
 * cyber_mesh_set_positions, tweak/move/relax, transform_vertices,
 * distribute_path, snap_all, snap_symmetry_plane, the weighted
 * selection_transform/selection_relax, and cyber_conform — cannot change the
 * render vertex order or any index buffer, so re-fetching after one only
 * recomputes the positions and normals. Re-fetching after an edit that adds,
 * removes or rewires elements rebuilds every buffer. Either way the rule for
 * the caller is the same: re-fetch, never reuse.
 *
 * Each accessor stores the element count (floats / indices) in *out_count
 * (ignored when NULL) and returns NULL with *out_count = 0 when the mesh is
 * NULL, empty, or — for colors — carries no per-vertex colors. */
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
 * elements; ids that are dead at build time are skipped silently, and an
 * id an editing op kills is dropped from these tables as the op completes,
 * so a recycled id is never born hidden or tagged. */

/* Stable ids of every live face, in id order (the universe the visibility
 * gestures complement against — "invert visibility" hides exactly the
 * faces not currently hidden). copy_positions convention: returns the
 * TOTAL live-face count and fills at most max_faces entries of out_faces
 * (which may be NULL when max_faces is 0, for size queries). */
size_t cyber_mesh_live_faces(const CyberMesh* mesh, uint32_t* out_faces, size_t max_faces);

/* Replaces the hidden-face set (faces may be NULL when face_count is 0 —
 * show all). Hidden faces are dropped from the triangle and edge streams,
 * and vertices used exclusively by hidden faces leave the render vertex
 * order, so the normal, color and cyber_mesh_copy_render_positions streams
 * shorten with it. cyber_mesh_copy_positions is unaffected: it keeps every
 * live vertex, for cyber_mesh_set_positions. */
CyberStatus cyber_mesh_set_hidden_faces(CyberMesh* mesh, const uint32_t* faces, size_t face_count);

/* Number of face ids currently in the hidden set. Ids killed by an editing
 * op are dropped as that op completes (see ELEMENT-ID STABILITY), so the
 * count only ever covers ids that were live at the last mutation. 0 for
 * NULL. */
size_t cyber_mesh_hidden_face_count(const CyberMesh* mesh);

/* Replaces the tagged-edge list (edges may be NULL when edge_count is 0 —
 * clear). Tagged edges surface through the accessor below for the
 * overlay's colored loop-tag pass. */
CyberStatus cyber_mesh_set_tagged_edges(CyberMesh* mesh, const uint32_t* edges, size_t edge_count);

/* Compacted index pairs (2 per tagged, live, visible edge) in tag order —
 * same LIFETIME contract as the pointer views above. NULL with
 * *out_count = 0 when the mesh is NULL or nothing is tagged. */
const uint32_t* cyber_mesh_tagged_edge_indices_ptr(const CyberMesh* mesh, size_t* out_count);

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
int cyber_snapper_snap_to_vertex(const CyberSnapper* snapper, const float query[3], float radius,
                                 float out_point[3], uint32_t* out_vertex);

/* First Target surface hit along the ray origin + t*direction, t in
 * (0, max_distance]. `direction` need not be normalized (it is normalized
 * internally; a zero direction misses). Returns 1 on a hit filling
 * out_point/out_t/out_face (each may be NULL), else 0. */
int cyber_snapper_raycast(const CyberSnapper* snapper, const float origin[3],
                          const float direction[3], float max_distance, float out_point[3],
                          float* out_t, uint32_t* out_face);

/* ---- EditMesh element queries (world space, brute force at cage scale) --
 *
 * Deterministic: distance ties break toward the lower element id. */

/* Nearest live vertex within `max_distance` of `query`. Returns 1 and fills
 * out_vertex/out_position (either may be NULL) when found, else 0. */
int cyber_mesh_nearest_vertex(const CyberMesh* mesh, const float query[3], float max_distance,
                              uint32_t* out_vertex, float out_position[3]);

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
int cyber_mesh_nearest_edge(const CyberMesh* mesh, const float query[3], float max_distance,
                            uint32_t* out_edge, float out_point[3]);

/* Endpoint vertex ids of a live edge. Returns 1 on success, 0 when the mesh
 * is NULL or the edge id is not alive. */
int cyber_mesh_edge_endpoints(const CyberMesh* mesh, uint32_t edge, uint32_t out_vertices[2]);

/* 1 when the live edge borders exactly one face, 0 when interior, -1 when
 * the mesh is NULL or the edge id is not alive. */
int cyber_mesh_is_boundary_edge(const CyberMesh* mesh, uint32_t edge);

/* Position of a live vertex. Returns 1 on success, 0 when the mesh is NULL
 * or the vertex id is not alive. */
int cyber_mesh_vertex_position(const CyberMesh* mesh, uint32_t vertex, float out_position[3]);

/* Live faces adjacent to a live edge, with each face's ring size (vertex
 * count) — the Build Quad/Build Triangle edge-drag semantics dispatch on
 * whether the picked boundary edge borders a quad or a triangle (task
 * 4.1). Fills out_faces/out_sizes (either may be NULL) in deterministic
 * (radial) order. Returns the number of entries WRITTEN — never more than
 * the 2 the out arrays hold, so the returned count is always a safe loop
 * bound — or -1 when the mesh is NULL or the edge id is not alive. A
 * non-manifold edge (3+ faces, which the engine supports and tags) reports
 * 2 here and its true valence through cyber_mesh_edge_face_count. */
int cyber_mesh_edge_faces(const CyberMesh* mesh, uint32_t edge, uint32_t out_faces[2],
                          size_t out_sizes[2]);

/* True number of live faces adjacent to a live edge, unclamped: 0 for a
 * wire edge, 1 on a boundary, 2 on a manifold interior edge, 3+ on a
 * non-manifold one. Returns -1 when the mesh is NULL or the edge id is not
 * alive. */
int cyber_mesh_edge_face_count(const CyberMesh* mesh, uint32_t edge);

/* Shortest edge path between two live vertices (Dijkstra over live edges,
 * Euclidean weights; deterministic — Path Distribute's "closest path
 * between the stroke's first and last vertex", task 4.1). Follows the
 * copy_positions convention: returns the TOTAL path vertex count
 * (endpoints inclusive, from -> to order) and fills at most max_vertices
 * entries of out_vertices (may be NULL when max_vertices is 0, for size
 * queries). Returns 0 when either vertex is dead, from == to, or no path
 * exists. */
size_t cyber_mesh_shortest_vertex_path(const CyberMesh* mesh, uint32_t from, uint32_t to,
                                       uint32_t* out_vertices, size_t max_vertices);

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
size_t cyber_mesh_boundary_loop(const CyberMesh* mesh, uint32_t edge, uint32_t* out_vertices,
                                size_t max_vertices, int* out_closed);

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
                                    const CyberSnapper* snapper, CyberLoopMetrics* out_metrics);

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
CyberStatus cyber_retopo_create_face(CyberMesh* mesh, const float* points_xyz, size_t point_count,
                                     const CyberSnapper* snapper, uint32_t* out_face);

/* Tweak: drops a live vertex at `target`, snapped to the Target surface
 * when `snapper` is non-NULL. Tweak ignores pins by design (pinned
 * vertices stay movable by explicit tweak). */
CyberStatus cyber_retopo_tweak_vertex(CyberMesh* mesh, uint32_t vertex, const float target[3],
                                      const CyberSnapper* snapper);

/* Move with surface-geodesic falloff: displaces `seed_vertex` by
 * `displacement`, with a smooth falloff over geodesic (through-the-surface)
 * distance up to `radius`. Vertices of disconnected components are NEVER
 * affected, no matter how close in space. Pinned vertices resist; moved
 * vertices reproject onto the Target when `snapper` is non-NULL. */
CyberStatus cyber_retopo_move(CyberMesh* mesh, uint32_t seed_vertex, const float displacement[3],
                              float radius, const uint32_t* pinned, size_t pinned_count,
                              const CyberSnapper* snapper);

/* Relax: tangential Laplacian smoothing inside the brush (`center`,
 * `radius`; radius <= 0 relaxes the whole mesh). `strength` is the 0..1
 * per-iteration blend. Explicit pins are honoured; auto_pin_corners
 * (non-zero) additionally pins low-valence grid corners so regular patch
 * shapes survive. Vertices reproject onto the Target when `snapper` is
 * non-NULL. */
CyberStatus cyber_retopo_relax(CyberMesh* mesh, const float center[3], float radius, float strength,
                               int iterations, int auto_pin_corners, const uint32_t* pinned,
                               size_t pinned_count, const CyberSnapper* snapper);

/* Erase: removes every face whose centroid lies within the pressure-scaled
 * radius of `center` (radius grows with stylus pressure in [0,1]: half the
 * base radius at 0 up to 1.5x at 1), then any vertices left isolated.
 * Writes the number of removed faces to *out_removed (may be NULL). */
CyberStatus cyber_retopo_erase(CyberMesh* mesh, const float center[3], float base_radius,
                               float pressure, size_t* out_removed);

/* Deletes the listed faces (dead/out-of-range ids are skipped), then any
 * vertices left isolated. Writes the number of faces actually removed to
 * *out_removed (may be NULL). */
CyberStatus cyber_retopo_delete_faces(CyberMesh* mesh, const uint32_t* faces, size_t face_count,
                                      size_t* out_removed);

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
CyberStatus cyber_retopo_create_grid(CyberMesh* mesh, const float* points_xyz, size_t rows,
                                     size_t cols, const CyberSnapper* snapper, size_t* out_faces);

/* Dissolves interior edges (the "scribble over an edge" gesture): each
 * listed edge with exactly two live faces is removed and its faces merged
 * into one (a triangle pair becomes a quad). Dead, boundary, and
 * would-be-degenerate edges are skipped, as are edges invalidated by an
 * earlier dissolve in the same call. Writes the number of edges actually
 * dissolved to *out_dissolved (may be NULL); dissolving none is CYBER_OK
 * with 0. */
CyberStatus cyber_retopo_dissolve_edges(CyberMesh* mesh, const uint32_t* edges, size_t edge_count,
                                        size_t* out_dissolved);

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
CyberStatus cyber_retopo_build_face(CyberMesh* mesh, size_t count, const uint32_t* vertex_ids,
                                    const float* points_xyz, const CyberSnapper* snapper,
                                    uint32_t* out_face, uint32_t* out_ring_vertices);

/* Grows the single triangle on a BOUNDARY edge into a quad (Build Quad's
 * triangle-edge drag, task 4.1): splits `edge` and drops the new ring
 * vertex at `point_xyz` (snapped to the Target first when `snapper` is
 * non-NULL). The face id is preserved. Writes the new vertex id to
 * *out_vertex (may be NULL). Fails with CYBER_ERR_INVALID_ARG (mesh
 * unchanged) when the edge is dead, interior, or its face is not a
 * triangle (growing a quad would leave an n-gon). */
CyberStatus cyber_retopo_grow_boundary_edge(CyberMesh* mesh, uint32_t edge,
                                            const float point_xyz[3], const CyberSnapper* snapper,
                                            uint32_t* out_vertex);

/* Path Distribute (task 4.1): repositions an ordered chain of live
 * vertices so they sit evenly (by arc length) along the chain's own
 * polyline; the two endpoints stay fixed and every moved vertex snaps to
 * the Target when `snapper` is non-NULL. `vertices` must be an ordered
 * chain of at least 3 DISTINCT live vertices where consecutive entries
 * are joined by live edges (e.g. a cyber_mesh_shortest_vertex_path
 * result). Positions only — topology is untouched. Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) on short/dead/broken chains. */
CyberStatus cyber_retopo_distribute_path(CyberMesh* mesh, const uint32_t* vertices, size_t count,
                                         const CyberSnapper* snapper);

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
CyberStatus cyber_retopo_surface_cut(CyberMesh* mesh, const float a[3], const float b[3],
                                     const float view_dir[3], int triangulate_ngons,
                                     const CyberSnapper* snapper, size_t* out_split_edges,
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
CyberStatus cyber_retopo_patch_clone(CyberMesh* mesh, const uint32_t* faces, size_t face_count,
                                     const float xf[12], int flip, const CyberSnapper* snapper,
                                     uint32_t* out_new_faces, size_t* out_new_face_count);

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
 * or a zero offset, and with CYBER_ERR_INVALID_PARAM on rings < 1 (the
 * out-of-range-number code the other numeric knobs use). */
CyberStatus cyber_retopo_extend_boundary_grid(CyberMesh* mesh, const uint32_t* chain, size_t count,
                                              int closed, const float offset[3], int rings,
                                              const CyberSnapper* snapper,
                                              uint32_t* out_outer_chain, size_t* out_new_faces);

/* Extend Boundary, triangle fan (task 4.2): closes the ordered boundary
 * chain onto a single apex at the chain centroid + `apex_offset` (snapped
 * to the Target when `snapper` is non-NULL) with a fan of triangles, one
 * per chain edge (plus the wrap edge when `closed`). Winding is corrected
 * against the existing face on the chain's first edge. Writes the apex
 * vertex id to *out_apex and the new face count to *out_new_faces (either
 * may be NULL). Chain validation matches
 * cyber_retopo_extend_boundary_grid. */
CyberStatus cyber_retopo_extend_boundary_fan(CyberMesh* mesh, const uint32_t* chain, size_t count,
                                             int closed, const float apex_offset[3],
                                             const CyberSnapper* snapper, uint32_t* out_apex,
                                             size_t* out_new_faces);

/* Draw Strip (task 4.2): a quad strip welded onto the boundary edge
 * start_a/start_b whose stations follow `path_xyz` (x,y,z per point —
 * world-space stroke samples, typically resampled at quad-size arc
 * length). Each station's rail pair spans `width` along
 * cross(view_dir, tangent) with sign continuity (curved strokes never
 * flip rails); rail vertices snap to the Target when `snapper` is
 * non-NULL; winding is corrected against the start edge's face. Writes
 * the number of new faces to *out_new_faces (may be NULL). Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) on an empty path, a degenerate
 * view direction, or when start_a/start_b are dead or not joined by a
 * live BOUNDARY edge, and with CYBER_ERR_INVALID_PARAM on width <= 0 (the
 * out-of-range-number code the other numeric knobs use). */
CyberStatus cyber_retopo_draw_strip(CyberMesh* mesh, const float* path_xyz, size_t point_count,
                                    float width, const float view_dir[3], uint32_t start_a,
                                    uint32_t start_b, const CyberSnapper* snapper,
                                    size_t* out_new_faces);

/* Transform Vertices (task 4.2): applies `xf` to every vertex in
 * `vertices` in place. When `snapper` is non-NULL each transformed vertex
 * is then reprojected onto the Target, and the RE-SNAP REPORT counts the
 * vertices whose reprojection moved them by more than `resnap_epsilon`
 * (*out_resnapped) with the maximum such distance (*out_max_distance;
 * either may be NULL). Fails with CYBER_ERR_INVALID_ARG (mesh unchanged)
 * on an empty list, dead ids, or repeated vertices, and with
 * CYBER_ERR_INVALID_PARAM on a non-finite component of `xf` — one NaN there
 * reaches every listed vertex and a NaN position cannot be edited back. */
CyberStatus cyber_retopo_transform_vertices(CyberMesh* mesh, const uint32_t* vertices, size_t count,
                                            const float xf[12], const CyberSnapper* snapper,
                                            float resnap_epsilon, size_t* out_resnapped,
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
                                        const CyberSnapper* snapper, size_t* out_added_faces);

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
                                      float match_tolerance, CyberResymmetrizeReport* out_report);

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
 *   - cyber_retopo_subdivide (and _ex, in either mode) REBUILDS THE MESH FROM
 *     SCRATCH (subdivision returns a new mesh). EVERY vertex, edge and face id
 *     is reassigned, so ALL caller-side annotations keyed on ids are
 *     orphaned and must be dropped or rebuilt by the caller. The handle's
 *     own hidden-face / tagged-edge render state is cleared here for the
 *     same reason.
 *
 *   - cyber_retopo_loop_subdivide rebuilds the mesh the same way and carries
 *     the same consequences; see its own note below.
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
                                  const uint32_t* pinned, size_t pinned_count, size_t* out_moved,
                                  float* out_max_distance);

/* Subdivision modes. Both produce the SAME topology (every n-gon becomes n
 * quads around its centroid); they differ only in where the new vertices go. */
typedef enum CyberSubdivisionMode {
    /* Vertices stay put, edge points at the midpoint: resolution, no
     * curvature. The historical behaviour of cyber_retopo_subdivide. */
    CYBER_SUBDIV_LINEAR = 0,
    /* Catmull-Clark smooth rules — face points, edge points and the vertex
     * weighting — so the cage converges toward its limit surface with no
     * Target to reproject onto. Creases (edges tagged as features, plus
     * boundary and non-manifold edges) use the sharp rule, so open borders do
     * not shrink inward and corners stay put. A mesh with no feature tags
     * subdivides fully smoothly; tag the edges you want kept hard first. */
    CYBER_SUBDIV_CATMULL_CLARK = 1
} CyberSubdivisionMode;

/* Subdivision into quads, LINEAR (Catmull-Clark topology, NO smoothing), plus
 * optional REPROJECTION: when `snapper` is non-NULL every vertex of the
 * subdivided mesh is projected onto the Target surface, which is what
 * "subdivide+reproject" means (with linear subdivision the reprojection is
 * what recovers curvature — linear subdivision alone only adds vertices along
 * the existing facets). Writes the resulting face count to *out_faces (may be
 * NULL). See the ELEMENT-ID STABILITY note above: all ids are reassigned.
 *
 * Exactly cyber_retopo_subdivide_ex(mesh, snapper, CYBER_SUBDIV_LINEAR,
 * out_faces). It keeps its old signature rather than growing a mode parameter
 * because that would break every already-compiled caller of this ABI; the
 * smooth mode is a sibling entry point instead. */
CyberStatus cyber_retopo_subdivide(CyberMesh* mesh, const CyberSnapper* snapper, size_t* out_faces);

/* Subdivision into quads in the chosen `mode`, plus the same optional
 * reprojection as cyber_retopo_subdivide. With CYBER_SUBDIV_CATMULL_CLARK the
 * result carries curvature on its own, so `snapper` may be NULL even when the
 * caller wants a rounded surface; passing one still pulls the smoothed result
 * back onto the Target, which is the accurate combination when a Target
 * exists. Fails with CYBER_ERR_INVALID_ARG on an unknown mode. Writes the
 * resulting face count to *out_faces (may be NULL). ELEMENT-ID STABILITY is
 * as above in BOTH modes: all ids are reassigned. */
CyberStatus cyber_retopo_subdivide_ex(CyberMesh* mesh, const CyberSnapper* snapper,
                                      CyberSubdivisionMode mode, size_t* out_faces);

/* How cyber_retopo_loop_subdivide places the vertices it produces. The
 * topology is the same either way (every triangle becomes four); only the
 * positions differ, and the difference is the whole reason both exist, so it
 * is never inferred. Persisted by callers, so values are append-only. */
typedef enum CyberLoopSubdivideMode {
    /* True Loop weights: the surface converges to the Loop limit surface, so
     * THE SHAPE CHANGES — a faceted cage visibly rounds off. */
    CYBER_LOOP_SUBDIVIDE_SMOOTH = 0,
    /* Pure topological 1-to-4 split: every new vertex is an exact edge
     * midpoint and NO original vertex moves, so the surface is unchanged and
     * only the triangle count grows. This is "more polygons, same shape". */
    CYBER_LOOP_SUBDIVIDE_LINEAR = 1
} CyberLoopSubdivideMode;

/* Loop subdivision: TRIANGLES IN, TRIANGLES OUT — every triangle becomes four,
 * so a mesh of N triangles comes back with 4N. This is the triangle
 * counterpart of cyber_retopo_subdivide, which applies Catmull-Clark topology
 * and would turn each triangle into three QUADS instead.
 *
 * `mode` chooses smoothing (see CyberLoopSubdivideMode); a caller who wants
 * resolution without a shape change passes CYBER_LOOP_SUBDIVIDE_LINEAR and gets
 * their vertices back bit-identical. Open meshes keep their border: boundary
 * edge points are plain midpoints and boundary vertices use the 1/8, 3/4, 1/8
 * curve rule, never the interior mask.
 *
 * A face that is not a triangle is refused with CYBER_ERR_UNSUPPORTED_TOPOLOGY
 * naming the face and its side count; the mesh is left exactly as it was. It is
 * deliberately NOT fan-triangulated on the caller's behalf — that is a topology
 * decision only the caller can make, and cyber_retopo_triangulate is the
 * explicit opt-in.
 *
 * `snapper` may be NULL. When it is not, every vertex of the refined mesh is
 * projected onto the Target afterwards, exactly as cyber_retopo_subdivide's
 * "subdivide+reproject" does. Writes the resulting face count to *out_faces
 * (may be NULL).
 *
 * ELEMENT-ID STABILITY (the contract callers MUST read): like
 * cyber_retopo_subdivide, this REBUILDS THE MESH FROM SCRATCH. EVERY vertex,
 * edge and face id is reassigned, so ALL caller-side annotations keyed on ids
 * (pins, loop tags, hidden faces) are orphaned and must be dropped or rebuilt.
 * The handle's own hidden-face / tagged-edge render state and its soft
 * selection are cleared here for the same reason. On a refusal nothing is
 * rebuilt and nothing is cleared. */
CyberStatus cyber_retopo_loop_subdivide(CyberMesh* mesh, CyberLoopSubdivideMode mode,
                                        const CyberSnapper* snapper, size_t* out_faces);

/* Fan-triangulates every face with more than three sides. Writes the
 * resulting face count to *out_faces (may be NULL). See the ELEMENT-ID
 * STABILITY note above: vertex/edge ids survive, face ids partially. */
CyberStatus cyber_retopo_triangulate(CyberMesh* mesh, size_t* out_faces);

/* ---- soft selection (manual-retopology spec, gradient regions) ----------
 *
 * A per-vertex weight field in [0,1] lives ON THE MESH HANDLE, next to the
 * hidden-face and tagged-edge overlays. The region ops below REPLACE it
 * (line, sphere) or ACCUMULATE into it (paint); the selection ops reshape
 * it; the weighted transform/relax consume it.
 *
 * SURFACE GLUE: cyber_retopo_selection_transform and _relax re-project the
 * vertices they move onto the Target inside the SAME pass whenever a
 * `snapper` is supplied. There is no separate cleanup call and none is
 * needed — calling cyber_retopo_snap_all afterwards is redundant and would
 * also drag the vertices the selection deliberately left alone. A drag
 * gesture is driven by calling _transform once per incremental delta.
 *
 * ZERO WEIGHT MEANS UNTOUCHED: a vertex whose weight is 0 is skipped
 * entirely — neither moved nor re-snapped — so its position is bit-identical
 * after the call. PINS: a pinned vertex is skipped the same way WHATEVER its
 * weight. cyber_retopo_selection_relax and
 * cyber_retopo_selection_transform_pinned take the `pinned` list;
 * cyber_retopo_selection_transform is the latter with an empty list, so on
 * that call weight is the only thing that holds a vertex still.
 *
 * ELEMENT-ID STABILITY (see the block above): weights are indexed by vertex
 * id, so cyber_retopo_subdivide clears the selection and every saved slot
 * along with the other id-keyed handle state. Triangulate preserves vertex
 * ids and therefore preserves the selection. Ids of DELETED vertices are
 * recycled by the next op that creates one (erase, delete_faces,
 * dissolve_edges, merge_vertices free them), so the selection and every
 * saved slot are unselected at those ids as the op completes — as are the
 * hidden-face set and the tagged-edge list on their own ids: no id-keyed
 * handle state can be inherited by unrelated new geometry. Caller-side
 * annotations keyed on element ids need the same treatment across deleting
 * ops. */

/* Editable falloff curves. All map 0 -> 0 and 1 -> 1 monotonically, so the
 * curve changes how a gradient feels, not where it starts or ends.
 * Persisted by callers, so values are append-only. */
typedef enum CyberFalloff {
    CYBER_FALLOFF_LINEAR = 0,
    CYBER_FALLOFF_SMOOTH = 1, /* smoothstep — the default */
    CYBER_FALLOFF_SHARP = 2,  /* t^2, weight builds late */
    CYBER_FALLOFF_ROUND = 3   /* quarter circle, weight builds early */
} CyberFalloff;

/* Result of a weighted transform/relax.
 *
 * BOTH COUNTS ARE DISTINCT VERTICES, never per-iteration writes: `moved` is
 * the number of distinct vertices the op wrote — weight > 0 and not pinned —
 * and so never exceeds the mesh's vertex count, even for a multi-iteration
 * cyber_retopo_selection_relax that revisits each of them every sweep. It
 * counts WRITES, NOT DISPLACEMENTS: a vertex whose weight is positive but
 * negligible (say 1e-34, which one-ring smoothing readily produces at the far
 * tail of a gradient) blends to a target that rounds to its current position
 * in float, and the op writes that same value back. Such a vertex is in
 * `moved` even though its position is bit-identical afterwards. Use `moved`
 * as "how many vertices the selection reached", not as "how many visibly
 * moved".
 *
 * `resnapped` counts the distinct moved vertices whose Target re-projection
 * pulled them back STRICTLY FURTHER than `resnap_epsilon`, and never exceeds
 * `moved`. `max_snap_distance` is the largest counted correction.
 *
 * `moved - resnapped` is therefore the vertices the re-projection did not
 * have to correct at all (with the default epsilon of 0, a correction of
 * exactly 0). There are two ways to land there, and both are normal:
 *   - the negligible-weight case above — the blend never left the surface, so
 *     re-projecting an already-projected point returns it unchanged;
 *   - a vertex whose weighted target happens to still lie on the Target.
 * Neither is a leak, and neither means a vertex escaped the glue. */
typedef struct CyberSoftTransformReport {
    size_t moved;
    size_t resnapped;
    float max_snap_distance;
} CyberSoftTransformReport;

/* Line gradient: weight ramps 0 at `anchor` to 1 at `end` following
 * `falloff`, and stays 1 beyond `end`. With `snap_angle` non-zero the
 * anchor->end direction is first quantized to `snap_degrees` increments
 * (15 is the shell default) measured IN THE VIEW PLANE — the plane
 * perpendicular to `view_dir`, the same screen-space convention
 * cyber_retopo_draw_strip and cyber_retopo_surface_cut use. Replaces the
 * current selection. Fails with CYBER_ERR_INVALID_ARG (selection
 * unchanged) on a null argument, a degenerate (zero-length) line, or a
 * non-finite anchor/end/view direction — or a non-finite `snap_degrees`
 * when `snap_angle` is non-zero. */
CyberStatus cyber_retopo_selection_line(CyberMesh* mesh, const float anchor[3], const float end[3],
                                        const float view_dir[3], int snap_angle, float snap_degrees,
                                        CyberFalloff falloff);

/* Sphere region: weight 1 at `center` falling to 0 at `radius` along
 * `falloff`. Replaces the current selection. Fails with
 * CYBER_ERR_INVALID_ARG (selection unchanged) on a null center, radius
 * <= 0, or a non-finite center coordinate or radius. */
CyberStatus cyber_retopo_selection_sphere(CyberMesh* mesh, const float center[3], float radius,
                                          CyberFalloff falloff);

/* Paint one brush dab: covered weights gain `pressure` * falloff, or lose
 * it when `subtract` is non-zero, saturating at 1 and bottoming out at 0.
 * Fails with CYBER_ERR_INVALID_ARG (selection unchanged) on a null center,
 * radius <= 0, or a non-finite center coordinate or pressure — a dab that
 * describes no region is dropped, never applied. */
CyberStatus cyber_retopo_selection_paint(CyberMesh* mesh, const float center[3], float radius,
                                         float pressure, int subtract, CyberFalloff falloff);

/* Paint a whole gesture in one call: `samples_xyzp` holds `sample_count`
 * quadruplets (x, y, z, pressure) along the stroke, all sharing one brush.
 * Equivalent to calling _paint once per sample but rejects the vertices no
 * sample can reach up front. Samples with a non-finite coordinate or
 * pressure are skipped; the rest of the stroke still paints. Fails with
 * CYBER_ERR_INVALID_ARG on a null or empty sample list or radius <= 0. */
CyberStatus cyber_retopo_selection_paint_stroke(CyberMesh* mesh, const float* samples_xyzp,
                                                size_t sample_count, float radius, int subtract,
                                                CyberFalloff falloff);

/* Zeroes every weight. */
CyberStatus cyber_retopo_selection_clear(CyberMesh* mesh);
/* w -> 1 - w over the live vertices. */
CyberStatus cyber_retopo_selection_invert(CyberMesh* mesh);
/* Grows the selection by taking the one-ring maximum, `steps` times
 * (steps <= 0 is a no-op). */
CyberStatus cyber_retopo_selection_expand(CyberMesh* mesh, int steps);
/* Shrinks the selection by taking the one-ring minimum, `steps` times. */
CyberStatus cyber_retopo_selection_contract(CyberMesh* mesh, int steps);
/* Softens the weight transition by averaging over the one-ring, `steps`
 * times — the shell's "smooth by 1 / 5 / 10" is just this count. Never
 * touches positions. */
CyberStatus cyber_retopo_selection_smooth(CyberMesh* mesh, int steps);

/* Copies up to `capacity` weights into `out` (index = vertex id) and
 * returns the total weight count available, exactly like
 * cyber_mesh_copy_positions. Returns 0 for a NULL mesh. */
size_t cyber_retopo_selection_copy_weights(const CyberMesh* mesh, float* out, size_t capacity);

/* Replaces the whole weight field; each value is clamped into [0,1]. */
CyberStatus cyber_retopo_selection_set_weights(CyberMesh* mesh, const float* weights, size_t count);

/* Saves the current selection into the named slot on the handle (an
 * existing slot of that name is overwritten), and loads it back. The slots
 * live on the handle and die with it; to persist them past the session put
 * the mesh in a CyberDocument (see the document block below) and save that —
 * cyber_document_set_edit_mesh copies the slots in and
 * cyber_document_edit_mesh hands them back. _load fails with
 * CYBER_ERR_INVALID_ARG when the name is unknown. Names must be non-empty
 * NUL-terminated UTF-8. */
CyberStatus cyber_retopo_selection_save(CyberMesh* mesh, const char* name);
CyberStatus cyber_retopo_selection_load(CyberMesh* mesh, const char* name);

/* Slot enumeration, in name order. The pointer returned by _slot_name is
 * owned by the handle and stays valid until the next _save on that handle
 * or an op that clears the slots; returns NULL for an out-of-range index. */
size_t cyber_retopo_selection_slot_count(const CyberMesh* mesh);
const char* cyber_retopo_selection_slot_name(const CyberMesh* mesh, size_t index);

/* Weighted transform: applies the 12-float affine `xf` (same column-major
 * layout as cyber_retopo_transform_vertices) per vertex scaled by its
 * selection weight, re-projecting onto the Target in the same pass when
 * `snapper` is non-NULL. *out_report may be NULL. Fails with
 * CYBER_ERR_INVALID_ARG (mesh unchanged) on a null transform, and with
 * CYBER_ERR_INVALID_PARAM on a non-finite component of `xf` — same gate as
 * cyber_retopo_selection_relax's `strength`, for the same reason. */
CyberStatus cyber_retopo_selection_transform(CyberMesh* mesh, const float xf[12],
                                             const CyberSnapper* snapper, float resnap_epsilon,
                                             CyberSoftTransformReport* out_report);

/* The same weighted transform with a pin list: every vertex id in `pinned` is
 * skipped whatever its weight, so it is neither moved nor re-snapped and its
 * position is bit-identical after the call — the guarantee the ZERO WEIGHT
 * MEANS UNTOUCHED note above states for pins. `pinned` may be NULL (with
 * `pinned_count` 0), which makes this exactly
 * cyber_retopo_selection_transform. Ids that are not live vertices are
 * harmless. Fails with CYBER_ERR_INVALID_ARG (mesh unchanged) on a null
 * transform, or on a null `pinned` with a non-zero count, and with
 * CYBER_ERR_INVALID_PARAM on a non-finite component of `xf`. */
CyberStatus cyber_retopo_selection_transform_pinned(CyberMesh* mesh, const float xf[12],
                                                    const uint32_t* pinned, size_t pinned_count,
                                                    const CyberSnapper* snapper,
                                                    float resnap_epsilon,
                                                    CyberSoftTransformReport* out_report);

/* Weighted relax: the ordinary relax sweep (auto-pinned grid corners,
 * tangential projection, in-pass re-snap) with the per-vertex blend
 * additionally scaled by the selection weight. `pinned` may be NULL.
 * *out_report may be NULL. `strength` must be in [0,1] — same gate as
 * cyber_retopo_relax; anything else (NaN included) fails with
 * CYBER_ERR_INVALID_PARAM and leaves the mesh untouched. */
CyberStatus cyber_retopo_selection_relax(CyberMesh* mesh, float strength, int iterations,
                                         const uint32_t* pinned, size_t pinned_count,
                                         const CyberSnapper* snapper, float resnap_epsilon,
                                         CyberSoftTransformReport* out_report);

/* ---- document persistence (application-shell spec) ---------------------
 *
 * A CyberDocument is the editing unit that OUTLIVES the session: the
 * high-poly Target, the low-poly EditMesh, and the named soft-selection
 * slots, serialized into one versioned byte container. It is what makes a
 * saved selection survive a restart — the slots on a CyberMesh handle are
 * session state and vanish with cyber_mesh_free.
 *
 * THE SEAM IS EXPLICIT AND BY VALUE. cyber_document_set_edit_mesh copies the
 * handle's geometry AND its named slots into the document;
 * cyber_document_edit_mesh hands back a fresh handle carrying those slots.
 * Nothing is aliased or kept in sync afterwards: mutate the mesh and you must
 * set it on the document again. The LIVE (unsaved) weight field is not a
 * slot and is not persisted — cyber_retopo_selection_save it under a name
 * first, and cyber_retopo_selection_load it after the round trip.
 *
 * ELEMENT-ID STABILITY: slot weights are indexed by EditMesh vertex id, so a
 * document is only meaningful with the EditMesh it was saved with. Store both
 * or neither.
 *
 * MESH DATA CARRIED: both meshes persist with their attribute columns (corner
 * UVs and normals, vertex colours) and their feature-edge tags, so a document
 * reloaded next session is the one that was saved rather than bare geometry.
 *
 * BUILD GATE: implemented only when the application-shell library is in the
 * build (-DCYBER_BUILD_APP=ON, the default). The declarations always exist so
 * the ABI is stable; without the module every call fails with
 * CYBER_ERR_RUNTIME or returns NULL/0. */
typedef struct CyberDocument CyberDocument;

/* An empty document (no meshes, no slots), or NULL when this build has no
 * application-shell library or allocation failed. Free with
 * cyber_document_free (NULL is a no-op). */
CyberDocument* cyber_document_create(void);
void cyber_document_free(CyberDocument* doc);

/* Copies `mesh` into the document. _set_edit_mesh also copies the handle's
 * named selection slots, REPLACING any the document already held. A NULL
 * `mesh` clears that slot of the document (and, for the edit mesh, its
 * slots). Fails with CYBER_ERR_INVALID_ARG on a NULL document. */
CyberStatus cyber_document_set_target(CyberDocument* doc, const CyberMesh* mesh);
CyberStatus cyber_document_set_edit_mesh(CyberDocument* doc, const CyberMesh* mesh);

/* Fresh handles owning a COPY of the document's meshes; free them with
 * cyber_mesh_free. The edit-mesh handle comes back with the document's named
 * selection slots restored and an empty live weight field, so
 * cyber_retopo_selection_load picks up where the save left off. Returns NULL
 * on a NULL document or allocation failure; a document that never had a mesh
 * set yields an empty handle (vertex count 0), not NULL. */
CyberMesh* cyber_document_target(const CyberDocument* doc);
CyberMesh* cyber_document_edit_mesh(const CyberDocument* doc);

/* The document's named slots without materializing a mesh: the count, the
 * name at `index` in name order (NULL when out of range; owned by the
 * document and valid until the next mutation), and the weights of one slot
 * following the cyber_mesh_copy_positions convention — returns the total
 * weight count and fills at most `capacity`, so call once with out=NULL to
 * size the buffer. Weight index == EditMesh vertex id. Returns 0 for an
 * unknown slot. */
size_t cyber_document_slot_count(const CyberDocument* doc);
const char* cyber_document_slot_name(const CyberDocument* doc, size_t index);
size_t cyber_document_slot_weights(const CyberDocument* doc, const char* name, float* out,
                                   size_t capacity);

/* Serialization. _save follows the copy_positions convention (returns the
 * byte count, fills at most `capacity`) and is cached, so the size-then-fill
 * pair serializes once. _load returns NULL on a truncated or foreign buffer.
 * _save_file / _load_file are the same container on disk. */
size_t cyber_document_save(const CyberDocument* doc, uint8_t* out, size_t capacity);
CyberDocument* cyber_document_load(const uint8_t* bytes, size_t count);
CyberStatus cyber_document_save_file(const CyberDocument* doc, const char* path);
CyberDocument* cyber_document_load_file(const char* path);

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
    CYBER_ACTION_CREATE_GRID
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
                                   const float* samples_xyt, size_t sample_count, float aspect,
                                   CyberStrokeInterpretation** out);

void cyber_stroke_interpretation_free(CyberStrokeInterpretation* interpretation);

/* Stage-1 result: shape class and its heuristic confidence (0..1). */
CyberStrokeShape cyber_stroke_interpretation_shape(const CyberStrokeInterpretation* interpretation);
float cyber_stroke_interpretation_shape_confidence(const CyberStrokeInterpretation* interpretation);

/* Stage-2 under-stroke context. */
CyberStrokeContext cyber_stroke_interpretation_context(
    const CyberStrokeInterpretation* interpretation);

/* Ranked candidates, best first. Index 0 is the chosen interpretation; the
 * rest are the one-tap alternatives. */
size_t cyber_stroke_interpretation_candidate_count(const CyberStrokeInterpretation* interpretation);
CyberStrokeAction cyber_stroke_interpretation_action(
    const CyberStrokeInterpretation* interpretation, size_t candidate);
float cyber_stroke_interpretation_confidence(const CyberStrokeInterpretation* interpretation,
                                             size_t candidate);

/* Mesh elements candidate `candidate` would touch, in deterministic order.
 * Element ids are engine element ids (see CYBER_INVALID_ID note above).
 * Returns 1 and fills out_kind/out_id (either may be NULL) when the indices
 * are in range, else 0. */
size_t cyber_stroke_interpretation_element_count(const CyberStrokeInterpretation* interpretation,
                                                 size_t candidate);
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
size_t cyber_stroke_interpretation_corner_count(const CyberStrokeInterpretation* interpretation);
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
    CYBER_BAKE_NORMAL = 0,   /* tangent-space normal map (RGB, encoded [0,1]) */
    CYBER_BAKE_AO,           /* ambient occlusion / openness (1 channel) */
    CYBER_BAKE_DISPLACEMENT, /* signed height along the low-poly normal (1 ch) */
    CYBER_BAKE_POSITION,     /* Target hit position, world space (RGB) */
    CYBER_BAKE_COLOR,        /* Target vertex color at the hit (RGB) */
    CYBER_BAKE_CURVATURE,    /* signed mean curvature around mid-gray (1 ch) */
    CYBER_BAKE_CAVITY        /* concavity only, white = flat/convex (1 ch) */
} CyberBakeMap;

typedef struct CyberBakeParams {
    int width; /* output resolution */
    int height;
    float cageDistance; /* rays start at surface + normal*cageDistance, cast inward 2x */
    int aoSamples;      /* hemisphere rays per texel for AO */
    float aoRadius;     /* an AO ray hit beyond this does not occlude */
    /* Curvature magnitude (1/length) saturating CURVATURE/CAVITY to full
     * white/black. 0 = auto (95th percentile of |curvature| on the Target).
     * Appended in 0.6.0 — always initialise via cyber_default_bake_params. */
    float curvatureRange;
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

/* ---- auto-routed seam paths (uv-editing) -----------------------------
 *
 * The UV Path tool: place waypoints on the EditMesh and the engine routes a
 * least-cost edge path between consecutive ones, biased toward feature-tagged
 * and concave (valley) edges so short hops follow the groove instead of
 * cutting straight across flat surface. Waypoints stay editable until commit;
 * commit marks the routed edges into a CyberSeamSet — the SAME seam model the
 * stroke-over-edges Pencil/Erase gestures use — and arms a resume marker so
 * the next path continues from the last committed point.
 *
 * Every entry point here returns 0 / CYBER_ERR_RUNTIME when the engine was
 * built without the UV module (CYBER_BUILD_UV=OFF), exactly like
 * cyber_uv_atlas. */

/* Opaque mutable set of seam edges (wraps cyber::uv::SeamSet). Free with
 * cyber_seam_set_free. */
typedef struct CyberSeamSet CyberSeamSet;

/* Opaque pending seam path (wraps cyber::uv::SeamPath). BORROWED MESH — this
 * is NOT the copying snapshot CyberSnapper takes: the path holds a reference
 * to the mesh handle it was created from, so that handle MUST outlive the
 * path (free the path first, and root the mesh in any garbage-collected
 * binding). It also caches vertex ids against that mesh, so recreate the path
 * if the mesh is edited. Free with cyber_seam_path_free. */
typedef struct CyberSeamPath CyberSeamPath;

/* Per-unit-length routing multipliers (POD mirror of
 * cyber::uv::SeamCostOptions). Lower means "cheaper, prefer this edge". Fill
 * with cyber_default_seam_path_options, then override as needed; values are
 * clamped to a strictly positive floor internally, so no setting can break
 * the router. */
typedef struct CyberSeamPathOptions {
    float flatWeight;    /* baseline multiplier for an ordinary edge */
    float featureWeight; /* multiplier for a feature-tagged edge */
    float concaveWeight; /* multiplier for a valley edge past creaseDegrees */
    float convexWeight;  /* multiplier for a ridge edge past creaseDegrees */
    float creaseDegrees; /* dihedral MAGNITUDE (degrees) that counts as a crease */
    float minWeight;     /* floor applied to the chosen multiplier */
} CyberSeamPathOptions;

/* Fills options with the engine defaults. No-op on NULL. */
void cyber_default_seam_path_options(CyberSeamPathOptions* options);

/* Signed dihedral angle of a live edge IN DEGREES: positive in a concave
 * valley, negative over a convex ridge, 0 when flat or when the edge is not
 * exactly two-face manifold. Returns 0 on NULL / dead ids. */
float cyber_mesh_edge_signed_dihedral(const CyberMesh* mesh, uint32_t edge);

/* --- seam set --- */
CyberStatus cyber_seam_set_create(CyberSeamSet** out);
void cyber_seam_set_free(CyberSeamSet* seams);
size_t cyber_seam_set_count(const CyberSeamSet* seams);
int cyber_seam_set_is_seam(const CyberSeamSet* seams, uint32_t edge);
/* Seam edge ids in ascending order. Follows the copy_positions convention:
 * returns the TOTAL count and fills at most max_edges entries (out_edges may
 * be NULL when max_edges is 0, for size queries). */
size_t cyber_seam_set_edges(const CyberSeamSet* seams, uint32_t* out_edges, size_t max_edges);
CyberStatus cyber_seam_set_mark(CyberSeamSet* seams, uint32_t edge);
/* Erases a seam edge — also the undo primitive for a commit: erase every id
 * cyber_seam_path_commit reported and the pre-commit state is back (edges
 * that were already seams are never reported, so they survive). */
CyberStatus cyber_seam_set_erase(CyberSeamSet* seams, uint32_t edge);

/* --- pending path --- */
/* Creates a path over `mesh`; `options` may be NULL (defaults). The path
 * BORROWS `mesh`: every later call on the handle reads it, so `mesh` must
 * still be alive then — cyber_mesh_free on it before cyber_seam_path_free is
 * a use-after-free. */
CyberStatus cyber_seam_path_create(const CyberMesh* mesh, const CyberSeamPathOptions* options,
                                   CyberSeamPath** out);
void cyber_seam_path_free(CyberSeamPath* path);

/* Appends a waypoint and routes the segment closing onto it. With the resume
 * marker armed and no waypoints yet, the path is seeded at the marker first.
 * Returns 1 on success, 0 for a dead vertex or a repeat of the last waypoint. */
int cyber_seam_path_add_waypoint(CyberSeamPath* path, uint32_t vertex);
/* Repositions waypoint `index`, re-routing ONLY its adjacent segments.
 * Returns 1 on success, 0 when the index or the vertex is invalid. */
int cyber_seam_path_move_waypoint(CyberSeamPath* path, size_t index, uint32_t vertex);
/* Repositions waypoint `index` onto the nearest live vertex within `radius`
 * of `position` (the drag gesture). Returns 1 on success, 0 when nothing is
 * in range (the path is then untouched). */
int cyber_seam_path_move_waypoint_to(CyberSeamPath* path, size_t index, const float position[3],
                                     float radius);
/* Deletes waypoint `index`; an interior deletion merges its two adjacent
 * segments into one re-routed segment. Returns 1 on success, else 0. */
int cyber_seam_path_remove_waypoint(CyberSeamPath* path, size_t index);
/* Drops the pending waypoints; committed seams and the resume marker stay. */
void cyber_seam_path_clear(CyberSeamPath* path);

size_t cyber_seam_path_waypoint_count(const CyberSeamPath* path);
/* Waypoint vertex ids, copy_positions convention. */
size_t cyber_seam_path_waypoints(const CyberSeamPath* path, uint32_t* out_vertices,
                                 size_t max_vertices);
size_t cyber_seam_path_segment_count(const CyberSeamPath* path);
/* Routed vertex chain of one segment (endpoints inclusive), copy_positions
 * convention. Returns 0 for an out-of-range index or an unroutable segment. */
size_t cyber_seam_path_segment(const CyberSeamPath* path, size_t index, uint32_t* out_vertices,
                               size_t max_vertices);
/* Monotonic counter bumped each time THIS segment re-routes — the local
 * re-route isolation an editor needs to know which geometry to redraw.
 * Returns 0 for an out-of-range index. */
uint64_t cyber_seam_path_segment_revision(const CyberSeamPath* path, size_t index);
/* 1 when there is at least one segment and every segment routed. */
int cyber_seam_path_is_routed(const CyberSeamPath* path);
/* Whole pending path as one vertex chain (shared joints appear once) /
 * as the live edges it would seam. Both copy_positions convention. */
size_t cyber_seam_path_vertices(const CyberSeamPath* path, uint32_t* out_vertices,
                                size_t max_vertices);
size_t cyber_seam_path_edges(const CyberSeamPath* path, uint32_t* out_edges, size_t max_edges);

/* Turns the pending path into seam edges in `seams`, arms the resume marker
 * at the path's last vertex and clears the pending waypoints. A path that is
 * not fully routed commits nothing and is LEFT PENDING for repair.
 * Follows the copy_positions convention over the UNDO RECORD: returns the
 * total number of edges this commit newly marked (edges that were already
 * seams are excluded, so erasing the reported ids restores the exact
 * pre-commit state) and fills at most max_edges of them. */
size_t cyber_seam_path_commit(CyberSeamPath* path, CyberSeamSet* seams, uint32_t* out_edges,
                              size_t max_edges);
/* Vertex the last commit ended on, or CYBER_INVALID_ID when no marker is
 * armed. */
uint32_t cyber_seam_path_resume_marker(const CyberSeamPath* path);
/* Forgets the resume marker so the next waypoint starts a fresh path. Never
 * touches a seam set — committed seams are unaffected. */
void cyber_seam_path_drop_resume_marker(CyberSeamPath* path);

/* --- Unwrap along seams the caller marked ------------------------------- */

/* The knobs that still mean something once the caller supplies the seams. The
 * atlas's chart-angle and chart-merge fields are absent on purpose: those exist
 * to DECIDE where to cut, and here that decision is the seam set. */
typedef struct CyberUnwrapSeamsParams {
    float packMargin;  /* gap around each island, in UV units */
    int textureSize;   /* resolution for the texel-density readout */
    int reorientCharts; /* non-zero: rotate each chart to its minimum-area
                         * bounding box before packing */
} CyberUnwrapSeamsParams;

/* Fills params with the engine defaults. No-op on NULL. */
void cyber_default_unwrap_seams_params(CyberUnwrapSeamsParams* params);

/* Unwraps `mesh` along `seams` — the seam set the caller built, by marking
 * edges directly or by committing routed seam paths into it — and writes the
 * per-corner UV attribute IN PLACE. The seams define the charts: the mesh is cut
 * into islands at exactly those edges, each island is LSCM-unwrapped (planar
 * projection as fallback), optionally re-oriented, and the islands are packed
 * into the unit square.
 *
 * This is the counterpart to cyber_uv_atlas: same pipeline and the same
 * CyberAtlasResult, but the cuts come from the caller instead of autoSeams.
 * `chartCount` is the number of islands the seams cut the mesh into, and
 * `seamEdges` is the size of the set that was handed in.
 *
 * An EMPTY seam set means "do not cut" — a closed mesh comes back as one chart —
 * never "seam it automatically". A NULL `seams` is CYBER_ERR_INVALID_ARG.
 * `params` may be NULL (defaults); `out` may be NULL. Returns CYBER_ERR_RUNTIME
 * when the engine was built without the UV module. */
CyberStatus cyber_uv_unwrap_seams(CyberMesh* mesh, const CyberSeamSet* seams,
                                  const CyberUnwrapSeamsParams* params, CyberAtlasResult* out);

/* As cyber_uv_unwrap_seams, with cooperative progress and cancellation on the
 * same terms as cyber_uv_atlas_cancellable: on cancel the status is
 * CYBER_ERR_CANCELLED and `mesh` IS LEFT EXACTLY AS IT WAS. Either callback may
 * be NULL; `user` is passed through to both. */
CyberStatus cyber_uv_unwrap_seams_cancellable(CyberMesh* mesh, const CyberSeamSet* seams,
                                              const CyberUnwrapSeamsParams* params,
                                              CyberProgressCb progress, CyberCancelCb cancel,
                                              void* user, CyberAtlasResult* out);

/* Sews `edges` shut: each one is removed from `seams` (so it stops cutting
 * islands) and, across it, the two corners at each shared endpoint are welded to
 * the average of their UVs so the boundary fits together. This is the "sew" half
 * of the seam model — the inverse of the cut cyber_uv_unwrap_seams applies.
 *
 * Both `mesh` and `seams` are modified. An edge that is not currently a seam is
 * ignored rather than an error. NULL `mesh`, NULL `seams`, or NULL `edges` with
 * a non-zero count is CYBER_ERR_INVALID_ARG. */
CyberStatus cyber_uv_stitch_seams(CyberMesh* mesh, CyberSeamSet* seams, const uint32_t* edges,
                                  size_t edge_count);

/* ---- sculpt handoff bridge (pipeline-bridge) -------------------------
 *
 * A versioned interchange for taking a sculpt as a Target, plus a field
 * evaluator that lets a bake sample exact normals/AO/curvature instead of
 * raycasting a mesh, plus conform (re-snap an EditMesh onto a replaced
 * Target). NOTHING here links a specific sculpting or volumetric engine: the
 * format and the evaluator interface are the only coupling points.
 * Format reference: docs/sculpt-handoff-format.md. */

/* Handoff version this build supports. */
#define CYBER_HANDOFF_VERSION_MAJOR 1
#define CYBER_HANDOFF_VERSION_MINOR 0

/* What a handoff declared, filled on a successful open. `producer` points into
 * a thread-local buffer valid until the next capi call on this thread. */
typedef struct CyberHandoffInfo {
    int versionMajor;
    int versionMinor;
    const char* producer; /* never NULL; empty when the handoff declared none */
    size_t vertexCount;
    size_t faceCount;
    int hasVertexColors;
    int hasVertexNormals;
    int hasMaterialMix;
    /* Triangles the handoff described that the mesh refused (a repeated vertex
     * index). Non-zero means the Target has less geometry than the producer
     * sent — the loss is counted here rather than passed over in silence. */
    size_t droppedFaces;
} CyberHandoffInfo;

/* Opens a handoff file (PLY profile; .gltf/.glb also accepted) as a Target.
 * On success *out receives a new CyberMesh the caller frees with
 * cyber_mesh_free, and `info` (may be NULL) is filled.
 * An unsupported version is CYBER_ERR_INCOMPATIBLE_VERSION with *out left
 * NULL — no partial Target is ever produced. cyber_last_error() names both
 * the found and the supported version. */
CyberStatus cyber_handoff_open(const char* path, CyberMesh** out, CyberHandoffInfo* info);

/* The in-memory handoff profile: plain arrays, no intermediate file. Optional
 * pointers may be NULL; positions and indices may not. Triangles only
 * (index_count % 3 == 0). */
typedef struct CyberHandoffBuffers {
    const float* positions;    /* 3 * vertex_count */
    const float* normals;      /* 3 * vertex_count, optional */
    const float* colors;       /* 3 * vertex_count in [0,1], optional */
    const float* material_mix; /* vertex_count, optional */
    size_t vertex_count;
    const uint32_t* indices;
    size_t index_count;
    int version_major;
    int version_minor;
    const char* producer; /* optional */
} CyberHandoffBuffers;

/* Same contract as cyber_handoff_open, from buffers instead of a file —
 * including the version gate, so an in-process producer cannot bypass it. */
CyberStatus cyber_handoff_open_buffers(const CyberHandoffBuffers* buffers, CyberMesh** out,
                                       CyberHandoffInfo* info);

/* ---- field-sampled baking -------------------------------------------- */

/* A sampleable field, as three C callbacks plus an opaque user pointer.
 * `distance` MUST be a Lipschitz-<=1 LOWER bound on the true distance to the
 * surface (negative inside): the bake sphere-traces the cage ray through it,
 * and a loose bound overshoots and misses thin features.
 * `gradient` writes the (not necessarily unit) field gradient at p.
 * `occlusion` returns openness in [0,1] over a hemisphere of `radius`.
 * All three are called from the baking thread and must be reentrant. */
typedef float (*CyberFieldDistanceFn)(void* user, const float p[3]);
typedef void (*CyberFieldGradientFn)(void* user, const float p[3], float out_gradient[3]);
typedef float (*CyberFieldOcclusionFn)(void* user, const float p[3], const float n[3],
                                       float radius);

typedef struct CyberFieldEvaluator {
    CyberFieldDistanceFn distance;
    CyberFieldGradientFn gradient;
    CyberFieldOcclusionFn occlusion;
    void* user;
} CyberFieldEvaluator;

/* Bakes `map` from a field instead of a Target mesh. `high` MAY be NULL for
 * CYBER_BAKE_NORMAL / _AO / _CURVATURE / _CAVITY — the four maps a field can
 * answer; every other map still needs the Target and returns
 * CYBER_ERR_INVALID_ARG with a NULL one.
 *
 * A separate entry point rather than a field on CyberBakeParams, deliberately:
 * growing that struct would break every caller that allocates it. */
CyberStatus cyber_bake_field(const CyberMesh* low, const CyberMesh* high, CyberBakeMap map,
                             const CyberBakeParams* params, const CyberFieldEvaluator* field,
                             CyberImage** out);

/* ---- conform ---------------------------------------------------------- */

typedef struct CyberConformReport {
    size_t movedVertices;
    float maxDeviation;
    float rmsDeviation;
    size_t flaggedCount; /* total, even when out_flagged could not hold them all */
} CyberConformReport;

/* Re-snaps every live vertex of `edit` onto `new_target`, preserving the
 * EditMesh's topology exactly (only positions change), and reports the maximum
 * and RMS deviation. Vertices whose deviation exceeds `threshold` (<= 0
 * disables flagging) are flagged: the operation still completes, it just never
 * stretches silently.
 *
 * Follows the copy-positions convention: `report->flaggedCount` is the total,
 * and at most `max_flagged` ids are written to `out_flagged` (may be NULL).
 * The Target snapper is built internally, so no snapper handle is needed. */
CyberStatus cyber_conform(CyberMesh* edit, const CyberMesh* new_target, float threshold,
                          CyberConformReport* report, uint32_t* out_flagged, size_t max_flagged);

/* ---- named export presets (mesh-io) ----------------------------------
 *
 * A preset is pure DATA describing an export bundle for one target app: which
 * maps to bake, how to name them, what colour space and normal-map convention
 * that app expects, and which mesh container to write. The DATA half
 * (listing, resolving, reading) lives in core and is available in EVERY
 * build. Only cyber_export_bundle_write needs the UV module — it unwraps a
 * low-poly that carries no UVs — and without it that one call returns
 * CYBER_ERR_RUNTIME, exactly like cyber_uv_atlas. */

/* Preset schema version this build understands. A preset file declaring any
 * other version is rejected rather than partially honored. */
#define CYBER_PRESET_SCHEMA_VERSION 1

/* The built-in presets, in a stable order suitable for help text.
 * cyber_export_preset_builtin_name returns static storage (valid for the
 * process) or NULL for an out-of-range index. */
size_t cyber_export_preset_builtin_count(void);
const char* cyber_export_preset_builtin_name(size_t index);

/* A resolved preset. Opaque; release with cyber_export_preset_free. */
typedef struct CyberExportPreset CyberExportPreset;

/* Resolves a preset argument the way the CLI's --preset does: a built-in name
 * if one matches, otherwise a path to a user preset JSON file. On success
 * *out receives a handle the caller frees with cyber_export_preset_free.
 *
 * A preset file declaring an unsupported schema version is
 * CYBER_ERR_INCOMPATIBLE_VERSION (cyber_last_error names both versions); a
 * name that is neither built in nor a readable file is CYBER_ERR_INVALID_ARG
 * listing the built-ins; a malformed or contradictory file is CYBER_ERR_IO.
 * *out is left NULL in every failure case. */
CyberStatus cyber_export_preset_resolve(const char* name_or_path, CyberExportPreset** out);
void cyber_export_preset_free(CyberExportPreset* preset);

/* Everything a preset declares except its per-map list. Every `const char*`
 * points into `preset` and stays valid until it is freed. */
typedef struct CyberExportPresetInfo {
    const char* name;
    int schemaVersion;
    const char* meshFormat;    /* container extension, no dot: obj|ply|stl|gltf|glb */
    const char* textureFormat; /* container extension, no dot: png|exr */
    const char* namingPattern; /* tokens: {basename} {map} {preset} {ext} */
    const char* units;
    const char* upAxis;
    int resolution;
    /* Normal-map green channel: 1 = +Y (OpenGL — Blender, Unity, glTF),
     * 0 = -Y (DirectX — Unreal). Getting this wrong inverts every bump. */
    int normalGreenPlusY;
    size_t mapCount;
} CyberExportPresetInfo;

CyberStatus cyber_export_preset_info(const CyberExportPreset* preset, CyberExportPresetInfo* out);

/* One entry of the preset's map list. Strings point into `preset`. */
typedef struct CyberExportPresetMap {
    const char* map;        /* canonical kind: normal|ao|curvature|cavity|
                             * displacement|color|position */
    const char* colorSpace; /* "linear" | "srgb" */
    /* Token substituted for {map} in the naming pattern — the map's canonical
     * name unless the preset overrode it to match an app's suffix style. */
    const char* suffix;
} CyberExportPresetMap;

CyberStatus cyber_export_preset_map(const CyberExportPreset* preset, size_t index,
                                    CyberExportPresetMap* out);

/* The file name entry `index` produces for `basename`, with the naming
 * pattern's tokens expanded. Points into a thread-local buffer valid until the
 * next capi call on this thread (same contract as CyberHandoffInfo.producer).
 * NULL on a null preset, an out-of-range index, or an expansion REFUSED for
 * leaving the output directory (an absolute or ".."-climbing value arriving
 * through any token, `basename` included) — cyber_last_error() carries the
 * reason. A NULL return is never a usable file name; never join it. */
const char* cyber_export_preset_map_file_name(const CyberExportPreset* preset, size_t index,
                                              const char* basename);

/* Overrides the preset's texture resolution, the way the CLI's --texture-size
 * does. Rejects a non-positive value rather than baking a zero-sized map. */
CyberStatus cyber_export_preset_set_resolution(CyberExportPreset* preset, int resolution);

/* ---- export bundles --------------------------------------------------- */

typedef struct CyberBundleParams {
    const char* meshPath; /* required; its extension wins over the preset's
                           * (an explicit output path is the user speaking
                           * last) and a mismatch comes back as a warning */
    const char* basename; /* substituted for {basename}; NULL or empty means
                           * the mesh path's stem */
    float cageDistance;   /* projection cage for every bake, in model units */
    int aoSamples;
    float aoRadius;
} CyberBundleParams;

/* Fills params with the engine defaults (meshPath and basename left NULL).
 * No-op on NULL. */
void cyber_default_bundle_params(CyberBundleParams* params);

/* What a bundle wrote. Opaque; release with cyber_bundle_result_free. */
typedef struct CyberBundleResult CyberBundleResult;

/* Writes `preset`'s bundle for the (low, high) pair: the mesh, then one baked
 * map per preset entry, named and encoded the way the target app expects.
 *
 * `low` IS MODIFIED IN PLACE when it carries no UVs — baking is impossible
 * without them, and requiring a pre-unwrap would make presets useless on a
 * freshly remeshed mesh. Clone it first (cyber_mesh_clone) to keep the
 * original. `high` is never modified and MUST NOT be NULL: every map is a
 * projection from it.
 *
 * On success *out receives the result handle. On a cooperative cancel the
 * status is CYBER_ERR_CANCELLED and *out is left NULL; files already written
 * are not removed. Returns CYBER_ERR_RUNTIME when the engine was built
 * without the UV module. Either callback may be NULL. */
CyberStatus cyber_export_bundle_write(CyberMesh* low, const CyberMesh* high,
                                      const CyberExportPreset* preset,
                                      const CyberBundleParams* params, CyberProgressCb progress,
                                      CyberCancelCb cancel, void* user, CyberBundleResult** out);
void cyber_bundle_result_free(CyberBundleResult* result);

/* One file the bundle wrote. Strings point into the result handle. */
typedef struct CyberBundleFile {
    const char* path;
    const char* kind;       /* "mesh", or the preset map name ("normal", ...) */
    const char* colorSpace; /* encoding actually written; "" for the mesh */
    int width;              /* 0 for the mesh */
    int height;
} CyberBundleFile;

size_t cyber_bundle_result_file_count(const CyberBundleResult* result);
CyberStatus cyber_bundle_result_file(const CyberBundleResult* result, size_t index,
                                     CyberBundleFile* out);

/* Non-fatal notes — a preset/extension mismatch, a map the source could not
 * feed. `cyber_bundle_result_warning` returns NULL for a bad index. */
size_t cyber_bundle_result_warning_count(const CyberBundleResult* result);
const char* cyber_bundle_result_warning(const CyberBundleResult* result, size_t index);

/* Set when the low-poly carried no UVs and the bundle unwrapped it; the chart
 * count and worst angle distortion are that unwrap's. All three read 0 when
 * the low-poly already had UVs. */
int cyber_bundle_result_unwrapped(const CyberBundleResult* result);
int cyber_bundle_result_chart_count(const CyberBundleResult* result);
float cyber_bundle_result_max_angle_distortion(const CyberBundleResult* result);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CYBER_CAPI_H */
