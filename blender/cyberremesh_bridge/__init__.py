"""CyberRemesher bridge — Blender addon.

UNVERIFIED: written against the Blender 3.x/4.x Python API but not executed
here (no Blender in the dev environment). It is a thin consumer of the
``cyberbridge`` client — all protocol logic lives there — so once ``cyberbridge``
is importable (installed, or its folder added to Blender's Python path) this
addon only marshals between Blender's mesh data and the client. Needs a first
run inside Blender to confirm the bpy glue.

Install: zip this folder and enable it in Edit > Preferences > Add-ons, then
open the sidebar (N) in the 3D viewport -> "CyberRemesher" tab.
"""

bl_info = {
    "name": "CyberRemesher Bridge",
    "author": "Cyberdyne Corp",
    "version": (0, 1, 0),
    "blender": (3, 0, 0),
    "location": "View3D > Sidebar > CyberRemesher",
    "description": "Push/pull meshes to a running CyberRemesher over the local bridge",
    "category": "Import-Export",
}

import bpy  # type: ignore
from bpy.props import IntProperty, StringProperty  # type: ignore
from bpy.types import Operator, Panel, PropertyGroup  # type: ignore

try:
    from cyberbridge import Client
except ImportError:  # pragma: no cover - surfaced to the user in the operators
    Client = None


def _wire_from_object(obj):
    """Blender mesh object -> wire mesh dict (positions flat, faces 0-indexed)."""
    mesh = obj.data
    positions = []
    for vertex in mesh.vertices:
        world = obj.matrix_world @ vertex.co
        positions.extend((world.x, world.y, world.z))
    faces = [list(poly.vertices) for poly in mesh.polygons]
    return {"positions": positions, "faces": faces}


def _object_from_wire(name, wire):
    """Wire mesh dict -> a new Blender mesh object in the active collection."""
    positions = wire.get("positions", [])
    verts = [
        (positions[i], positions[i + 1], positions[i + 2]) for i in range(0, len(positions), 3)
    ]
    faces = [tuple(face) for face in wire.get("faces", [])]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


class CyberBridgeSettings(PropertyGroup):
    port: IntProperty(name="Port", default=5140, min=1, max=65535)
    host: StringProperty(name="Host", default="127.0.0.1")


def _connect(settings):
    if Client is None:
        raise RuntimeError("cyberbridge package not found on Blender's Python path")
    return Client().connect(settings.port, host=settings.host)


class CYBER_OT_ping(Operator):
    bl_idname = "cyber.ping"
    bl_label = "Test Connection"

    def execute(self, context):
        try:
            with _connect(context.scene.cyber_bridge) as app:
                ok = app.ping()
            self.report({"INFO"} if ok else {"WARNING"}, "Connected" if ok else "No response")
        except Exception as exc:  # noqa: BLE001 - report any failure to the user
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class CYBER_OT_push_target(Operator):
    bl_idname = "cyber.push_target"
    bl_label = "Push Selected as Target"

    def execute(self, context):
        obj = context.active_object
        if obj is None or obj.type != "MESH":
            self.report({"ERROR"}, "Select a mesh object first")
            return {"CANCELLED"}
        try:
            with _connect(context.scene.cyber_bridge) as app:
                stats = app.push_target(_wire_from_object(obj))
            self.report({"INFO"}, f"Pushed {stats.get('vertices', 0)} verts")
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class CYBER_OT_pull_editmesh(Operator):
    bl_idname = "cyber.pull_editmesh"
    bl_label = "Pull EditMesh"

    def execute(self, context):
        try:
            with _connect(context.scene.cyber_bridge) as app:
                wire = app.pull_editmesh()
            if not wire.get("positions"):
                self.report({"WARNING"}, "EditMesh is empty")
                return {"CANCELLED"}
            _object_from_wire("CyberEditMesh", wire)
            self.report({"INFO"}, "Pulled EditMesh")
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        return {"FINISHED"}


class CYBER_PT_panel(Panel):
    bl_label = "CyberRemesher"
    bl_idname = "CYBER_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "CyberRemesher"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.cyber_bridge
        layout.prop(settings, "host")
        layout.prop(settings, "port")
        layout.operator("cyber.ping", icon="PLUGIN")
        layout.separator()
        layout.operator("cyber.push_target", icon="EXPORT")
        layout.operator("cyber.pull_editmesh", icon="IMPORT")


_CLASSES = (
    CyberBridgeSettings,
    CYBER_OT_ping,
    CYBER_OT_push_target,
    CYBER_OT_pull_editmesh,
    CYBER_PT_panel,
)


def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.cyber_bridge = bpy.props.PointerProperty(type=CyberBridgeSettings)


def unregister():
    del bpy.types.Scene.cyber_bridge
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
