#!/usr/bin/env python3
"""Bambu Studio project (3MF) from STL parts: one print plate per colour, every part with its AMS slot.

Why one plate per colour: if two colours share a plate, the printer swaps filament in every layer that contains
both (each swap takes about 1 min and wastes 0.2-1.1 g). Single-colour plates print without a single swap.
Only multi-part objects (the badge with inlaid lettering) carry two colours -- and there the second colour is
only in the first few layers.

Layout follows a file saved by Bambu Studio itself:
  3D/3dmodel.model                 objects as <components> pointing at part files, <build> with positions
  3D/Objects/object_N.model        the meshes
  Metadata/model_settings.config   names, filament (extruder 1..n) per object and part, plates with their objects
  Metadata/project_settings.config printer, process and filament settings from bambu_p1s_template.json
The colours are also written as an m:colorgroup into the model (preview in other programs).

  from bambu_3mf import Project
  p = Project(filaments=[("Black", "#1A1A1A"), ("Dark brown", "#4A3428")])
  p.plate("Black", [("Base", "parts/base.stl", 1)])
  p.plate("Badge", [("Badge", [("Plate", "badge.stl", 2), ("Text", "text.stl", 1)])])
  p.write("project.3mf")
"""
from __future__ import annotations

import datetime as dt
import json
import math
import uuid
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

import numpy as np

try:
    import trimesh
except ImportError as e:  # pragma: no cover
    raise SystemExit("bambu_3mf needs trimesh (pip install trimesh)") from e

BED = (256.0, 256.0)             # Bambu Lab P1S / X1C / A1
KEEP_OUT = (18.0, 28.0)          # front left: filament cutter area (P1S, X1C) -- place nothing there
PLATE_GAP = 0.2                  # Bambu Studio places plates 1/5 of the bed width apart
MARGIN = 6.0                     # distance between parts and to the bed edge

NS = ('xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
      'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" '
      'xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02" '
      'xmlns:BambuStudio="http://schemas.bambulab.com/package/2021"')


TEMPLATE = Path(__file__).with_name("bambu_p1s_template.json")


def project_settings(filaments: list[tuple[str, str]], plates: int = 1) -> dict:
    """Project settings for n filaments: printer + process from the template, every filament setting as a list
    with one entry per slot. Bambu Studio expects the same length everywhere -- otherwise it crashes on load."""
    t = json.loads(TEMPLATE.read_text(encoding="utf-8"))
    n = len(filaments)
    cfg = dict(t["printer"])
    cfg.update(t["process"])
    for k, value in t["filament"].items():
        cfg[k] = [value[0] if isinstance(value, list) and value else value] * n
    cfg.update({
        "printer_settings_id": t["names"]["printer_settings_id"],
        "print_settings_id": t["names"]["print_settings_id"],
        "filament_settings_id": [t["names"]["filament_settings_id"]] * n,
        "filament_ids": [t["names"]["filament_ids"]] * n,
        "filament_colour": [c.upper() for _, c in filaments],
        "default_filament_colour": [""] * n,
        # Bambu Studio 2.x: every filament names its index and nozzle variant (otherwise slicing fails with
        # "could not found extruder_type ... filament_index 2")
        "filament_self_index": [str(i + 1) for i in range(n)],
        "filament_map": ["1"] * n,
        "filament_extruder_variant": ["Direct Drive Standard"] * n,
        "from": "project",
        # prime tower (only on plates with two colours) front right: parts are placed from the back left
        "wipe_tower_x": ["205"] * plates,
        "wipe_tower_y": ["30"] * plates,
        "version": "02.08.02.61",
    })
    return cfg


def _mesh(path: str | Path) -> trimesh.Trimesh:
    m = trimesh.load(str(path), force="mesh")
    m.merge_vertices()
    return m


class Project:
    def __init__(self, filaments: list[tuple[str, str]], title: str = "Project"):
        self.filaments = filaments      # [(name, "#RRGGBB"), ...] -> slot 1, 2, ...
        self.title = title
        self.plates: list[tuple[str, list]] = []

    def plate(self, name: str, objects: list) -> None:
        """objects: (name, STL, slot) or (name, [(part name, STL, slot), ...]) for a multi-part object."""
        self.plates.append((name, objects))

    # ------------------------------------------------------------ placement on the bed
    @staticmethod
    def _arrange(sizes: list[tuple[float, float]]) -> list[tuple[float, float]]:
        """Shelf packing: rows from back left to front right, centres in bed coordinates (0...256).
        The keep-out corner at the front left stays free. If something does not fit, abort (loud beats wrong)."""
        x, y, row_h = MARGIN, BED[1] - MARGIN, 0.0
        centres = []
        for w, d in sizes:
            if w > BED[0] - 2 * MARGIN or d > BED[1] - 2 * MARGIN:
                raise SystemExit(f"part {w:.0f} x {d:.0f} mm does not fit on the bed")
            if x + w > BED[0] - MARGIN:          # new row
                x, y, row_h = MARGIN, y - row_h - MARGIN, 0.0
            bottom = y - d
            if x < KEEP_OUT[0] + MARGIN and bottom < KEEP_OUT[1] + MARGIN:  # skip the front left corner
                x = KEEP_OUT[0] + MARGIN
            if bottom < MARGIN:
                raise SystemExit("plate too full -- split the parts over two plates")
            centres.append((x + w / 2, y - d / 2))
            x += w + MARGIN
            row_h = max(row_h, d)
        return centres

    # ------------------------------------------------------------ writing
    def write(self, target: str | Path) -> dict:
        target = Path(target)
        columns = max(1, math.ceil(math.sqrt(len(self.plates))))
        step = (BED[0] * (1 + PLATE_GAP), BED[1] * (1 + PLATE_GAP))
        mesh_files: list[tuple[int, str]] = []   # (mesh_id, xml)
        root_objects, build_items, settings_xml, plates_xml = [], [], [], []
        next_id = 1
        report = {"plates": []}

        colours = "".join(f'<m:color color="{c.upper()}FF"/>' for _, c in self.filaments)
        for p_index, (p_name, objects) in enumerate(self.plates):
            # load meshes, put them on z = 0 in print orientation and measure the footprint
            loaded = []
            for obj in objects:
                name, rest = obj[0], obj[1:]
                parts = rest[0] if isinstance(rest[0], list) else [(name, rest[0], rest[1])]
                meshes = [(p_name_, _mesh(path), slot) for p_name_, path, slot in parts]
                lo = np.min([m.bounds[0] for _, m, _ in meshes], axis=0)
                hi = np.max([m.bounds[1] for _, m, _ in meshes], axis=0)
                loaded.append((name, meshes, lo, hi))
            centres = self._arrange([(hi[0] - lo[0], hi[1] - lo[1]) for _, _, lo, hi in loaded])
            ox = (p_index % columns) * step[0]
            oy = -(p_index // columns) * step[1]
            instances = []
            p_report = {"name": p_name, "objects": []}
            for (name, meshes, lo, hi), (mx, my) in zip(loaded, centres):
                components, part_settings = [], []
                for part_name, m, slot in meshes:
                    mesh_id = next_id
                    next_id += 1
                    v = m.vertices - [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]]
                    verts = "".join(f'<vertex x="{a:.4f}" y="{b:.4f}" z="{c:.4f}"/>' for a, b, c in v)
                    tris = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in m.faces)
                    mesh_files.append((mesh_id, f'<object id="{mesh_id}" p:UUID="{uuid.uuid4()}" type="model"><mesh><vertices>{verts}'
                                                f'</vertices><triangles>{tris}</triangles></mesh></object>'))
                    components.append(mesh_id)
                    part_settings.append((mesh_id, part_name, slot, len(m.faces)))
                obj_id = next_id
                next_id += 1
                comps = "".join(f'<component p:UUID="{uuid.uuid4()}" p:path="/3D/Objects/object_{i}.model" objectid="{i}" '
                                f'transform="1 0 0 0 1 0 0 0 1 0 0 0"/>' for i in components)
                root_objects.append(f'<object id="{obj_id}" p:UUID="{uuid.uuid4()}" type="model"><components>{comps}</components></object>')
                tx, ty = ox + mx, oy + my
                build_items.append(f'<item objectid="{obj_id}" p:UUID="{uuid.uuid4()}" transform="1 0 0 0 1 0 0 0 1 {tx:.3f} {ty:.3f} 0" '
                                   f'printable="1"/>')
                main_slot = part_settings[0][2]
                parts_xml = "".join(
                    f'<part id="{mid}" subtype="normal_part"><metadata key="name" value="{escape(pn)}"/>'
                    f'<metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>'
                    f'<metadata key="extruder" value="{slot}"/>'
                    f'<mesh_stat face_count="{fc}" edges_fixed="0" degenerate_facets="0" facets_removed="0" '
                    f'facets_reversed="0" backwards_edges="0"/></part>'
                    for mid, pn, slot, fc in part_settings)
                settings_xml.append(f'<object id="{obj_id}"><metadata key="name" value="{escape(name)}"/>'
                                    f'<metadata key="extruder" value="{main_slot}"/>{parts_xml}</object>')
                instances.append(obj_id)
                p_report["objects"].append({"name": name, "parts": [(pn, slot) for _, pn, slot, _ in part_settings],
                                            "size_mm": [round(float(x), 1) for x in (hi - lo)]})
            plates_xml.append(
                f'<plate><metadata key="plater_id" value="{p_index + 1}"/>'
                f'<metadata key="plater_name" value="{escape(p_name)}"/><metadata key="locked" value="false"/>'
                + "".join(f'<model_instance><metadata key="object_id" value="{i}"/>'
                          f'<metadata key="instance_id" value="0"/>'
                          f'<metadata key="identify_id" value="{1000 + i}"/></model_instance>' for i in instances)
                + '</plate>')
            report["plates"].append(p_report)

        today = dt.date.today().isoformat()
        model = (f'<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" {NS} '
                 f'requiredextensions="p">'
                 f'<metadata name="Application">BambuStudio-02.08.02.61</metadata>'
                 f'<metadata name="Designer">{escape(self.title)} (bambu_3mf.py)</metadata>'
                 f'<metadata name="BambuStudio:3mfVersion">1</metadata>'
                 f'<metadata name="Title">{escape(self.title)}</metadata>'
                 f'<metadata name="CreationDate">{today}</metadata>'
                 f'<resources><m:colorgroup id="{next_id}">{colours}</m:colorgroup>'
                 + "".join(root_objects) + f'</resources><build p:UUID="{uuid.uuid4()}">' + "".join(build_items) + '</build></model>')
        rels = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships '
                'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                + "".join(f'<Relationship Target="/3D/Objects/object_{i}.model" Id="rel-{k + 1}" '
                          'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
                          for k, (i, _) in enumerate(mesh_files))
                + '</Relationships>')
        settings = ('<?xml version="1.0" encoding="UTF-8"?>\n<config>' + "".join(settings_xml)
                    + "".join(plates_xml) + '</config>')
        types = ('<?xml version="1.0" encoding="UTF-8"?>\n<Types '
                 'xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                 '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                 '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
                 '<Default Extension="config" ContentType="application/xml"/></Types>')
        root_rels = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships '
                     'xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                     '<Relationship Target="/3D/3dmodel.model" Id="rel-1" '
                     'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        target.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("[Content_Types].xml", types)
            z.writestr("_rels/.rels", root_rels)
            z.writestr("3D/3dmodel.model", model)
            z.writestr("3D/_rels/3dmodel.model.rels", rels)
            for i, xml in mesh_files:
                z.writestr(f"3D/Objects/object_{i}.model",
                           f'<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" '
                           f'{NS}><resources>{xml}</resources><build/></model>')
            z.writestr("Metadata/model_settings.config", settings)
            z.writestr("Metadata/project_settings.config",
                       json.dumps(project_settings(self.filaments, len(self.plates)), ensure_ascii=False, indent=1))
        report["file"] = str(target)
        report["filaments"] = [{"slot": k + 1, "name": n, "colour": c} for k, (n, c) in enumerate(self.filaments)]
        return report
