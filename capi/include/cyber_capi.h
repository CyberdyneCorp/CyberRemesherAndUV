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
    int quadMethod;            /* 0 = field-aligned matching (default),
                                * 1 = Instant-Meshes position-field extractor */
} CyberRemeshParams;

/* Quadrangulator selection values for CyberRemeshParams.quadMethod. */
#define CYBER_QUAD_FIELD_ALIGNED 0
#define CYBER_QUAD_INSTANT_MESHES 1

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
