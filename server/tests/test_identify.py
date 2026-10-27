"""Tests for identify.py without a model: name mapping and the crop around the focus ring."""

import io
import pathlib
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from platinenlupe.identify import PARTS, crop, normalize  # noqa: E402


def test_normalize_keeps_known_keys():
    for key in PARTS:
        assert normalize(key) == key


def test_normalize_maps_free_text():
    assert normalize("SMD resistor") == "resistor"
    assert normalize("Integrated circuit") == "ic"
    assert normalize("Ceramic Capacitor") == "ceramic_capacitor"
    assert normalize("electrolytic capacitor") == "electrolytic_capacitor"
    assert normalize("resistor network") == "resistor_array"
    assert normalize("soldered joint") == "solder_joint"
    assert normalize("Zener diode") == "zener_diode"
    assert normalize("something odd") == "unclear"
    assert normalize("") == "unclear"


def _jpeg(w=640, h=480):
    out = io.BytesIO()
    Image.new("RGB", (w, h), (20, 90, 50)).save(out, "JPEG")
    return out.getvalue()


def test_crop_is_square_and_sized():
    img = Image.open(io.BytesIO(crop(_jpeg(), 0.5, 0.5, 0.15)))
    assert img.size == (448, 448)


def test_crop_at_the_edge_still_works():
    assert crop(_jpeg(), 0.0, 1.0, 0.1) is not None


def test_crop_rejects_garbage():
    assert crop(b"not a jpeg", 0.5, 0.5, 0.15) is None
