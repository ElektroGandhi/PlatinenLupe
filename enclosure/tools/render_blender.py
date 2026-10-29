"""Photo-like render of the assembled case on a Skadis pegboard -- Blender (Cycles), from the STLs build.py wrote.

  blender -b -P tools/render_blender.py -- --work ~/.cache/platinenlupe-enclosure --out ../docs/images

Writes case_front.jpg. Uses the GPU if Blender finds one (OptiX, CUDA, HIP, Metal), else the CPU.
Colours are those of the printed case: beige body, red accents and badge, "PLATINEN" brown, "LUPE" black,
the loupe pen black. Tested with Blender 5.2.
"""
import argparse
import math
import os
import sys

import bmesh
import bpy
from mathutils import Vector

MM = 0.001
HOOK_DROP = 16.0       # platinenlupe_case.scad: hook_y = outer_h - 16
SLOT = (5.0, 15.0)     # Skadis slot width x height
PITCH_X = 40.0         # Skadis horizontal pitch (= hook_spacing)
PITCH_Y = 20.0         # NOT confirmed (see README "Check before printing"); only the background uses it
BOARD_T = 5.2


def srgb(h):
    c = [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    return [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c] + [1]


def material(name, colour, rough, coat=0.05):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = srgb(colour)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Coat Weight"].default_value = coat
    return m


def load(work, name, *mats):
    bpy.ops.wm.stl_import(filepath=os.path.join(work, name + ".stl"))
    o = bpy.context.selected_objects[0]
    for m in mats:
        o.data.materials.append(m)
    return o


def place(o):
    """OpenSCAD: front is +Z, up is +Y. Blender: front is -Y, up is +Z."""
    o.scale = (MM, MM, MM)
    o.rotation_euler = (math.radians(90), 0, 0)
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.shade_auto_smooth(angle=math.radians(35))
    o.select_set(False)


def pegboard(sc, beige, centre):
    """White Skadis board behind the back wall (y = 0 .. 5.2 mm), slots lined up with the hooks, dark wall behind."""
    xs = [v.co.x for v in beige.data.vertices]
    ys = [v.co.y for v in beige.data.vertices]
    hook_x, hook_y = (min(xs) + max(xs)) / 2 - PITCH_X / 2, max(ys) - HOOK_DROP
    t, w, h = BOARD_T * MM, 520 * MM, 420 * MM
    bpy.ops.mesh.primitive_cube_add(size=1, location=(centre.x, t / 2, centre.z + 20 * MM))
    board = bpy.context.object
    board.scale = (w, t, h)
    bpy.ops.object.transform_apply(scale=True)
    board.data.materials.append(material("skadis", "#f2f1ee", 0.65, coat=0))

    bm, r, n = bmesh.new(), SLOT[0] / 2, 10
    for row in range(-12, 13):
        for col in range(-8, 9):
            x, z = hook_x + col * PITCH_X + (PITCH_X / 2 if row % 2 else 0), hook_y + row * PITCH_Y
            half = SLOT[1] / 2 - r
            ring = [(x + r * math.cos(math.pi * i / n), z + half + r * math.sin(math.pi * i / n)) for i in range(n + 1)]
            ring += [(x - r * math.cos(math.pi * i / n), z - half - r * math.sin(math.pi * i / n)) for i in range(n + 1)]
            face = bm.faces.new([bm.verts.new((px * MM, -MM, pz * MM)) for px, pz in ring])
            ext = bmesh.ops.extrude_face_region(bm, geom=[face])
            bmesh.ops.translate(bm, verts=[v for v in ext["geom"] if isinstance(v, bmesh.types.BMVert)],
                                vec=(0, t + 2 * MM, 0))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)  # the kept start face of each extrusion points the wrong way
    me = bpy.data.meshes.new("slots")
    bm.to_mesh(me)
    bm.free()
    cutter = bpy.data.objects.new("slots", me)
    sc.collection.objects.link(cutter)
    mod = board.modifiers.new("slots", "BOOLEAN")
    mod.operation, mod.object, mod.solver = "DIFFERENCE", cutter, "EXACT"
    bpy.context.view_layer.objects.active = board
    bpy.ops.object.modifier_apply(modifier="slots")
    bpy.data.objects.remove(cutter)

    bpy.ops.mesh.primitive_plane_add(size=3, location=(centre.x, 60 * MM, centre.z), rotation=(math.radians(90), 0, 0))
    bpy.context.object.data.materials.append(material("wall", "#6f6a63", 0.9, coat=0))


def light(sc, name, centre, size, offset, power, colour):
    lamp = bpy.data.lights.new(name, "AREA")
    lamp.energy, lamp.size, lamp.color = power, size * 0.9, srgb(colour)[:3]
    o = bpy.data.objects.new(name, lamp)
    sc.collection.objects.link(o)
    o.location = centre + Vector(offset) * size
    o.rotation_euler = (centre - o.location).to_track_quat("-Z", "Y").to_euler()


def use_gpu(sc):
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("OPTIX", "CUDA", "HIP", "METAL", "ONEAPI"):
        try:
            prefs.compute_device_type = kind
        except TypeError:
            continue
        prefs.get_devices()
        devices = [d for d in prefs.devices if d.type == kind]
        if devices:
            for d in prefs.devices:
                d.use = d.type == kind
            sc.cycles.device = "GPU"
            return kind
    return "CPU"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=os.path.expanduser("~/.cache/platinenlupe-enclosure"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../docs/images"))
    ap.add_argument("--samples", type=int, default=256)
    ap.add_argument("--size", default="1600x1200")
    a = ap.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    beige = material("pla_beige", "#ebe1cc", 0.5)
    red = material("pla_red", "#a3121a", 0.42, coat=0.08)
    brown, black = material("pla_brown", "#5e3f27", 0.45), material("pla_black", "#141414", 0.4, coat=0.08)

    body = load(a.work, "assembled_beige", beige)
    objs = [body, load(a.work, "assembled_red", red), load(a.work, "assembled_platinen", brown),
            load(a.work, "assembled_lupe", black), load(a.work, "assembled_pen", material("pen", "#0e0e0f", 0.28, 0.3))]
    for o in objs:
        place(o)
    bpy.context.view_layer.update()

    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    centre, size = (lo + hi) / 2, (hi - lo).length
    pegboard(sc, body, centre)

    sc.world = bpy.data.worlds.new("world")
    sc.world.use_nodes = True
    bg = sc.world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value, bg.inputs["Strength"].default_value = srgb("#f6f2ea"), 0.3
    light(sc, "key", centre, size, (-1.0, -1.4, 1.3), 6, "#fff1dc")
    light(sc, "fill", centre, size, (1.5, -1.2, 0.4), 2, "#e6efff")
    light(sc, "rim", centre, size, (0.9, -0.3, 1.6), 3, "#ffffff")

    cam = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.lens = 85
    yaw, pitch, dist = math.radians(-25), math.radians(12), size * 2.8
    cam.location = centre + Vector((math.sin(yaw) * math.cos(pitch), -math.cos(yaw) * math.cos(pitch), math.sin(pitch))) * dist
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    cam.data.dof.use_dof, cam.data.dof.aperture_fstop = True, 16
    cam.data.dof.focus_distance = (centre - cam.location).length

    sc.render.engine = "CYCLES"
    print("device:", use_gpu(sc))
    sc.cycles.samples, sc.cycles.use_denoising = a.samples, True
    sc.render.resolution_x, sc.render.resolution_y = map(int, a.size.split("x"))
    sc.view_settings.view_transform, sc.view_settings.look = "AgX", "AgX - Medium High Contrast"
    sc.view_settings.exposure = -0.6
    sc.render.image_settings.file_format, sc.render.image_settings.quality = "JPEG", 88
    sc.render.filepath = os.path.abspath(os.path.join(a.out, "case_front.jpg"))
    bpy.ops.render.render(write_still=True)


main()
