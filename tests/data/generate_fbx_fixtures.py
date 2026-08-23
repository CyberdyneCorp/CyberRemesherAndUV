"""Regenerates the committed FBX fixtures in this directory.

FBX is a proprietary binary container with no permissively licensed writer, so
the engine cannot produce its own test input (mesh-io spec: FBX is import-only).
The fixtures are therefore authored once in Blender and committed; this script
is what authored them, so they can be reproduced or extended:

    blender --background --python tests/data/generate_fbx_fixtures.py

Each fixture is deliberately small (a cube or two) and exercises one property of
the importer:

    cube_quads.fbx    quad faces, UVs, normals, vertex colors; Y-up, metres
    cube_zup.fbx      the same 2 m cube written Z-up
    cube_cm.fbx       the same 2 m cube authored in a centimetre scene, so the
                      file declares centimetres and carries 200-unit geometry
    two_cubes.fbx     two mesh nodes at different world transforms — must import
                      as one mesh with both cubes in their placed positions

The three cube fixtures describe the same physical object through different
conventions, so a correct importer lands all three on the same bounding box.
"""

import os
import sys

import bpy

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# Blender's FBX exporter changes very little between releases, but pin what the
# fixtures were built with so a regeneration that shifts them is traceable.
print("generating FBX fixtures with Blender", bpy.app.version_string)


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def add_cube(name, location=(0.0, 0.0, 0.0), size=2.0):
    """A cube with UVs, per-corner normals and a vertex color layer."""
    bpy.ops.mesh.primitive_cube_add(size=size, location=location)
    cube = bpy.context.active_object
    cube.name = name

    mesh = cube.data
    # primitive_cube_add gives us a UVMap already; add colors so the importer's
    # color path has something to read.
    colors = mesh.color_attributes.new(name="Color", type="FLOAT_COLOR", domain="POINT")
    for i, entry in enumerate(colors.data):
        t = i / max(len(colors.data) - 1, 1)
        entry.color = (t, 0.5, 1.0 - t, 1.0)
    return cube


def export(filename, **overrides):
    opts = dict(
        filepath=os.path.join(OUT_DIR, filename),
        use_selection=False,
        use_mesh_modifiers=False,
        mesh_smooth_type="FACE",
        path_mode="STRIP",
        # Keep the polygons as authored — the point of the arity test.
        use_triangles=False,
        bake_anim=False,
    )
    opts.update(overrides)
    bpy.ops.export_scene.fbx(**opts)
    print("wrote", opts["filepath"])


def main():
    # 1. A 2 m quad cube in the conventional Blender export frame (Y-up).
    reset_scene()
    add_cube("QuadCube")
    export("cube_quads.fbx", axis_up="Y", axis_forward="-Z")

    # 2. The same 2 m cube, written Z-up — the convention 3ds Max, Unreal and
    #    Blender's own scenes use. Only the axis declaration differs.
    reset_scene()
    add_cube("QuadCube")
    export("cube_zup.fbx", axis_up="Z", axis_forward="Y")

    # 3. The same 2 m cube authored in a centimetre scene (1 unit = 1 cm, so the
    #    cube is 200 units across) — what a Unity/3ds Max pipeline produces. The
    #    file declares centimetres, and the importer has to honour that
    #    declaration to land back on a 2-unit cube.
    reset_scene()
    bpy.context.scene.unit_settings.system = "METRIC"
    bpy.context.scene.unit_settings.scale_length = 0.01
    add_cube("QuadCube", size=200.0)
    export("cube_cm.fbx", axis_up="Y", axis_forward="-Z", apply_unit_scale=True)

    # 4. Two separately transformed mesh nodes: the importer must apply each
    #    node's world transform, not merge the geometry at the origin.
    reset_scene()
    add_cube("Left", location=(-3.0, 0.0, 0.0))
    add_cube("Right", location=(3.0, 0.0, 0.0))
    export("two_cubes.fbx", axis_up="Y", axis_forward="-Z")


if __name__ == "__main__":
    main()
    sys.exit(0)
