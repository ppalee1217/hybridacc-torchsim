"""Operator coordinates of every tile, and the check that joins them with the schedule IR (M21 WP-I).

    python -m hybridacc_cc.wave_map check WAVE_MAP.json HARDWARE_IR.json SCHEDULE_IR.json WORKLOAD_IR.json
        [-o REPORT.json]

cc writes ``wave_map.json`` (schema hybridacc-wave-map/v1) beside ``hardware_ir.json``.  For each layer it
records, per tile-grid axis (``oc``, ``h``, ``w``, ``ic`` as in hardware_ir's ``tiling_params`` and the
schedule IR's ``covers``), the operator axis it tiles and one ``[start, end, allocated]`` segment per tile
index: the valid half-open range in operator elements and the extent the lowering sized the wave's SPM
footprint for.  The values are the lowering's own variables (``LayerHwConfig.wave_geometry``); the map is
bound to the sha256 of the hardware IR text and adds nothing to it, so the firmware, hardware_ir.json and
the schedule IR are byte-for-byte what they were before the map existed.

The check is structural and exact: the segments of every axis cover [0, size) once, in order; the tile
counts equal hardware_ir's; the axis sizes, kernel, stride and padding equal the operator's in the frontend's
workload IR (``workload_ir.json``, written before lowering), so the multiply-accumulates over all tiles
equal the operator's; every schedule IR execution wave's ``covers`` names existing tiles and the waves
cover the grid once.  It also derives the
per-layer strata that ODR D-34's C1 asks for (multi-round reduction, reduction tail, output tail, padding,
residency).  It does not prove what the hardware computed; the golden comparison and an independent decode
of the firmware's DMA do that.
"""
import argparse
import hashlib
import json
from pathlib import Path

from . import __version__

SCHEMA = "hybridacc-wave-map/v1"
GRID = ("oc", "h", "w", "ic")
# The tiling_params fields that make tiles stay resident across waves, per operator, and the value that
# means "none" (conv1x1 keeps 1 OC tile at a time unless it is > 1; the GEMM fields are counts, 0 = none).
RESIDENT_FIELDS = {
    "conv2d_1x1": {"conv1x1_resident_oc_tiles": 1},
    "conv2d_3x3": {},
    "gemm": {"gemm_resident_m_tiles": 0, "gemm_resident_n_tiles": 0, "gemm_pd_preload_m_tiles": 0},
}


class WaveMapError(ValueError):
    pass


def _require(ok, message):
    if not ok:
        raise WaveMapError(message)


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()


def build(hardware_ir, hardware_ir_text: str) -> dict:
    """The wave map of a lowered HardwareIR whose hardware_ir.json text is ``hardware_ir_text``."""
    hw = json.loads(hardware_ir_text)
    layers = []
    for index, (layer, hw_layer) in enumerate(zip(hardware_ir.layers, hw["layers"])):
        geo = layer.wave_geometry
        _require(geo is not None, f"layer {index} ({layer.op_type}) has no wave geometry")
        tp = hw_layer["tiling_params"]
        layers.append({
            "index": index,
            "name": layer.name,
            "op_type": geo.op_type,
            "dtype": geo.dtype,
            "kernel": list(geo.kernel),
            "stride": geo.stride,
            "padding": geo.padding,
            "axes": [{"grid": a.grid, "axis": a.axis, "size": a.size, "reduction": a.reduction,
                      "segments": [list(s) for s in a.segments]} for a in geo.axes],
            "resident": {f: tp[f] for f in RESIDENT_FIELDS[geo.op_type]},
            "gemm_k_wave": hw_layer.get("gemm_k_wave"),
            # PE mapping and dataflow: what the scan chain, PE program, HDDU planes and cluster mapping set.
            "pe_mapping_sha256": _sha256_bytes(_canonical({k: hw_layer[k] for k in (
                "scan_chain", "pe_program", "hddu", "cluster_mapping", "target_cluster_mask")})),
        })
    return {
        "schema": SCHEMA,
        "workload_name": hw["workload_name"],
        "hardware_ir_sha256": _sha256_bytes(hardware_ir_text.encode("utf-8")),
        "config_fingerprint": hw["config_fingerprint"],
        "producer": {"module": "hybridacc_cc.wave_map", "package_version": __version__,
                     "sha256": _sha256_bytes(Path(__file__).read_bytes())},
        "layers": layers,
    }


def write_wave_map(hardware_ir, hardware_ir_text: str, out_dir: Path) -> Path:
    path = Path(out_dir) / "wave_map.json"
    path.write_text(json.dumps(build(hardware_ir, hardware_ir_text), indent=1) + "\n", encoding="utf-8")
    return path


def _axis_checks(layer_name, axis, count):
    segs = axis["segments"]
    what = f"{layer_name} axis {axis['grid']} ({axis['axis']})"
    _require(len(segs) == count, f"{what}: {len(segs)} segments, hardware_ir has {count} tiles")
    _require(count >= 1, f"{what}: no tiles")
    pos = 0
    for i, (start, end, allocated) in enumerate(segs):
        _require(start == pos, f"{what}: tile {i} starts at {start}, expected {pos}")
        _require(end > start, f"{what}: tile {i} is empty [{start}, {end})")
        _require(allocated >= end - start, f"{what}: tile {i} covers {end - start} but allocates {allocated}")
        pos = end
    _require(pos == axis["size"], f"{what}: tiles end at {pos}, the axis has {axis['size']}")


def strata(layer: dict) -> dict:
    """ODR D-34 C1 strata of one wave-map layer."""
    red = [a for a in layer["axes"] if a["reduction"]]
    out = [a for a in layer["axes"] if not a["reduction"]]
    length = lambda s: s[1] - s[0]
    return {
        "multi_round_reduction": any(len(a["segments"]) > 1 for a in red),
        "reduction_tail": any(length(a["segments"][-1]) < a["segments"][-1][2] for a in red),
        "output_tail": any(len(a["segments"]) > 1 and length(a["segments"][-1]) < length(a["segments"][0])
                           for a in out),
        "allocated_padding": any(length(s) < s[2] for a in layer["axes"] for s in a["segments"]),
        "input_padding": layer["padding"] > 0,
        "resident": any(v != RESIDENT_FIELDS[layer["op_type"]][f] for f, v in layer["resident"].items()),
    }


def operator_axes(op: dict) -> dict:
    """Axis sizes, kernel, stride and padding of one workload-IR op (frontend shapes, not the lowering's)."""
    ins, outs, attrs = op["inputs"], op["outputs"], op.get("attrs", {})
    if op["op_type"] == "gemm":
        (m, k), (k2, n) = ins[0]["shape"], ins[1]["shape"]
        _require(k == k2, f"op {op['name']}: GEMM operands disagree on K")
        return {"sizes": {"m": m, "n": n, "unit": 1, "k": k}, "kernel": [1, 1], "stride": 1, "padding": 0}
    _require(op["op_type"] in ("conv2d_1x1", "conv2d_3x3"), f"op {op['name']}: unknown op type {op['op_type']}")
    batch, _, _, c = ins[0]["shape"]
    oc, kh, kw, c2 = ins[1]["shape"]
    _, ho, wo, oc2 = outs[0]["shape"]
    _require(batch == 1 and c == c2 and oc == oc2, f"op {op['name']}: shapes disagree or batch > 1")
    return {"sizes": {"oc": oc, "h_out": ho, "w_out": wo, "ic": c}, "kernel": [kh, kw],
            "stride": int(attrs.get("stride", 1)), "padding": int(attrs.get("padding", 0))}


def check(wave_map: dict, hardware_ir_bytes: bytes, schedule_ir: dict, workload_ir: dict) -> dict:
    """Raise WaveMapError on the first violation; return per-layer facts and strata."""
    _require(wave_map.get("schema") == SCHEMA, f"wave map schema must be {SCHEMA}")
    _require(wave_map.get("hardware_ir_sha256") == _sha256_bytes(hardware_ir_bytes),
             "wave map was made for another hardware IR (sha256 differs)")
    hw = json.loads(hardware_ir_bytes)
    _require(wave_map.get("config_fingerprint") == hw["config_fingerprint"], "config fingerprint differs")
    _require(len(wave_map["layers"]) == len(hw["layers"]) == len(schedule_ir["layers"]),
             "layer count differs between the wave map, hardware IR and schedule IR")
    ops = {op["name"]: op for op in workload_ir["ops"]}
    report = []
    for layer, hw_layer, ir_layer in zip(wave_map["layers"], hw["layers"], schedule_ir["layers"]):
        name = f"layer {layer['index']}"
        _require(layer["op_type"] == hw_layer["op_type"], f"{name}: op type differs from hardware IR")
        axes = {a["grid"]: a for a in layer["axes"]}
        _require(tuple(a["grid"] for a in layer["axes"]) == GRID, f"{name}: axes must be {GRID}")
        tp = hw_layer["tiling_params"]
        for g in GRID:
            _axis_checks(name, axes[g], tp[f"num_{g}_tiles"])
        _require([a["reduction"] for a in layer["axes"]] == [False, False, False, True],
                 f"{name}: only the ic axis is a reduction")
        _require(layer["name"] in ops, f"{name}: no workload-IR op named {layer['name']!r}")
        want = operator_axes(ops[layer["name"]])
        got = {a["axis"]: a["size"] for a in layer["axes"]}
        _require(got == want["sizes"], f"{name}: axis sizes {got} differ from the operator's {want['sizes']}")
        for f in ("kernel", "stride", "padding"):
            _require(layer[f] == want[f], f"{name}: {f} {layer[f]} differs from the operator's {want[f]}")
        kh, kw = want["kernel"]
        operator_macs = kh * kw
        for v in want["sizes"].values():
            operator_macs *= v
        tile_macs = 0
        counts = [len(axes[g]["segments"]) for g in GRID]
        for oc in range(counts[0]):
            for h in range(counts[1]):
                for w in range(counts[2]):
                    for ic in range(counts[3]):
                        n = kh * kw
                        for g, i in zip(GRID, (oc, h, w, ic)):
                            s = axes[g]["segments"][i]
                            n *= s[1] - s[0]
                        tile_macs += n
        _require(tile_macs == operator_macs, f"{name}: tiles hold {tile_macs} MACs, the operator {operator_macs}")
        seen = {}
        for wave in ir_layer["execution_waves"]:
            for c in map(tuple, wave["covers"]):
                _require(len(c) == 4 and all(0 <= c[i] < counts[i] for i in range(4)),
                         f"{name}: wave {wave['id']} covers {list(c)}, outside the tile grid {counts}")
                seen[c] = seen.get(c, 0) + 1
        grid_size = counts[0] * counts[1] * counts[2] * counts[3]
        _require(len(seen) == grid_size and all(v == 1 for v in seen.values()),
                 f"{name}: execution waves do not cover the {grid_size} tiles once")
        report.append({"layer": layer["index"], "op_type": layer["op_type"], "tiles": grid_size,
                       "waves": len(ir_layer["execution_waves"]), "macs": operator_macs,
                       "strata": strata(layer)})
    return {"schema": "hybridacc-wave-map-check/v1", "ok": True, "layers": report}


def wave_ranges(wave_map: dict, layer_index: int, covers) -> list:
    """Per covered tile, {axis: [start, end, allocated]} for one execution wave."""
    layer = wave_map["layers"][layer_index]
    out = []
    for c in covers:
        out.append({a["axis"]: list(a["segments"][i]) for a, i in zip(layer["axes"], c)})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("wave_map", type=Path)
    c.add_argument("hardware_ir", type=Path)
    c.add_argument("schedule_ir", type=Path)
    c.add_argument("workload_ir", type=Path)
    c.add_argument("-o", "--output", type=Path)
    a = ap.parse_args(argv)
    result = check(json.loads(a.wave_map.read_text()), a.hardware_ir.read_bytes(),
                   json.loads(a.schedule_ir.read_text()), json.loads(a.workload_ir.read_text()))
    text = json.dumps(result, indent=1) + "\n"
    if a.output:
        a.output.write_text(text)
    print(json.dumps({"ok": True, "layers": len(result["layers"])}))


if __name__ == "__main__":
    main()
