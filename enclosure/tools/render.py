#!/usr/bin/env python3
"""Render the README pictures from the STLs that build.py wrote -- a small software rasteriser, no OpenGL.

Why not OpenSCAD's own PNG export: in the OpenSCAD 2021.01 Flatpak it fails without a usable offscreen
GL context ("Assertion 'get() != pointer()' failed", empty PNG).

  python tools/render.py --work ~/.cache/platinenlupe-enclosure --out ../docs/images

Writes case_parts.png (the three print plates). The assembled view, case_front.jpg, comes from render_blender.py.
Needs numpy, trimesh, Pillow.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import trimesh
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bambu_3mf import Project  # noqa: E402

BEIGE = "#E8D9B5"
RED = "#A3121A"
BROWN = "#5E3F27"
BLACK = "#141414"
PLATE = "#D9DADC"
BACKGROUND = (247, 245, 240)


def rgb(h: str) -> np.ndarray:
    return np.array([int(h[i:i + 2], 16) for i in (1, 3, 5)], float) / 255


def view_matrix(azim: float, elev: float) -> np.ndarray:
    """Rotation into view space: +x right, +y up, +z towards the viewer. azim turns about y, elev tilts down."""
    a, e = np.radians(azim), np.radians(elev)
    ry = np.array([[np.cos(a), 0, np.sin(a)], [0, 1, 0], [-np.sin(a), 0, np.cos(a)]])
    rx = np.array([[1, 0, 0], [0, np.cos(e), -np.sin(e)], [0, np.sin(e), np.cos(e)]])
    return rx @ ry


def render(meshes: list[tuple[trimesh.Trimesh, str]], rot: np.ndarray, size: tuple[int, int],
           ssaa: int = 2, fill: float = 0.86) -> Image.Image:
    W, H = size[0] * ssaa, size[1] * ssaa
    pts = np.vstack([m.vertices for m, _ in meshes]) @ rot.T
    lo, hi = pts.min(0), pts.max(0)
    centre = (lo + hi) / 2
    scale = fill * min(W / (hi[0] - lo[0]), H / (hi[1] - lo[1]))
    img = np.empty((H, W, 3))
    img[:] = np.array(BACKGROUND, float) / 255
    depth = np.full((H, W), -np.inf)
    key = np.array([-0.45, 0.6, 0.65]); key /= np.linalg.norm(key)
    fill_l = np.array([0.6, -0.1, 0.8]); fill_l /= np.linalg.norm(fill_l)
    half = (key + np.array([0, 0, 1.0])); half /= np.linalg.norm(half)
    for m, colour in meshes:
        v = (m.vertices @ rot.T - centre)
        sx = v[:, 0] * scale + W / 2
        sy = H / 2 - v[:, 1] * scale
        sz = v[:, 2]
        normals = m.face_normals @ rot.T
        base = rgb(colour)
        for tri, n in zip(m.faces, normals):
            if n[2] <= 0:          # back face
                continue
            x, y, z = sx[tri], sy[tri], sz[tri]
            x0, x1 = max(0, int(x.min())), min(W - 1, int(x.max()) + 1)
            y0, y1 = max(0, int(y.min())), min(H - 1, int(y.max()) + 1)
            if x1 < x0 or y1 < y0:
                continue
            d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
            if abs(d) < 1e-12:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
            w1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
            block = depth[y0:y1 + 1, x0:x1 + 1]
            new = inside & (zz > block)
            if not new.any():
                continue
            block[new] = zz[new]
            light = 0.45 + 0.50 * max(0.0, n @ key) + 0.15 * max(0.0, n @ fill_l)
            spec = 0.10 * max(0.0, n @ half) ** 24
            img[y0:y1 + 1, x0:x1 + 1][new] = np.clip(base * light + spec, 0, 1)
    # thin dark outline where the depth jumps (silhouettes and steps)
    hit = np.isfinite(depth)
    dz = np.where(hit, depth, depth[hit].min() - 50 if hit.any() else 0)
    jump = np.zeros_like(hit)
    lim = 1.2
    jump[1:, :] |= np.abs(dz[1:, :] - dz[:-1, :]) > lim
    jump[:, 1:] |= np.abs(dz[:, 1:] - dz[:, :-1]) > lim
    img[jump] *= 0.55
    out = Image.fromarray((img * 255).round().astype(np.uint8))
    return out.resize(size, Image.LANCZOS) if ssaa > 1 else out


def load(work: Path, name: str) -> trimesh.Trimesh:
    return trimesh.load(work / f"{name}.stl", force="mesh")


def case_parts(work: Path) -> Image.Image:
    plates = [[("body", BEIGE), ("lid", BEIGE), ("hooks", BEIGE)],
              [("ring", RED), ("stripe", RED), ("light_frames", RED), ("fin", RED),
               ("quiver", RED), ("saturn", RED), ("slider", RED)],
              [(("badge", "badge_platinen", "badge_lupe"), None)]]
    # Parts as build.py places them on each plate (same packing as the 3MF), plates stacked front to back.
    # Each plate is drawn only as large as its parts plus a margin, otherwise the picture is mostly empty bed.
    scene, margin, gap, y_top = [], 8.0, 14.0, 0.0
    for objects in plates:
        loaded = []
        for names, colour in objects:
            if isinstance(names, tuple):   # three-colour badge: red plate, PLATINEN brown, LUPE black
                group = [(load(work, n), c) for n, c in zip(names, (RED, BROWN, BLACK))]
            else:
                group = [(load(work, names), colour)]
            lo = np.min([m.bounds[0] for m, _ in group], axis=0)
            hi = np.max([m.bounds[1] for m, _ in group], axis=0)
            loaded.append((group, lo, hi))
        sizes = [(hi[0] - lo[0], hi[1] - lo[1]) for _, lo, hi in loaded]
        centres = Project._arrange(sizes)
        x0 = min(cx - w / 2 for (cx, _), (w, _) in zip(centres, sizes)) - margin
        x1 = max(cx + w / 2 for (cx, _), (w, _) in zip(centres, sizes)) + margin
        y0 = min(cy - d / 2 for (_, cy), (_, d) in zip(centres, sizes)) - margin
        y1 = max(cy + d / 2 for (_, cy), (_, d) in zip(centres, sizes)) + margin
        dx, dy = -(x0 + x1) / 2, y_top - y1           # centre in x, stack downwards (towards the viewer)
        bed = trimesh.creation.box(extents=[x1 - x0, y1 - y0, 1.0])
        bed.apply_translation([0, (y0 + y1) / 2 + dy, -0.5])
        scene.append((bed, PLATE))
        for (group, lo, hi), (cx, cy) in zip(loaded, centres):
            for m, colour in group:
                m = m.copy()
                m.apply_translation([cx + dx - (lo[0] + hi[0]) / 2, cy + dy - (lo[1] + hi[1]) / 2, -lo[2]])
                scene.append((m, colour))
        y_top += -(y1 - y0) - gap
    # bed in x/y with z up = straight top view; tilt the back away to see it from the front and above
    rot = view_matrix(0, -38)
    return render(scene, rot, (1600, 1200), fill=0.92)


def save(img: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    img = img.quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    img.save(path, optimize=True)
    print(path.name, path.stat().st_size // 1024, "KB")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=str(Path.home() / ".cache/platinenlupe-enclosure"))
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "docs/images"))
    a = ap.parse_args()
    work, out = Path(a.work).expanduser(), Path(a.out).expanduser()
    save(case_parts(work), out / "case_parts.png")


if __name__ == "__main__":
    main()
