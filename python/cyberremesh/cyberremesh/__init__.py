"""CyberRemesher Python bindings.

Thin, Pythonic ctypes bindings over the engine's versioned C ABI (the `capi`
module, ``libcyber_capi_shared``). Importing this package never loads the
shared library; that happens lazily on the first engine call, so the package
imports cleanly on a machine where the engine has not been built.

    from cyberremesh import Mesh, RemeshParams, remesh, CyberError

    mesh = Mesh.load_obj("model.obj")
    result = remesh(mesh, RemeshParams(target_quad_count=5000))
    print(result.stats.quads)
    result.save_obj("out.obj")
"""

from .api import (
    AtlasParams,
    AtlasResult,
    BakeMap,
    BakeParams,
    CyberError,
    Falloff,
    HAVE_NUMPY,
    Image,
    Mesh,
    FlowGuide,
    RemeshParams,
    SeamCostParams,
    SeamPath,
    SeamSet,
    Snapper,
    SoftTransformReport,
    Statistics,
    bake,
    is_available,
    remesh,
    version,
)

__all__ = [
    "Mesh",
    "FlowGuide",
    "RemeshParams",
    "Statistics",
    "remesh",
    "CyberError",
    "is_available",
    "version",
    "HAVE_NUMPY",
    "BakeMap",
    "BakeParams",
    "Image",
    "bake",
    "AtlasParams",
    "AtlasResult",
    "Falloff",
    "Snapper",
    "SoftTransformReport",
    "SeamCostParams",
    "SeamSet",
    "SeamPath",
]
__version__ = "0.5.0"
