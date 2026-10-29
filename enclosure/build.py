"""Build the case print-ready: lettering -> STL -> mesh and collision checks -> 3MF (Bambu Lab) -> test slice.

  python build.py [--work DIR] [--print-dir DIR] [--no-slice]

Needs: OpenSCAD, Python with trimesh + matplotlib + numpy + shapely. Bambu Studio for the optional test slice.

Tool commands (both may contain arguments, they are split like a shell would):
  OPENSCAD       default: `openscad` on PATH, else `flatpak run org.openscad.OpenSCAD`,
                 else `flatpak-spawn --host flatpak run org.openscad.OpenSCAD` (when run inside a Flatpak)
  BAMBU_STUDIO   default: `bambu-studio` on PATH, else the slice step is skipped.
                 Example for an extracted AppImage, called from inside a Flatpak sandbox:
                 BAMBU_STUDIO="flatpak-spawn --host $HOME/Apps/bambu-studio/squashfs-root/AppRun"
Sandboxed tools (Flatpak OpenSCAD, flatpak-spawn) usually only see your home directory -- keep --work there.

Result: print/platinenlupe_case.3mf with three plates (beige, brown, badge in two colours) plus one STL per part.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "tools"))

# printable parts -> expected number of separate solids. If it differs, something is loose or fused.
PRINT_PARTS = {"body": 1, "lid": 1, "hooks": 2, "ring": 1, "stripe": 1, "badge": 1,
               "badge_text": 12, "light_frames": 2, "fin": 1, "saturn": 1, "quiver": 1, "slider": 1}
ASSEMBLED = ["assembled_beige", "assembled_brown", "assembled_text", "assembled_pen"]

# Checks with dummies for both boards (check.scad). Volume of the intersection in mm^3:
#   collision  must be ~0 (touching faces do not count)
#   contact    must be > 0.5 (hold-downs press on the PCBs)
#   blocked    must be > 0.5 (a USB-C plug in a covered opening hits the slider)
#   retention  must be > 0.2 (the part, moved against its snap, hits a barb/nub/latch: it cannot fall out).
#              Real snaps give 0.45 mm^3 and more; with the nubs taken away 0.06 remains (chord of the ring nub).
CHECKS = [("collision_body", "collision"), ("retention_boards", "retention"),
          ("collision_lid", "collision"), ("contact_hold_downs", "contact"),
          ("collision_lid_body", "collision"),
          ("collision_parts_body", "collision"), ("collision_parts_boards", "collision"),
          ("retention_fin", "retention"), ("retention_quiver", "retention"),
          ("retention_saturn", "retention"), ("retention_hooks", "retention"),
          ("clearance_quiver", "collision"), ("collision_pen", "collision"),
          ("collision_inlays", "collision"), ("retention_inlays", "retention"),
          ("collision_slider_run", "collision"), ("collision_slider_flash", "collision"),
          ("retention_slider", "retention"),
          ("collision_plug_run", "collision"), ("collision_plug_flash", "collision"),
          ("blocked_esp_run", "blocked"), ("blocked_node_flash", "blocked"),
          ("blocked_node_mid", "blocked"), ("blocked_esp_mid", "blocked")]
LIMITS = {"collision": lambda v: v < 0.05, "contact": lambda v: v > 0.5,
          "blocked": lambda v: v > 0.5, "retention": lambda v: v > 0.2}


def tool(env: str, cli: str | None, candidates: list[list[str]]) -> list[str] | None:
    """Command for a tool: CLI argument, then environment variable, then the first candidate found on PATH."""
    given = cli or os.environ.get(env)
    if given:
        return shlex.split(given)
    for cmd in candidates:
        if shutil.which(cmd[0]):
            return cmd
    return None


def openscad_cmd(cli: str | None) -> list[str]:
    cmd = tool("OPENSCAD", cli, [["openscad"],
                                 ["flatpak", "run", "org.openscad.OpenSCAD"],
                                 ["flatpak-spawn", "--host", "flatpak", "run", "org.openscad.OpenSCAD"]])
    if not cmd:
        raise SystemExit("OpenSCAD not found: install it or set OPENSCAD (see build.py --help)")
    return cmd


def run_openscad(scad: list[str], out: Path, var: str, value: str, src: Path) -> subprocess.CompletedProcess:
    return subprocess.run(scad + ["-o", str(out), "-D", f'{var}="{value}"', str(src)], capture_output=True, text=True)


def export(scad: list[str], work: Path) -> None:
    parts = list(PRINT_PARTS) + ASSEMBLED

    def one(part: str) -> str | None:
        r = run_openscad(scad, work / f"{part}.stl", "part", part, HERE / "platinenlupe_case.scad")
        msgs = [z for z in (r.stdout + r.stderr).splitlines() if any(w in z for w in ("WARNING", "ERROR", "empty"))]
        # OpenSCAD builds even with an undefined variable (warning, exit code 0) -- every warning aborts
        if r.returncode or msgs:
            return f"{part}: OpenSCAD reports\n  " + "\n  ".join(msgs or [r.stderr[-500:]])
        return None

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        errors = [e for e in pool.map(one, parts) if e]
    if errors:
        raise SystemExit("\n".join(errors))


def layer_bodies(mesh, pitch: float = 0.1, min_area: float = 0.05) -> int:
    """Number of bodies as the printer sees them: slice into layers, link islands that overlap the next layer.

    trimesh's split() counts two solids as one when they touch along a single edge. That is how the 0.6 mm
    light skin of the first lid (2026-10-29) passed: it met the lid only along its outline and printed as a
    loose plate. Here an edge contact links nothing -- the overlap area between layers is zero.
    """
    from shapely.strtree import STRtree
    z0, z1 = mesh.bounds[:, 2]
    heights = np.arange(z0 + pitch / 2, z1, pitch)
    sections = mesh.section_multiplane(plane_origin=[0, 0, 0], plane_normal=[0, 0, 1], heights=heights)
    # slivers without area are artefacts of slicing exactly through a face -- not islands
    layers = [[] if s is None else [p.buffer(0) for p in s.polygons_full if p.area > min_area] for s in sections]
    parent: dict = {}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for i, polys in enumerate(layers):
        for j in range(len(polys)):
            parent[(i, j)] = (i, j)
    for i in range(len(layers) - 1):
        below, above = layers[i], layers[i + 1]
        if not below or not above:
            continue
        tree = STRtree(above)
        for j, p in enumerate(below):
            for k in tree.query(p):
                if p.intersection(above[int(k)]).area > min_area:
                    parent[find((i, j))] = find((i + 1, int(k)))
    return len({find(n) for n in parent})


def check_meshes(work: Path) -> None:
    import trimesh
    for part, expected in PRINT_PARTS.items():
        m = trimesh.load(work / f"{part}.stl")
        found = len(m.split(only_watertight=False))
        layered = layer_bodies(m)
        if not m.is_watertight or found != expected or layered != expected:
            raise SystemExit(f"{part}: watertight={m.is_watertight}, {found} solids / {layered} printed bodies "
                             f"instead of {expected}")
        print(f"  {part:15} {' x '.join(f'{x:.1f}' for x in m.extents)} mm, {m.volume / 1000:.2f} cm^3")


def check_collisions(scad: list[str], work: Path) -> None:
    import trimesh
    folder = work / "check"
    folder.mkdir(exist_ok=True)

    def one(item: tuple[str, str]) -> tuple[str, str, float]:
        name, kind = item
        target = folder / f"{name}.stl"
        target.unlink(missing_ok=True)
        r = run_openscad(scad, target, "check", name, HERE / "check.scad")
        empty = "top level object is empty" in r.stdout + r.stderr
        vol = 0.0 if empty or not target.exists() else abs(trimesh.load(target, force="mesh").volume or 0.0)
        return name, kind, vol

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(one, CHECKS))
    failed = []
    for name, kind, vol in results:
        ok = LIMITS[kind](vol)
        print(f"  {name:24} {kind:9} {vol:8.2f} mm^3  {'ok' if ok else 'FAIL'}")
        if not ok:
            failed.append(name)
    if failed:
        raise SystemExit("collision check failed: " + ", ".join(failed))


def project(work: Path, print_dir: Path) -> Path:
    from bambu_3mf import Project
    p = Project(filaments=[("Beige", "#E8D9B5"), ("Brown", "#6B4A2E")], title="Platinenlupe")
    s = lambda n: str(work / f"{n}.stl")
    p.plate("Beige", [("Body", s("body"), 1), ("Lid", s("lid"), 1), ("Hooks", s("hooks"), 1)])
    p.plate("Brown", [("Porthole ring", s("ring"), 2), ("Speed stripes", s("stripe"), 2),
                      ("Light frames", s("light_frames"), 2), ("Tail fin", s("fin"), 2),
                      ("Quiver", s("quiver"), 2),
                      ("Saturn", s("saturn"), 2), ("USB selector", s("slider"), 2)])
    p.plate("Badge", [("Badge", [("Plate", s("badge"), 2), ("Lettering", s("badge_text"), 1)])])
    target = work / "platinenlupe_case.3mf"
    p.write(target)
    print_dir.mkdir(parents=True, exist_ok=True)
    for part in PRINT_PARTS:
        shutil.copyfile(work / f"{part}.stl", print_dir / f"{part}.stl")
    shutil.copyfile(target, print_dir / target.name)
    return target


def slice_test(bambu: list[str] | None, project_file: Path) -> None:
    if not bambu:
        print("  skipped: Bambu Studio not found (set BAMBU_STUDIO or pass --bambu-studio)")
        return
    folder = project_file.parent / "slice"
    folder.mkdir(exist_ok=True)
    result = folder / "result.json"
    result.unlink(missing_ok=True)
    try:
        r = subprocess.run(bambu + ["--slice", "0", "--outputdir", str(folder), str(project_file)],
                           capture_output=True, text=True, timeout=1800)
    except FileNotFoundError:
        print(f"  skipped: cannot start {bambu[0]}")
        return
    if r.returncode or not result.exists():
        raise SystemExit(f"slicing failed (exit code {r.returncode}):\n{(r.stdout + r.stderr)[-1500:]}")
    data = json.loads(result.read_text())
    for plate in data.get("sliced_plates", []):
        secs = int(plate.get("total_predication", plate.get("main_predication", 0)))
        grams = sum(f.get("total_used_g", 0) for f in plate.get("filaments", []))
        print(f"  plate {plate.get('id')}: {secs // 3600} h {secs % 3600 // 60:02d} min, {grams:.1f} g")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=str(Path.home() / ".cache/platinenlupe-enclosure"),
                    help="working folder for all intermediate files (default: ~/.cache/platinenlupe-enclosure)")
    ap.add_argument("--print-dir", default=str(HERE / "print"),
                    help="where the printable STLs and the 3MF are copied (default: print/ next to this file)")
    ap.add_argument("--openscad", help="OpenSCAD command (overrides OPENSCAD)")
    ap.add_argument("--bambu-studio", help="Bambu Studio command (overrides BAMBU_STUDIO)")
    ap.add_argument("--no-slice", action="store_true", help="skip the test slice")
    a = ap.parse_args()
    work = Path(a.work).expanduser()
    work.mkdir(parents=True, exist_ok=True)
    scad = openscad_cmd(a.openscad)
    subprocess.run([sys.executable, str(HERE / "lettering.py")], check=True)
    print("Exporting STL ..."); export(scad, work)
    print("Checking meshes ..."); check_meshes(work)
    print("Checking collisions ..."); check_collisions(scad, work)
    target = project(work, Path(a.print_dir).expanduser()); print("3MF:", target)
    if not a.no_slice:
        print("Test slice ...")
        slice_test(tool("BAMBU_STUDIO", a.bambu_studio, [["bambu-studio"]]), target)


if __name__ == "__main__":
    main()
