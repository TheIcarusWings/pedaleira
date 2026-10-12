#!/usr/bin/env python3
"""Reads presets from your own Quad Cortex and writes Pedaleira "foreign preset" JSON.

This is the Quad Cortex reader of Pedaleira's importer (docs/import.md). It
uses pyquadcortex (https://github.com/stokes-audio/pyquadcortex, MIT), an
unofficial library that talks to the unit over USB with the protocol the unit
already speaks, and writes what it reads as the neutral format of
docs/import.md. Nothing is decoded, decrypted or taken from Neural DSP's
software: the script only asks the unit for the preset data it hands to any
USB client, and keeps block names, parameter values (in the units the screen
shows), scenes and routing.

Read only. Nothing here writes to the unit: no set_*, save_*, delete_* or
move_* call exists in this file. One caveat comes from the library, not from
us: the unit offers no way to read a stored preset without loading it, so
reading by slot or name RECALLS that preset on the unit (the audio cuts, and
unsaved edits on the unit are discarded). `--current` reads what is on the
grid now and recalls nothing.

Run it with the unit connected by USB and Cortex Control quit (tools/import/README.md):

    python3 quadcortex_dump.py --current --out dumps/
    python3 quadcortex_dump.py --slot 28C --out dumps/
    python3 quadcortex_dump.py --preset "Brit 2203" --setlist factory --out dumps/
    python3 quadcortex_dump.py --all --setlist user --yes --out dumps/

The translation from pyquadcortex's objects to JSON is `build_foreign_preset`,
a pure function that gets the pyquadcortex helpers through a small `Toolkit`,
so it runs (and is tested) with fake objects and no library or unit.
"""

import argparse
import json
import math
import os
import re
import sys
from typing import Any, Callable, Dict, List, NamedTuple, Optional

FORMAT = "pedaleira-foreign-preset"
VERSION = 1
SOURCE = "quad-cortex"
SCENE_COUNT = 8


class Toolkit(NamedTuple):
    """The pyquadcortex helpers the translation needs, as plain callables.

    The real ones come from `pyquadcortex_toolkit()`; the tests pass fakes.
    """

    blocks: Callable[[Any], list]  # preset -> [object with row, column, model_id]
    splits: Callable[[Any], list]  # preset -> [object with row, split_column, mix_column]
    bypass_state: Callable[[Any, Any], Any]  # (preset, cell) -> .scene_mode, .scenes
    param_state: Callable[[Any, Any, int], Any]  # (preset, cell, index) -> .scene_mode, .values
    input_name: Callable[[int], str]  # in_portid -> "INPUT_1"
    output_name: Callable[[int], str]  # out_portid -> "XLR_1_2"
    tempo_bpm: Callable[[Any], Optional[float]]  # preset -> BPM or None


# ####################
# TRANSLATION (pure)
# ####################


def _present(message: Any, field: str) -> bool:
    """HasField without the exception some fields raise (pyquadcortex.field_present)."""
    try:
        return bool(message.HasField(field))
    except (ValueError, AttributeError):
        return False


def _plain_number(value: Any) -> float:
    """A pyquadcortex Value (a float subclass) as a plain float."""
    return float(value)


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _clean_number(value: float) -> float:
    """Six significant digits, so the JSON is stable and readable."""
    return float(f"{value:.6g}")


def convert_value(spec: Any, stored: Any) -> Optional[Dict[str, Any]]:
    """One stored parameter value as { "value": ..., "unit": ... }, or None.

    `stored` is what pyquadcortex's param_state hands back: an Encoded (0..1 on
    the unit's own line), a string, or None. `spec` is the catalog Parameter.
    A list parameter becomes the option's name, a switch a bool-like option
    name, a plain knob its value in the screen's units. A value the library
    cannot convert (NaN in a factory preset, an unmeasured range) is None.
    """
    if stored is None:
        return None
    if isinstance(stored, str):
        return {"value": stored}
    if not _finite(stored):
        return None
    wire = _plain_number(stored)
    options = tuple(getattr(spec, "options", ()) or ())
    count = getattr(spec, "option_count", None)
    if count is not None:
        index = int(spec.value_to_option(wire))
        if options and 0 <= index < len(options):
            return {"value": options[index]}
        return {"value": index}
    try:
        real = _plain_number(spec.to_real(wire))
    except (ValueError, TypeError):
        return None
    if not math.isfinite(real):
        return None
    result: Dict[str, Any] = {"value": _clean_number(real)}
    unit = getattr(spec, "units", "") or ""
    if unit:
        result["unit"] = unit
    return result


def _scene_values(state: Any, scene: int) -> Any:
    """The stored value of a scene-following parameter, else the first slot's."""
    values = list(state.values)
    if state.scene_mode and scene < len(values):
        return values[scene]
    return values[0] if values else None


# The unit's port names (pyquadcortex Input / Output) as the neutral port
# descriptions of docs/import.md. "Input 1" is a jack, so channel 1; a "1/2"
# name is a stereo pair; anything else (return jacks, USB) is passed on by
# name only. EMPTY means the row is fed by a split or another row.
_INPUT_JACKS = {"INPUT_1": (1, False), "INPUT_2": (2, False), "INPUT_1_2": (1, True)}
_OUTPUT_PAIRS = {"XLR_1_2": 1, "OUT_3_4": 2}
_ROW_FEEDS = {"NEXT_ROW_3", "NEXT_ROW_4", "NEXT_ROW_3_4"}


def describe_input(name: str) -> Dict[str, Any]:
    if name == "EMPTY":
        return {"type": "none", "name": name}
    if name in _INPUT_JACKS:
        channel, stereo = _INPUT_JACKS[name]
        return {"type": "device", "channel": channel, "stereo": stereo, "name": name}
    return {"type": "other", "name": name}


def describe_output(name: str) -> Dict[str, Any]:
    if name == "EMPTY":
        return {"type": "none", "name": name}
    if name in _OUTPUT_PAIRS:
        return {"type": "device", "pair": _OUTPUT_PAIRS[name], "name": name}
    if name in _ROW_FEEDS:
        return {"type": "row", "name": name}
    return {"type": "other", "name": name}


def _key(row: int, column: int) -> str:
    return f"{row}:{column}"


def build_foreign_preset(
    preset: Any,
    catalog: Any,
    kit: Toolkit,
    source_version: str = "",
    scene_count: int = SCENE_COUNT,
) -> Dict[str, Any]:
    """A pyquadcortex preset as the foreign preset dictionary of docs/import.md.

    `preset` is a BinaryPreset (read_preset / read_current_preset), `catalog`
    the connected unit's model catalog (`qc.catalog`, indexable by model id).
    Rows and columns are zero-based, as in pyquadcortex (the unit shows rows
    1 to 4). Never raises on one odd block: it is left out and named in
    "warnings".
    """
    warnings: List[str] = []
    default_scene = int(preset.default_scene) if _present(preset, "default_scene") else 0
    if not 0 <= default_scene < scene_count:
        default_scene = 0

    rows: Dict[int, List[Dict[str, Any]]] = {}
    scenes_enabled: List[Dict[str, bool]] = [dict() for _ in range(scene_count)]
    scenes_params: List[Dict[str, Dict[str, Any]]] = [dict() for _ in range(scene_count)]

    for cell in sorted(kit.blocks(preset), key=lambda c: (c.row, c.column)):
        model = catalog.get(cell.model_id)
        where = f"row {cell.row + 1} column {cell.column + 1}"
        if model is None:
            warnings.append(f"{where}: model id {cell.model_id} is not in the unit's catalog")
            continue

        bypass = kit.bypass_state(preset, cell)
        scenes_bypass = list(bypass.scenes)

        def enabled_in(scene: int) -> bool:
            if bypass.scene_mode and scene < len(scenes_bypass):
                return not scenes_bypass[scene]
            return not scenes_bypass[0] if scenes_bypass else True

        params: Dict[str, Any] = {}
        for spec in getattr(model, "parameters", ()):
            if getattr(spec, "hidden", False):
                continue
            try:
                state = kit.param_state(preset, cell, spec.index)
            except (IndexError, ValueError):
                continue
            base = convert_value(spec, _scene_values(state, default_scene))
            if base is None:
                continue
            params[spec.name] = base
            if state.scene_mode:
                for scene in range(scene_count):
                    value = convert_value(spec, _scene_values(state, scene))
                    if value is not None and value != base:
                        scenes_params[scene].setdefault(_key(cell.row, cell.column), {})[spec.name] = value["value"]

        block = {
            "position": cell.column,
            "model": model.name,
            "category": model.category,
            "enabled": enabled_in(default_scene),
            "params": params,
        }
        rows.setdefault(cell.row, []).append(block)
        if bypass.scene_mode:
            for scene in range(scene_count):
                scenes_enabled[scene][_key(cell.row, cell.column)] = enabled_in(scene)

    labels = list(getattr(preset, "scene_labels", []) or [])
    scenes: List[Dict[str, Any]] = []
    for scene in range(scene_count):
        label = labels[scene].strip() if scene < len(labels) and labels[scene] else ""
        scenes.append(
            {
                "name": label or f"Scene {chr(ord('A') + scene)}",
                "enabled": scenes_enabled[scene],
                "params": scenes_params[scene],
            }
        )

    routing_rows = []
    for index, chain in enumerate(preset.chains):
        row = int(chain.row) if _present(chain, "row") else index
        entry: Dict[str, Any] = {"index": row}
        if _present(chain, "in_portid"):
            entry["input"] = describe_input(kit.input_name(int(chain.in_portid)))
        if _present(chain, "out_portid"):
            entry["output"] = describe_output(kit.output_name(int(chain.out_portid)))
        routing_rows.append(entry)
    routing = {
        "rows": routing_rows,
        "splits": [
            {
                "row": int(s.row),
                "splitAt": int(s.split_column),
                "mixAt": int(s.mix_column) if int(s.mix_column) >= 0 else None,
            }
            for s in kit.splits(preset)
        ],
    }

    result: Dict[str, Any] = {
        "format": FORMAT,
        "version": VERSION,
        "source": SOURCE,
        "sourceVersion": source_version,
        "name": preset.name if _present(preset, "name") else "",
    }
    bpm = kit.tempo_bpm(preset)
    if bpm is not None and math.isfinite(bpm):
        result["tempo"] = round(bpm, 2)
    result["defaultScene"] = default_scene
    result["rows"] = [{"index": row, "blocks": rows[row]} for row in sorted(rows)]
    result["routing"] = routing
    result["scenes"] = scenes
    if warnings:
        result["warnings"] = warnings
    return result


def safe_file_name(slot: str, name: str) -> str:
    """`28C Brit 2203.json`, with anything a file system dislikes turned into '-'."""
    stem = re.sub(r'[\\/:*?"<>|\x00-\x1f]+', "-", f"{slot} {name}".strip()).strip(" .-")
    return (stem or "preset")[:100] + ".json"


# ####################
# THE UNIT (needs pyquadcortex)
# ####################


def pyquadcortex_toolkit(catalog: Any = None) -> Toolkit:
    """The real helpers (pyquadcortex 0.40 and newer, whose helpers are top-level
    names). pyquadcortex is imported here, not at module load. `catalog` is the
    connected unit's model catalog, which turns the stored tempo into BPM."""
    try:
        import pyquadcortex as qclib
    except ImportError as error:  # pragma: no cover - needs the library
        raise SystemExit(
            "pyquadcortex is not installed. Run: pip install pyquadcortex (Python 3.11 or newer, "
            "plus the hidapi library; see tools/import/README.md)\n" + str(error)
        )

    def input_name(port: int) -> str:
        try:
            return qclib.Input(port).name
        except ValueError:
            return f"UNKNOWN_{port}"

    def output_name(port: int) -> str:
        try:
            return qclib.Output(port).name
        except ValueError:
            return f"UNKNOWN_{port}"

    def tempo_bpm(preset: Any) -> Optional[float]:
        # The preset's tempo control (model 25000) stores TEMPO at index 0 as a
        # normalized 0..1; the catalog's range for it gives BPM.
        try:
            stored = qclib.tempo_params(preset)[qclib.QuadCortex.TEMPO_PARAMS["TEMPO"]]
            model = catalog[qclib.QuadCortex.TEMPO_CONTROL]
            return float(model.parameters[qclib.QuadCortex.TEMPO_PARAMS["TEMPO"]].to_real(stored))
        except (KeyError, IndexError, ValueError, AttributeError, TypeError):
            return None

    return Toolkit(
        blocks=qclib.blocks,
        splits=qclib.splits,
        bypass_state=lambda preset, cell: qclib.bypass_state(preset, cell.row, cell.column),
        param_state=lambda preset, cell, index: qclib.param_state(preset, cell.row, cell.column, index),
        input_name=input_name,
        output_name=output_name,
        tempo_bpm=tempo_bpm,
    )


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read presets from your own Quad Cortex (read only) and write Pedaleira foreign preset JSON."
    )
    what = parser.add_mutually_exclusive_group(required=True)
    what.add_argument("--current", action="store_true", help="the preset on the grid now (recalls nothing)")
    what.add_argument("--preset", metavar="NAME", help="one preset by name (recalls it on the unit)")
    what.add_argument("--slot", metavar="SLOT", help='one preset by slot name, such as "28C" (recalls it)')
    what.add_argument("--all", action="store_true", help="every preset of --setlist (recalls each in turn)")
    parser.add_argument("--setlist", choices=["user", "factory"], default="user", help="default: user (My Presets)")
    parser.add_argument("--out", required=True, metavar="DIR", help="folder for the JSON files")
    parser.add_argument(
        "--yes",
        action="store_true",
        help="needed with --all: confirms that each preset is loaded on the unit in turn",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    if args.all and not args.yes:
        print(
            "--all loads every preset of the setlist on the unit, one after the other (the audio "
            "cuts each time; nothing is written). Add --yes to go ahead.",
            file=sys.stderr,
        )
        return 2
    pyquadcortex_toolkit()  # fails early with install advice when the library is missing
    import pyquadcortex as qclib

    setlist = qclib.Setlist.FACTORY if args.setlist == "factory" else qclib.Setlist.USER
    os.makedirs(args.out, exist_ok=True)
    written = 0
    with qclib.connect() as qc:
        kit = pyquadcortex_toolkit(qc.catalog)
        version = ""
        try:
            version = str(qc.version().app_fw_version)
        except Exception:  # the version is information only
            pass
        source_version = f"CorOS {version}".strip() if version else ""

        jobs = []  # (slot label, preset object)
        if args.current:
            jobs.append(("current", qc.read_current_preset()))
        elif args.all:
            for entry in qc.list_presets(setlist):
                label = qclib.position_to_slot(entry.index)
                jobs.append((label, qc.read_preset(setlist, entry.index)))
        elif args.slot:
            jobs.append((args.slot.upper(), qc.read_preset(setlist, args.slot.upper())))
        else:
            entry = qc.find_preset(args.preset, setlist)
            jobs.append((qclib.position_to_slot(entry.index), qc.read_preset(setlist, entry.index)))

        for label, preset in jobs:
            document = build_foreign_preset(preset, qc.catalog, kit, source_version)
            path = os.path.join(args.out, safe_file_name(label, document["name"]))
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(document, handle, indent=2, ensure_ascii=False)
                handle.write("\n")
            print(f"wrote {path} ({sum(len(r['blocks']) for r in document['rows'])} blocks)")
            written += 1
    print(f"{written} preset(s) written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
