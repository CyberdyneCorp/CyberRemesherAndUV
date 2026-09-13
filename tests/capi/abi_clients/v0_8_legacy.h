/* Extracted from the v0.8.0 public C header: the subset exercised by the
 * previous-client executable.  It intentionally does not include the current
 * header, so this translation unit cannot accidentally inherit new layouts or
 * declarations while proving source and binary compatibility. */
#ifndef CYBER_V0_8_LEGACY_H
#define CYBER_V0_8_LEGACY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CyberMesh CyberMesh;

typedef enum CyberStatus {
    CYBER_OK = 0,
    CYBER_ERR_IO,
    CYBER_ERR_INVALID_ARG,
    CYBER_ERR_INVALID_PARAM,
    CYBER_ERR_EMPTY,
    CYBER_ERR_RUNTIME,
    CYBER_ERR_CANCELLED,
    CYBER_ERR_INCOMPATIBLE_VERSION,
    CYBER_ERR_UNSUPPORTED_TOPOLOGY
} CyberStatus;

typedef enum CyberBackend {
    CYBER_BACKEND_AUTO = 0,
    CYBER_BACKEND_CPU = 1,
    CYBER_BACKEND_METAL = 2,
    CYBER_BACKEND_CUDA = 3,
    CYBER_BACKEND_OPENCL = 4
} CyberBackend;

typedef struct CyberRemeshParams {
    int targetQuads;
    float edgeScale;
    float sharpEdgeDegrees;
    float smoothNormalDegrees;
    float adaptivity;
    int pureQuads;
    int holeFillMaxBoundary;
    int quadMethod;
} CyberRemeshParams;

void cyber_version(int* major, int* minor, int* patch);
size_t cyber_available_backends(CyberBackend* out, size_t max_backends);
CyberStatus cyber_mesh_load_obj(const char* path, CyberMesh** out);
void cyber_mesh_free(CyberMesh* mesh);
size_t cyber_mesh_vertex_count(const CyberMesh* mesh);
size_t cyber_mesh_copy_positions(const CyberMesh* mesh, float* out, size_t max_floats);
void cyber_default_params(CyberRemeshParams* params);

#ifdef __cplusplus
}
#endif
#endif
