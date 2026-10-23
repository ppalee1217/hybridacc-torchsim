"""Which waves of one schedule share a measured wave profile (M22 condition 3).

    python -m hybridacc_cc.wave_reuse table WAVE_MAP.json SCHEDULE_IR.json -o TABLE.json
    python -m hybridacc_cc.wave_reuse reduce PROFILE.json TABLE.json -o REDUCED.json
    python -m hybridacc_cc.wave_reuse check PROFILE.json TABLE.json [-o REPORT.json]

Ruling 2026-10-24 (personal-research agent_run/261022-m22-reuse-chain/decide-chain/decision.md): within one schedule,
waves are grouped before any run, the first wave of each group in schedule order is its representative, the profile
keeps the representatives only, and the TOG adapter gives each wave its representative's record.

The key (``wave-reuse/v1``) is read from the schedule IR and cc's ``wave_map.json`` only, never from a measurement:
  1. pattern: the layer and, for every tile the wave covers, per operator axis the tile's valid extent, its allocated
     extent and, on a reduction axis, its position (only, first, mid, last); M21 WP-L's key;
  2. context: which axes' covered tiles differ from the previous wave of the same layer ("first" for the layer's
     first wave); the first wave and the first wave after a change of tile group run one cycle longer (dev);
  3. every AGU's SPM base address (the wave's ping/pong buffers) and
  4. the wave's ``spm_config_map``: the SPM claims' bank and group masks follow these two (dev: the claims' times are
     the same within a group, their masks are not without them).
``check`` compares a full profile against a table: every wave's hardware conditions (hddu_done, agu_idle,
pe_halted, cycles after START) and SPM claims must equal its representative's. cluster_quiesced is the cluster's
state (it includes the DMA) and the raw tokens include the firmware's polls; neither is shared: a TOG made from a
reduced profile carries the three wave conditions only, so it needs TOGSim's ``hacc_quiesce_model tls``.
"""
import argparse
import hashlib
import json
from pathlib import Path

SCHEMA = "hybridacc-wave-reuse/v1"
KEY_VERSION = "wave-reuse/v1"
CONDITIONS = ("hddu_done", "agu_idle", "pe_halted")


class WaveReuseError(ValueError):
    pass


def _require(ok, message):
    if not ok:
        raise WaveReuseError(message)


def _sha256_file(path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _pattern(axes, index, covers):
    parts = []
    for cover in covers:
        _require(len(cover) == len(axes), f"layer {index}: a cover names {len(cover)} axes, the map has {len(axes)}")
        tile = []
        for ax, idx in zip(axes, cover):
            segments = ax["segments"]
            _require(0 <= idx < len(segments), f"layer {index}: tile {idx} outside axis {ax['grid']}")
            start, end, alloc = segments[idx]
            pos = ""
            if ax["reduction"]:
                n = len(segments)
                pos = "only" if n == 1 else "first" if idx == 0 else "last" if idx == n - 1 else "mid"
            tile.append([end - start, alloc, pos])
        parts.append(tile)
    return sorted(parts)


def keys(ir: dict, wave_map: dict) -> dict:
    """{(layer, wave): key string} for every execution wave of the schedule IR, in schedule order."""
    maps = {L["index"]: L for L in wave_map["layers"]}
    out = {}
    for layer in ir["layers"]:
        index = layer["index"]
        _require(index in maps, f"wave map has no layer {index}")
        axes = maps[index]["axes"]
        prev = None
        for w in sorted(layer["execution_waves"], key=lambda w: w["id"]):
            covers = w["covers"]
            spans = [sorted({c[i] for c in covers}) for i in range(len(axes))]
            ctx = "first" if prev is None else ",".join(str(i) for i in range(len(axes)) if spans[i] != prev[i])
            prev = spans
            bases = sorted([name, regs.get("BASE_ADDR"), regs.get("BASE_ADDR_H")] for name, regs in w["agu"].items())
            out[(index, w["id"])] = json.dumps([index, _pattern(axes, index, covers), ctx, bases,
                                                w.get("spm_config_map")], separators=(",", ":"))
    return out


def table(ir: dict, wave_map: dict, ir_sha256: str, wave_map_sha256: str) -> dict:
    """The reuse table: each wave's group and representative (the group's first wave in schedule order)."""
    groups, rows = {}, []
    for (layer, wave), key in keys(ir, wave_map).items():
        if key not in groups:
            groups[key] = (len(groups), [layer, wave])
        g, rep = groups[key]
        rows.append({"layer": layer, "wave": wave, "group": g, "representative": rep})
    return {"schema": SCHEMA, "key_version": KEY_VERSION, "ir_sha256": ir_sha256, "wave_map_sha256": wave_map_sha256,
            "producer_sha256": _sha256_file(Path(__file__)), "groups": len(groups), "waves": rows}


def _reps(tab: dict) -> dict:
    _require(tab.get("schema") == SCHEMA and tab.get("key_version") == KEY_VERSION, f"not a {SCHEMA} table")
    return {(r["layer"], r["wave"]): tuple(r["representative"]) for r in tab["waves"]}


def _bind(profile: dict, tab: dict):
    _require(profile.get("ir_sha256") == tab["ir_sha256"], "the profile was made for another IR than the table")
    prov = profile.get("provenance")
    if prov is not None:  # profile v2 binds the wave map too
        _require(prov.get("wave_map_sha256") == tab["wave_map_sha256"],
                 "the profile was made with another wave map than the table")


def reduce(profile: dict, tab: dict, table_sha256: str) -> dict:
    """The profile with the representatives' rows only, bound to the table."""
    reps = _reps(tab)
    _bind(profile, tab)
    rows = {(w["layer"], w["wave"]): w for w in profile["waves"]}
    _require(set(rows) == set(reps), "the profile's waves differ from the table's")
    keep = sorted(set(reps.values()))
    out = {k: v for k, v in profile.items() if k != "waves"}
    out["waves"] = [rows[k] for k in keep]
    out["reuse"] = {"table_sha256": table_sha256, "key_version": KEY_VERSION}
    return out


def _record(row: dict):
    observed = row.get("observed") or {}
    return ({k: observed[k]["condition_minus_start_cycles"] for k in CONDITIONS if k in observed},
            row.get("spm_claims"))


def check(profile: dict, tab: dict) -> dict:
    """Every wave's conditions and SPM claims against its representative's, in a full profile of the schedule."""
    reps = _reps(tab)
    _bind(profile, tab)
    rows = {(w["layer"], w["wave"]): w for w in profile["waves"]}
    _require(set(rows) == set(reps), "the profile's waves differ from the table's")
    differ = []
    for key, rep in sorted(reps.items()):
        if key != rep and _record(rows[key]) != _record(rows[rep]):
            differ.append({"wave": list(key), "representative": list(rep),
                           "conditions_equal": _record(rows[key])[0] == _record(rows[rep])[0],
                           "claims_equal": _record(rows[key])[1] == _record(rows[rep])[1]})
    return {"waves": len(reps), "groups": len(set(reps.values())), "reusable": len(reps) - len(set(reps.values())),
            "differ": differ}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("table"); t.add_argument("wave_map", type=Path); t.add_argument("ir", type=Path)
    t.add_argument("-o", "--output", type=Path, required=True)
    r = sub.add_parser("reduce"); r.add_argument("profile", type=Path); r.add_argument("table", type=Path)
    r.add_argument("-o", "--output", type=Path, required=True)
    c = sub.add_parser("check"); c.add_argument("profile", type=Path); c.add_argument("table", type=Path)
    c.add_argument("-o", "--output", type=Path)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "table":
            out = table(json.loads(a.ir.read_text()), json.loads(a.wave_map.read_text()), _sha256_file(a.ir),
                        _sha256_file(a.wave_map))
            a.output.write_text(json.dumps(out, indent=1) + "\n")
            print(json.dumps({"waves": len(out["waves"]), "groups": out["groups"]}))
        elif a.cmd == "reduce":
            out = reduce(json.loads(a.profile.read_text()), json.loads(a.table.read_text()), _sha256_file(a.table))
            a.output.write_text(json.dumps(out, indent=1) + "\n")
            print(json.dumps({"waves": len(out["waves"])}))
        else:
            out = check(json.loads(a.profile.read_text()), json.loads(a.table.read_text()))
            if a.output:
                a.output.write_text(json.dumps(out, indent=1) + "\n")
            print(json.dumps({k: (len(v) if k == "differ" else v) for k, v in out.items()}))
            return 1 if out["differ"] else 0
    except WaveReuseError as e:
        print(f"wave_reuse: {e}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
