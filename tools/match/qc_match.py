#!/usr/bin/env python3
"""Automatic effect matching sessions against your own Quad Cortex, over USB.

For each take of a plan the script sets up the Quad Cortex (QC), records its
output through PedaleiraMatch, renders our equivalent effect with
PedaleiraRender over the same test signal, compares the two and writes a
Markdown report (docs/matching.md, "Automatic sessions with a Quad Cortex over
USB"). It uses pyquadcortex (https://github.com/stokes-audio/pyquadcortex, MIT),
an unofficial library that speaks the protocol the unit already speaks. Nothing
is decoded, decrypted or taken from Neural DSP's software.

THE NEVER-SAVE RULE. This script never stores anything on the unit: no preset,
setlist, setting or global. Enforced in code, not by good manners: the unit is
only ever reached through `GuardedClient`, an allow-list proxy that exposes the
read calls and the grid-edit calls a session needs and raises on everything
else, so a call such as save_current_preset cannot be made even by mistake.
Grid edits are not stored: the unit keeps them as unsaved edits of the preset
on the grid, and recalling any preset restores it. A session also refuses to
start when the unit reports unsaved edits (your work is never overwritten).

    python3 qc_match.py --plan plan.json                  # dry run: the plan only
    python3 qc_match.py --plan plan.json --go --dir match # the real session
    python3 qc_match.py --catalog --out qc-reports        # read the block catalogue
    python3 qc_match.py --check-mapping --out qc-reports  # catalogue against the mapping

The planning, mapping, command building and report code are pure and tested
with fake objects (test_qc_match.py); pyquadcortex is imported only when a unit
is really talked to.
"""

import argparse
import array
import datetime
import difflib
import json
import math
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Sequence, Tuple

FORMAT = "pedaleira-match-plan"
CATALOG_FORMAT = "pedaleira-qc-catalog"
VERSION = 1

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MAPPING = REPO / "resources" / "import" / "quad-cortex.json"
CURVES_PATH = REPO / "resources" / "import" / "quad-cortex-curves.json"
# Filled at the bottom of the module, once load_curves exists.
CURVES: Dict[str, Dict[str, float]] = {}

DEFAULT_SCRATCH_PRESET = "Brit 2203"
DEFAULT_QC_INPUT = "USB_5_6"
DEFAULT_QC_OUTPUT = "USB_3_4"
DEFAULT_DEVICE = "Quad Cortex"
DEFAULT_OUT_CH = "5,6"
DEFAULT_IN_CH = "3,4"
DEFAULT_RATE = 48000
DEFAULT_SETTLE_S = 1.5
DEFAULT_TAIL_S = 4.0
DEFAULT_SIGNAL_NAME = "pedaleira-test-signal-v1.wav"

# The port names of pyquadcortex 0.40.0 (enums.Input, enums.Output). A dry run
# checks the names against these without the library; a real session checks
# them against the library's own enums.
KNOWN_INPUTS = (
    "EMPTY", "INPUT_1", "INPUT_2", "INPUT_1_2", "RETURN_1", "RETURN_2", "RETURN_1_2", "PREV_ROW",
    "USB_5", "USB_6", "USB_7", "USB_8", "USB_5_6", "USB_7_8",
)
KNOWN_OUTPUTS = (
    "EMPTY", "XLR_1_2", "OUT_3_4", "SEND_1_2", "XLR_1", "XLR_2", "OUT_3", "OUT_4", "SEND_1", "SEND_2",
    "USB_5", "USB_6", "USB_7", "USB_8", "USB_5_6", "USB_7_8", "MULTIPLE", "USB_3", "USB_4", "USB_3_4",
)

# The settings every block has (docs/commands.md, block.setParam). They are set
# by command after the preset is loaded; every other id is a kind parameter.
COMMON_PARAMS = ("mix", "inputGain", "outputGain", "enabled")

EXIT_OK = 0
EXIT_TAKES_FAILED = 1
EXIT_USAGE = 2
EXIT_NO_DEVICE = 3
EXIT_DIRTY = 4
EXIT_REFUSED = 5

NOTHING_SAVED = (
    "Nothing was saved on the Quad Cortex. Every change was an unsaved edit of the preset on the "
    "grid; recalling any preset on the unit restores it."
)

DIRTY_MESSAGE = (
    "The Quad Cortex says the preset on the grid has unsaved edits. Nothing was touched. Save "
    "them on the unit, or discard them (recall the preset again), then run this again."
)


class PlanError(ValueError):
    """The plan, the options or the mapping cannot be used. Nothing was touched."""


class NeverSaveError(RuntimeError):
    """A call that is not on the allow-list was attempted on the unit."""


class UnitRefused(RuntimeError):
    """The unit is in a state this session will not work on (dirty, no free row)."""

    def __init__(self, message: str, code: int = EXIT_REFUSED):
        super().__init__(message)
        self.code = code


# ####################
# THE NEVER-SAVE GUARD
# ####################

# Reads, and the grid edits one take needs. `recall_preset` and `read_preset`
# load a preset onto the grid (the audio cuts, nothing is written). Everything
# else is refused, in particular every call that stores a preset, a setlist, a
# setting or a global: save_current_preset, write_preset, update_settings,
# set_input_port, set_output_port, set_usb_port, global EQ, MIDI, tuner, mode...
ALLOWED_CALLS = frozenset(
    {
        "version",
        "catalog",
        "find_preset",
        "list_presets",
        "read_preset",
        "recall_preset",
        "read_current_preset",
        "preset_dirty",
        "switch_scene",
        "set_block",
        "remove_block",
        "set_chain_input",
        "set_chain_output",
        "set_param",
        "set_bypass",
        "close",
    }
)

# Named in messages and checked by the tests. Not an exhaustive list of what is
# refused: anything not in ALLOWED_CALLS is.
KNOWN_WRITES = (
    "save_current_preset",
    "write_preset",
    "delete_preset",
    "move_preset",
    "copy_preset",
    "update_settings",
    "set_input_port",
    "set_output_port",
    "set_usb_port",
    "set_input_level",
    "set_output_level",
    "set_global_eq",
    "set_master_volume",
    "set_midi_out",
    "set_mode",
    "show_tuner",
    "create_setlist",
    "delete_setlist",
)

_GUARD_OWN = frozenset({"__class__", "__enter__", "__exit__", "__repr__", "__doc__", "__dict__"})


class GuardedClient:
    """The only way this script reaches the unit: an allow-list proxy.

    Attribute access of anything outside ALLOWED_CALLS raises NeverSaveError,
    and nothing can be assigned. The real client is held in a slot that is not
    reachable by a normal attribute name.
    """

    __slots__ = ("_unit",)

    def __init__(self, unit: Any):
        object.__setattr__(self, "_unit", unit)

    def __getattribute__(self, name: str) -> Any:
        if name in ALLOWED_CALLS:
            return getattr(object.__getattribute__(self, "_unit"), name)
        if name in _GUARD_OWN:
            return object.__getattribute__(self, name)
        raise NeverSaveError(
            f"{name!r} is refused: this session never saves or changes anything but the grid. "
            f"Allowed: {', '.join(sorted(ALLOWED_CALLS))}."
        )

    def __setattr__(self, name: str, value: Any) -> None:
        raise NeverSaveError(f"cannot set {name!r}: the guarded client is read only")

    def __delattr__(self, name: str) -> None:
        raise NeverSaveError(f"cannot delete {name!r}: the guarded client is read only")

    def __enter__(self) -> "GuardedClient":
        return self

    def __exit__(self, kind: Any, error: Any, trace: Any) -> bool:
        self.close()
        return False


# ####################
# NAMES, MAPPING (pure)
# ####################


def norm(text: Any) -> str:
    """Names compared without regard to case, spaces and punctuation (docs/import.md)."""
    return re.sub(r"[^a-z0-9]+", "", str(text).lower())


FORMAT_MARKERS = ("m", "st", "s/c", "sc", "mono", "stereo")


def model_key(name: Any) -> str:
    """A model name as the importer looks it up in the mapping table: "Digital Delay (M)" and
    "Digital Delay (ST)" are one model, so a trailing parenthesis holding only a mono or stereo
    marker is dropped first (stripFormatSuffix in plugin/src/PresetImporter.cpp)."""
    text = str(name).strip()
    if text.endswith(")"):
        open_at = text.rfind("(")
        if open_at > 0 and text[open_at + 1 : -1].strip().lower() in FORMAT_MARKERS:
            text = text[:open_at].strip()
    return norm(text)


def clean_number(value: float) -> float:
    return float(f"{value:.6g}")


def fmt(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return f"{value:g}"
    return str(value)


def safe_stem(name: str) -> str:
    stem = re.sub(r"[^A-Za-z0-9._-]+", "-", str(name)).strip(" .-")
    return stem[:100]


def load_json(path: Path) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise PlanError(f"cannot read {path}: {error}")
    except ValueError as error:
        raise PlanError(f"{path} is not valid JSON: {error}")


def mapping_entry(mapping: Dict[str, Any], model_name: Optional[str]) -> Optional[Dict[str, Any]]:
    if not model_name:
        return None
    wanted = model_key(model_name)
    for name, entry in (mapping.get("models") or {}).items():
        if model_key(name) == wanted:
            return entry
    return None


def apply_transform(transform: Any, value: Any) -> Tuple[bool, Any]:
    """(True, our value) or (False, the reason). docs/import.md, the mapping table."""
    if isinstance(value, bool):
        number: Optional[float] = 1.0 if value else 0.0
    elif isinstance(value, (int, float)):
        number = float(value)
    else:
        number = None
    if transform is None or transform == "linear":
        if number is None:
            return False, f"{value!r} is not a number"
        return True, clean_number(number)
    if isinstance(transform, dict) and ("scale" in transform or "offset" in transform):
        if number is None:
            return False, f"{value!r} is not a number"
        return True, clean_number(number * float(transform.get("scale", 1.0)) + float(transform.get("offset", 0.0)))
    if isinstance(transform, dict) and "map" in transform:
        table = {norm(key): val for key, val in transform["map"].items()}
        for candidate in (value, number if number is None else fmt(number)):
            if candidate is not None and norm(candidate) in table:
                return True, float(table[norm(candidate)])
        return False, f"{value!r} is not one of the mapped values {sorted(transform['map'])}"
    return False, f"unknown transform {transform!r}"


def describe_transform(transform: Any) -> str:
    if transform is None or transform == "linear":
        return "as is"
    if isinstance(transform, dict) and ("scale" in transform or "offset" in transform):
        return f"x {fmt(float(transform.get('scale', 1.0)))} + {fmt(float(transform.get('offset', 0.0)))}"
    if isinstance(transform, dict) and "map" in transform:
        return "map " + ", ".join(f"{key}={val}" for key, val in transform["map"].items())
    return str(transform)


@dataclass
class OursParam:
    ident: str
    value: float
    source: str  # "mapping" or "plan"
    qc_name: str = ""
    qc_value: Any = None
    transform: str = ""


@dataclass
class OursPlan:
    """Our side of a take: a kind and its settings, and how sure the mapping is."""

    kind: Optional[str] = None
    params: List[OursParam] = field(default_factory=list)  # kind parameters
    common: List[OursParam] = field(default_factory=list)  # mix, inputGain, outputGain, enabled
    unmapped: List[str] = field(default_factory=list)  # QC parameters with no mapping
    skipped: List[Tuple[str, str]] = field(default_factory=list)  # (QC parameter, why)
    verified: bool = False
    entry_found: bool = False
    notes: List[str] = field(default_factory=list)  # statements for the report
    mapping_note: str = ""

    def kind_values(self) -> Dict[str, float]:
        return {p.ident: p.value for p in self.params}

    def common_values(self) -> Dict[str, float]:
        return {p.ident: p.value for p in self.common}


def derive_ours(take: "Take", mapping: Dict[str, Any], model_name: Optional[str] = None) -> OursPlan:
    """Our kind and parameters for a take: from the plan, else from the mapping table.

    `model_name` is the name the unit's catalogue gives the model (for a take
    that names its model by id); by default the name in the plan.
    """
    plan = OursPlan()
    if take.bypass:
        plan.verified = True
        plan.notes.append("A bypass take: the unit's own path, our render is an empty chain.")
        return plan
    name = model_name or (take.model if isinstance(take.model, str) else None)
    entry = mapping_entry(mapping, name)
    plan.entry_found = entry is not None
    entry_kind = entry.get("kind") if entry else None
    plan.mapping_note = (entry or {}).get("notes", "") or ""

    if take.ours_kind:
        plan.kind = take.ours_kind
        if entry_kind and entry_kind != take.ours_kind:
            plan.notes.append(f"The plan gives kind {take.ours_kind}; the mapping would have used {entry_kind}.")
    elif entry_kind:
        plan.kind = entry_kind
    else:
        if name is None:
            plan.notes.append("The model is given by id and the unit was not asked, so no mapping entry could be looked up.")
        elif entry is None:
            plan.notes.append(f"No mapping entry for {name!r} in the mapping table and no kind in the plan.")
        else:
            plan.notes.append(f"The mapping entry for {name!r} has no kind and the plan gives none.")
        plan.notes.append("Only the Quad Cortex side is recorded and analysed for this take.")
        return plan

    params_table = (entry or {}).get("params") or {}
    lookup = {norm(qc_name): (qc_name, spec) for qc_name, spec in params_table.items()}
    for qc_name, value in take.params.items():
        shown = value["wire"] if isinstance(value, dict) else value
        found = lookup.get(norm(qc_name))
        if found is None:
            plan.unmapped.append(qc_name)
            continue
        _, spec = found
        if isinstance(value, dict):
            plan.skipped.append((qc_name, "given as a wire value, which the mapping's units do not cover"))
            continue
        ok, result = apply_transform(spec.get("transform"), shown)
        if not ok:
            plan.skipped.append((qc_name, str(result)))
            continue
        ours = OursParam(
            ident=spec["param"],
            value=result,
            source="mapping",
            qc_name=qc_name,
            qc_value=shown,
            transform=describe_transform(spec.get("transform")),
        )
        (plan.common if ours.ident in COMMON_PARAMS else plan.params).append(ours)

    for ident, value in take.ours_params.items():
        target = plan.common if ident in COMMON_PARAMS else plan.params
        target[:] = [p for p in target if p.ident != ident]
        target.append(OursParam(ident=ident, value=float(value), source="plan"))

    verified_entry = bool(entry and entry.get("verified") is True)
    complete = not plan.unmapped and not plan.skipped
    plan.verified = verified_entry and complete and not take.ours_kind and not take.ours_params
    return plan


def mapping_statements(plan: OursPlan, model_label: str) -> List[str]:
    """What the report says about how trustworthy our side's settings are."""
    lines: List[str] = []
    if plan.kind is None:
        return list(plan.notes)
    if plan.verified:
        lines.append("Mapping verified: every setting was mapped by an entry marked verified.")
    elif not plan.entry_found:
        lines.append(f"Mapping UNVERIFIED: no entry for {model_label!r}; the kind and settings come from the plan.")
    else:
        lines.append(
            f"Mapping UNVERIFIED: the entry for {model_label!r} in resources/import/quad-cortex.json is "
            "marked verified false, or the plan overrides it, or a setting has no mapping."
        )
    for qc_name in plan.unmapped:
        lines.append(
            f"No mapping for the Quad Cortex parameter {qc_name!r}: it is set on the unit; our side has it only if the plan's ours.params give it."
        )
    for qc_name, why in plan.skipped:
        lines.append(f"Could not map {qc_name!r}: {why}.")
    lines.extend(plan.notes)
    if plan.mapping_note:
        lines.append(f"Mapping note: {plan.mapping_note}")
    return lines


# ####################
# THE PLAN (pure)
# ####################


@dataclass
class Take:
    name: str
    model: Any = None  # catalogue name or id; None for a bypass take
    params: Dict[str, Any] = field(default_factory=dict)  # name -> number | {"wire": x}
    bypass: bool = False
    ours_kind: Optional[str] = None
    ours_params: Dict[str, float] = field(default_factory=dict)
    notes: str = ""
    # dB below nominal the test signal is played and rendered at (0 or less).
    signal_gain_db: float = 0.0
    # A second block in column 2 after the first (a cab after an amp, say): catalogue name or id, and its
    # params. For listening takes; the render of ours never includes it.
    after_model: Any = None
    after_params: Dict[str, Any] = field(default_factory=dict)


MIN_SIGNAL_GAIN_DB = -60.0


def parse_signal_gain(value: Any, where: str, problems: List[str]) -> Optional[float]:
    """A `signalGainDb` of the plan: a number from -60 to 0, else a problem is recorded."""
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)) \
            or not MIN_SIGNAL_GAIN_DB <= float(value) <= 0.0:
        problems.append(f"{where}: signalGainDb must be a number from {MIN_SIGNAL_GAIN_DB:g} to 0 (dB below the nominal signal)")
        return None
    return float(value) + 0.0  # no negative zero


def parse_params(raw_params: Any, where: str, problems: List[str]) -> Dict[str, Any]:
    """A block's `params` of the plan: name -> number in the parameter's units, or {"wire": 0..1}."""
    params: Dict[str, Any] = {}
    if not isinstance(raw_params, dict):
        problems.append(f"{where}: params must be an object of name: value")
        return params
    for key, value in raw_params.items():
        if isinstance(value, bool):
            value = 1 if value else 0
        if isinstance(value, (int, float)) and math.isfinite(float(value)):
            params[key] = value
        elif isinstance(value, dict) and set(value) == {"wire"} and isinstance(value["wire"], (int, float)) \
                and 0.0 <= float(value["wire"]) <= 1.0:
            params[key] = {"wire": float(value["wire"])}
        else:
            problems.append(
                f"{where}: parameter {key!r} must be a number in the parameter's own units "
                '(a list parameter: its option index) or {"wire": 0..1}'
            )
    return params


def parse_plan(document: Any) -> List[Take]:
    """The takes of a plan: {"format": ..., "takes": [...]} or a bare list of takes."""
    problems: List[str] = []
    default_gain = 0.0
    if isinstance(document, dict):
        if document.get("format") not in (None, FORMAT):
            raise PlanError(f"the plan's format must be {FORMAT!r}")
        raw_takes = document.get("takes")
        default_gain = parse_signal_gain(document.get("signalGainDb"), "the plan", problems) or 0.0
    else:
        raw_takes = document
    if not isinstance(raw_takes, list) or not raw_takes:
        raise PlanError("the plan needs a non-empty list of takes")

    takes: List[Take] = []
    seen: Dict[str, int] = {}
    for number, raw in enumerate(raw_takes, 1):
        where = f"take {number}"
        if not isinstance(raw, dict):
            problems.append(f"{where}: must be an object")
            continue
        name = raw.get("name")
        if not isinstance(name, str) or not name.strip():
            problems.append(f"{where}: needs a name (it becomes the file name)")
            continue
        where = f"take {name!r}"
        if safe_stem(name) != name:
            problems.append(f"{where}: the name may use letters, digits, '.', '_' and '-' only (try {safe_stem(name)!r})")
        if norm(name) in seen:
            problems.append(f"{where}: the name is used twice")
        seen[norm(name)] = number

        bypass = bool(raw.get("bypass", False))
        model = raw.get("model")
        if bypass:
            if model is not None:
                problems.append(f"{where}: a bypass take has no model")
        elif isinstance(model, bool) or not isinstance(model, (str, int)) or model == "":
            problems.append(f"{where}: model must be a catalogue name or an id")

        params = parse_params(raw.get("params", {}), where, problems)
        if bypass and params:
            problems.append(f"{where}: a bypass take has no params")

        after_model = None
        after_params: Dict[str, Any] = {}
        after = raw.get("after")
        if after is not None:
            if bypass:
                problems.append(f"{where}: a bypass take has no second block")
            elif not isinstance(after, dict):
                problems.append(f"{where}: after must be an object with a model and its params")
            else:
                after_model = after.get("model")
                if isinstance(after_model, bool) or not isinstance(after_model, (str, int)) or after_model == "":
                    problems.append(f"{where}: after.model must be a catalogue name or an id")
                    after_model = None
                after_params = parse_params(after.get("params", {}), f"{where}, after", problems)

        ours_kind = None
        ours_params: Dict[str, float] = {}
        ours = raw.get("ours")
        if ours is not None:
            if not isinstance(ours, dict):
                problems.append(f"{where}: ours must be an object")
            else:
                ours_kind = ours.get("kind")
                if ours_kind is not None and (not isinstance(ours_kind, str) or not ours_kind):
                    problems.append(f"{where}: ours.kind must be a kind id such as delay.digital")
                    ours_kind = None
                merged = dict(ours.get("params") or {})
                for ident in COMMON_PARAMS:
                    if ident in ours:
                        merged[ident] = ours[ident]
                for ident, value in merged.items():
                    if isinstance(value, bool):
                        value = 1 if value else 0
                    if isinstance(value, (int, float)) and math.isfinite(float(value)):
                        ours_params[ident] = float(value)
                    else:
                        problems.append(f"{where}: ours setting {ident!r} must be a number")
        take_gain = parse_signal_gain(raw.get("signalGainDb"), where, problems)
        takes.append(
            Take(
                name=name,
                model=None if bypass else model,
                params=params,
                bypass=bypass,
                ours_kind=ours_kind,
                ours_params=ours_params,
                notes=str(raw.get("notes", "")),
                signal_gain_db=default_gain if take_gain is None else take_gain,
                after_model=after_model,
                after_params=after_params,
            )
        )
    if problems:
        raise PlanError("the plan has problems:\n  " + "\n  ".join(problems))
    return takes


# ####################
# OUR RENDER (pure)
# ####################


def build_preset(name: str, kind: Optional[str], params: Dict[str, float]) -> Dict[str, Any]:
    """A minimal JSON preset holding one block of `kind` (docs/matching.md, "Rendering ours")."""
    blocks: List[Dict[str, Any]] = []
    if kind:
        blocks.append(
            {
                "node": "ChainBlock",
                "id": "b1",
                "type": "kind",
                "enabled": True,
                # Fully wet, as the app adds a block (BlockKindInfo::defaultMix);
                # a plan that sets MIX changes it with a command after the load.
                "mix": 1.0,
                "children": [
                    {
                        "node": "KindState",
                        "kind": kind,
                        "version": 1,
                        "name": kind,
                        "children": [
                            {"node": "Param", "id": ident, "value": float(value)} for ident, value in sorted(params.items())
                        ],
                    }
                ],
            }
        )
    return {
        "format": "pedaleira.preset",
        "version": 1,
        "schemaVersion": 1,
        "name": name,
        "id": re.sub(r"[^a-z0-9]+", "", name.lower()) or "take",
        "chain": {
            "stereoEnabled": False,
            "branchSide": "left",
            "branchAfterBlockId": "",
            "children": [{"node": "ChainBlocks", "children": blocks}, {"node": "RightChainBlocks"}],
        },
        "params": {},
    }


def build_commands(common: Dict[str, float]) -> List[Dict[str, Any]]:
    """block.setParam commands for the settings every block has (mix, gains, enabled)."""
    return [
        {"command": "block.setParam", "args": {"blockId": "b1", "param": ident, "value": float(common[ident])}}
        for ident in COMMON_PARAMS
        if ident in common
    ]


# ####################
# THE TOOLS (command lines, pure)
# ####################


def default_tool(name: str) -> Path:
    exe = name + (".exe" if os.name == "nt" else "")
    return REPO / "build" / "tools" / f"{name}_artefacts" / "Release" / exe


def signals_cmd(match_tool: str, out_dir: str, rate: int) -> List[str]:
    return [match_tool, "signals", "--out", out_dir, "--rate", str(rate)]


def record_cmd(
    match_tool: str,
    device: str,
    out_ch: str,
    in_ch: str,
    name: str,
    out_dir: str,
    rate: int = DEFAULT_RATE,
    signal_type: Optional[str] = None,
    buffer: Optional[int] = None,
    gain_db: float = 0.0,
) -> List[str]:
    """PedaleiraMatch record: plays the test signal on out_ch and records in_ch into out_dir/name.wav.

    `gain_db` (0 or less) plays the signal that many dB below nominal (record --gain-db); at 0 the
    option is left out, so the command is the one older builds know."""
    cmd = [match_tool, "record", "--device", device, "--out-ch", out_ch, "--in-ch", in_ch, "--name", name, "--dir", out_dir]
    if signal_type:
        cmd += ["--type", signal_type]
    cmd += ["--rate", str(rate)]
    if buffer:
        cmd += ["--buffer", str(buffer)]
    if gain_db:
        cmd += ["--gain-db", f"{gain_db:g}"]
    return cmd


def render_cmd(
    render_tool: str,
    preset: str,
    signal: str,
    out_wav: str,
    tail: float = DEFAULT_TAIL_S,
    script: Optional[str] = None,
    out_channels: Optional[int] = None,
) -> List[str]:
    cmd = [render_tool, "--preset", preset]
    if script:
        cmd += ["--run", script]
    cmd += ["--in", signal, "--out", out_wav, "--tail", f"{tail:g}"]
    if out_channels:
        cmd += ["--out-channels", str(out_channels)]
    return cmd


# ####################
# THE ATTENUATED SIGNAL (pure, plus file I/O)
# ####################

SIGNAL_FILE_RE = re.compile(r"^pedaleira-test-signal-v\d+\.wav$")


def gain_factor(gain_db: float) -> float:
    return 10.0 ** (gain_db / 20.0)


def attenuated_name(signal_name: str, gain_db: float) -> str:
    """File name of the copy of a signal WAV played `gain_db` below nominal: ...-v1-atten-6dB.wav."""
    stem, dot, extension = signal_name.rpartition(".")
    return f"{stem}-atten-{abs(gain_db):g}dB{dot}{extension}"


def read_float_wav(path: Path) -> Tuple[int, int, "array.array"]:
    """(sample rate, channels, samples) of a 32-bit float WAV, the format `PedaleiraMatch signals` writes."""
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise PlanError(f"{path} is not a WAV file")
    rate = channels = 0
    samples: Optional[array.array] = None
    at = 12
    while at + 8 <= len(data):
        chunk_id, size = data[at : at + 4], struct.unpack("<I", data[at + 4 : at + 8])[0]
        body = data[at + 8 : at + 8 + size]
        if chunk_id == b"fmt " and len(body) >= 16:
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            if tag != 3 or bits != 32:
                raise PlanError(f"{path} is not a 32-bit float WAV (format {tag}, {bits} bits)")
        elif chunk_id == b"data":
            samples = array.array("f")
            samples.frombytes(body[: len(body) - len(body) % 4])
            if sys.byteorder == "big":
                samples.byteswap()
        at += 8 + size + (size & 1)
    if samples is None or not rate or not channels:
        raise PlanError(f"{path} has no fmt or data chunk")
    return rate, channels, samples


def write_float_wav(path: Path, rate: int, channels: int, samples: "array.array") -> None:
    body = array.array("f", samples)
    if sys.byteorder == "big":
        body.byteswap()
    raw = body.tobytes()
    fmt_chunk = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
    header = b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt_chunk) + 8 + len(raw)) + b"WAVE"
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header + b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk + b"data" + struct.pack("<I", len(raw)) + raw)


def attenuated_signal(signal: str, gain_db: float) -> str:
    """The signal WAV scaled by `gain_db`, written next to it once per session (a copy that is
    newer than the signal is kept). Returns `signal` itself at 0 dB. PedaleiraRender renders
    ours from this copy, and `record --gain-db` plays the same scaling, so both sides see the
    same levels."""
    if not gain_db:
        return signal
    target = Path(signal).with_name(attenuated_name(Path(signal).name, gain_db))
    if target.is_file() and target.stat().st_mtime >= Path(signal).stat().st_mtime:
        return str(target)
    rate, channels, samples = read_float_wav(Path(signal))
    factor = gain_factor(gain_db)
    write_float_wav(target, rate, channels, array.array("f", (value * factor for value in samples)))
    return str(target)


def analyse_cmd(match_tool: str, wav: str, out_json: str, rate: int = DEFAULT_RATE) -> List[str]:
    return [match_tool, "analyse", "--in", wav, "--out", out_json, "--rate", str(rate)]


def compare_cmd(
    match_tool: str, reference: str, candidate: str, out_json: str, markdown: str, rate: int = DEFAULT_RATE
) -> List[str]:
    return [
        match_tool, "compare", "--reference", reference, "--candidate", candidate,
        "--out", out_json, "--markdown", markdown, "--rate", str(rate),
    ]  # fmt: skip


class SessionDirs(NamedTuple):
    root: Path
    signal: Path
    ref: Path
    ours: Path
    analysis: Path
    presets: Path

    @staticmethod
    def at(root: Path) -> "SessionDirs":
        return SessionDirs(root, root / "signal", root / "ref", root / "ours", root / "analysis", root / "presets")


class TakeFiles(NamedTuple):
    ref_wav: str
    ours_wav: str
    preset: str
    script: str
    ref_analysis: str
    ours_analysis: str
    comparison: str
    markdown: str

    @staticmethod
    def for_take(dirs: SessionDirs, name: str) -> "TakeFiles":
        return TakeFiles(
            ref_wav=str(dirs.ref / f"{name}.wav"),
            ours_wav=str(dirs.ours / f"{name}.wav"),
            preset=str(dirs.presets / f"{name}.json"),
            script=str(dirs.presets / f"{name}.commands.json"),
            ref_analysis=str(dirs.analysis / f"ref-{name}.json"),
            ours_analysis=str(dirs.analysis / f"ours-{name}.json"),
            comparison=str(dirs.analysis / f"{name}.compare.json"),
            markdown=str(dirs.analysis / f"{name}.md"),
        )


class ToolFailed(RuntimeError):
    def __init__(self, stage: str, argv: Sequence[str], code: int, output: str):
        tail = "\n".join(output.strip().splitlines()[-6:])
        super().__init__(f"{stage} failed (exit {code}): {tail}".strip())
        self.stage = stage
        self.argv = list(argv)
        self.code = code


def run_tool(stage: str, argv: Sequence[str], timeout: float = 600.0) -> str:
    """Runs a tool, returns its stdout; raises ToolFailed on a non-zero exit or a missing binary."""
    try:
        done = subprocess.run(list(argv), capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        raise ToolFailed(stage, argv, 127, f"{argv[0]} was not found (build it, or give its path with an option)")
    except subprocess.TimeoutExpired:
        raise ToolFailed(stage, argv, 124, f"timed out after {timeout:g} s")
    if done.returncode != 0:
        raise ToolFailed(stage, argv, done.returncode, (done.stderr or "") + (done.stdout or ""))
    return done.stdout or ""


# ####################
# SCORES (pure)
# ####################


def summarise_comparison(comparison: Dict[str, Any]) -> Dict[str, Any]:
    """Overall score, score per group and the failed metrics of a `PedaleiraMatch compare` result."""
    groups: Dict[str, List[int]] = {}
    failed: List[Dict[str, Any]] = []
    for metric in comparison.get("metrics", []):
        if not metric.get("judged", True):
            continue
        counts = groups.setdefault(str(metric.get("group", "")), [0, 0])  # passed, judged
        counts[1] += 1
        if metric.get("pass", False):
            counts[0] += 1
        else:
            failed.append(metric)
    return {
        "score": comparison.get("score"),
        "judged": comparison.get("judged"),
        "passed": comparison.get("passed"),
        "groups": {
            name: {"passed": c[0], "judged": c[1], "score": round(100.0 * c[0] / c[1], 1) if c[1] else None}
            for name, c in groups.items()
        },
        "failed": failed,
    }


# ####################
# THE UNIT (needs pyquadcortex)
# ####################


class Toolkit(NamedTuple):
    """What a session needs from pyquadcortex. The real ones come from `pyquadcortex_toolkit()`."""

    connect: Callable[[], Any]
    free_rows: Callable[[Any], list]
    blocks: Callable[[Any], list]  # preset -> [object with row, column, model_id]
    inputs: Any  # the Input enum
    outputs: Any  # the Output enum
    setlists: Any  # object with USER and FACTORY
    device_errors: Tuple[type, ...]  # the device cannot be opened
    refused_errors: Tuple[type, ...]  # the block was refused (no DSP room)
    lost_errors: Tuple[type, ...]  # the link died
    # (preset, cell, parameter index) -> .scene_mode, .values: what a read preset stores for one
    # parameter (pyquadcortex.param_state). None: the parameter readback is not available.
    param_state: Optional[Callable[[Any, Any, int], Any]] = None


def pyquadcortex_toolkit() -> Toolkit:
    """The real helpers. pyquadcortex is imported here, not at module load."""
    try:
        import pyquadcortex
    except ImportError as error:  # pragma: no cover - needs the library
        raise SystemExit(
            "pyquadcortex is not installed. Run: pip install pyquadcortex (Python 3.11 or newer, "
            "plus the hidapi library; see tools/match/README.md)\n" + str(error)
        )
    return Toolkit(
        connect=pyquadcortex.connect,
        free_rows=pyquadcortex.free_rows,
        blocks=pyquadcortex.blocks,
        inputs=pyquadcortex.Input,
        outputs=pyquadcortex.Output,
        setlists=pyquadcortex.Setlist,
        device_errors=(pyquadcortex.DeviceNotFoundError,),
        refused_errors=(pyquadcortex.BlockRefused,),
        lost_errors=(pyquadcortex.DeviceLostError,),
        param_state=lambda preset, cell, index: pyquadcortex.param_state(preset, cell.row, cell.column, index),
    )


def port_value(enum: Any, name: str, kind: str) -> Any:
    names = [n for n in dir(enum) if n.isupper() and not n.startswith("_")]
    if name.upper() not in names:
        raise PlanError(f"unknown {kind} port {name!r}; the library knows {', '.join(names)}")
    return getattr(enum, name.upper())


def open_unit(kit: Toolkit) -> GuardedClient:
    """Connects and wraps the client. The raw client never leaves this function."""
    try:
        raw = kit.connect()
    except kit.device_errors as error:
        raise UnitRefused(
            "Could not open the Quad Cortex over USB. Is Cortex Control still running? It holds the "
            "USB interface exclusively: quit it, check the cable and that the unit has finished "
            f"booting, then run this again.\n({error})",
            EXIT_NO_DEVICE,
        )
    return GuardedClient(raw)


def read_dirty(qc: GuardedClient, sleep: Callable[[float], None]) -> bool:
    """Whether the grid holds unsaved edits. The first request after connecting is sometimes dropped."""
    for attempt in range(3):
        try:
            return bool(qc.preset_dirty())
        except TimeoutError:
            sleep(0.5)
    raise UnitRefused(
        "The unit did not say whether the preset on the grid has unsaved edits (three tries). "
        "Nothing was touched, because a session never works on an unknown state. Run this again.",
        EXIT_DIRTY,
    )


def ensure_clean(qc: GuardedClient, sleep: Callable[[float], None]) -> None:
    """Stops when the grid holds unsaved edits."""
    if read_dirty(qc, sleep):
        raise UnitRefused(DIRTY_MESSAGE, EXIT_DIRTY)


def scratch_setlist_of(kit: "Toolkit", opts: "Options") -> Any:
    return kit.setlists.FACTORY if opts.scratch_setlist == "factory" else kit.setlists.USER


def recall(qc: GuardedClient, setlist: Any, position: Any, why: str, log: List[str]) -> Any:
    """Recalls a stored preset on the unit (the audio cuts, nothing is written), and logs it on the console
    and in the report."""
    entry = f"recall {position!r} of the {getattr(setlist, 'name', setlist)} setlist: {why}"
    log.append(entry)
    print(f"  {entry}")
    return qc.read_preset(setlist, position)


def clear_scratch_edits(qc: GuardedClient, kit: "Toolkit", opts: "Options", sleep: Callable[[float], None], log: List[str]) -> bool:
    """The grid holds unsaved edits. When the preset loaded is the scratch preset they can only be left over
    from an earlier session, so they are cleared: another preset (the first factory preset that is not the
    scratch) is recalled and then the scratch again, because recalling the loaded preset does not reload it.
    Returns False when the loaded preset is not the scratch (nothing was done). Raises UnitRefused when the
    edits are still there afterwards. Nothing is ever saved."""
    if opts.use_current:
        return False
    try:
        current_name = str(getattr(qc.read_current_preset(), "name", "") or "")
    except (TimeoutError, OSError, ValueError):
        return False
    setlist = scratch_setlist_of(kit, opts)
    try:
        if opts.scratch_slot:
            position = opts.scratch_slot
            names = [e.name for e in qc.list_presets(setlist) if str(e.index).upper() == str(position).upper()
                     or str(getattr(e, "slot", "")).upper() == str(position).upper()]
            scratch_name = names[0] if names else ""
        else:
            scratch_name = opts.scratch_preset
            position = None
        if not scratch_name or norm(current_name) != norm(scratch_name):
            return False
        if position is None:
            position = qc.find_preset(scratch_name, setlist).index
    except (LookupError, ValueError, AttributeError, TimeoutError):
        return False

    print(f"The loaded preset {current_name!r} is the scratch preset and has unsaved edits: they are leftovers of "
          "an earlier session, so they are cleared by recalling another preset and then the scratch again.")
    others = [e for e in qc.list_presets(kit.setlists.FACTORY) if norm(e.name) != norm(scratch_name)
              and not (setlist == kit.setlists.FACTORY and str(e.index) == str(position))]
    if not others:
        raise UnitRefused("No other factory preset was found to recall first, so the scratch preset's edits could not be cleared.", EXIT_DIRTY)
    detour = min(others, key=lambda e: e.index)
    recall(qc, kit.setlists.FACTORY, detour.index, f"a harmless preset ({detour.name!r}), so the scratch preset is reloaded", log)
    sleep(0.5)
    stored = recall(qc, setlist, position, f"the scratch preset {scratch_name!r}, without its leftover edits", log)
    sleep(0.5)
    if read_dirty(qc, sleep):
        raise UnitRefused(DIRTY_MESSAGE + " (The scratch preset was recalled again through another preset and still reports unsaved edits.)", EXIT_DIRTY)
    live = qc.read_current_preset()
    for row in range(4):
        if chain_ports(live, row) != chain_ports(stored, row):
            raise UnitRefused(f"After clearing the scratch preset's edits, row {row + 1} routing differs from what the preset stores. "
                              "Nothing was saved; recall the preset on the unit and run this again.", EXIT_DIRTY)
    print("The unsaved-edit flag is gone and the routing is the one the scratch preset stores.")
    log.append("check: the unsaved-edit flag is gone and the routing matches what the scratch preset stores")
    return True


def _present(message: Any, name: str) -> bool:
    try:
        return bool(message.HasField(name))
    except (ValueError, AttributeError):
        return False


def chain_ports(preset: Any, row: int) -> Tuple[Optional[int], Optional[int]]:
    """(in_portid, out_portid) of a row in a read preset, or None for a field that is not there."""
    for index, chain in enumerate(preset.chains):
        if (int(chain.row) if _present(chain, "row") else index) == row:
            in_port = int(chain.in_portid) if _present(chain, "in_portid") else None
            out_port = int(chain.out_portid) if _present(chain, "out_portid") else None
            return in_port, out_port
    return None, None


def all_models(catalog: Any) -> list:
    if hasattr(catalog, "models"):
        return list(catalog.models.values())
    if isinstance(catalog, dict):
        return list(catalog.values())
    return list(catalog)


def resolve_model(catalog: Any, ref: Any) -> Any:
    """A catalogue model by id or by name (case, spaces and punctuation ignored).

    A name shared by an old superseded model and one current model means the current one;
    any other ambiguity is an error."""
    models = all_models(catalog)
    if isinstance(ref, int):
        for model in models:
            if int(model.id) == ref:
                return model
        raise PlanError(f"the unit's catalogue has no model with id {ref}")
    matches = [m for m in models if norm(m.name) == norm(ref)]
    if not matches:
        close = difflib.get_close_matches(str(ref), [m.name for m in models], n=5, cutoff=0.6)
        hint = f" Close names: {', '.join(close)}." if close else ""
        raise PlanError(f"the unit's catalogue has no model named {ref!r}.{hint} Use --catalog to list them.")
    if len(matches) > 1:
        current = [m for m in matches if not getattr(m, "superseded", False)]
        if len(current) == 1:
            return current[0]
        ids = ", ".join(f"{m.id} ({m.category})" for m in matches)
        raise PlanError(f"{ref!r} names several models: {ids}. Give the id in the plan.")
    return matches[0]


def find_parameter(model: Any, name: str) -> Any:
    for spec in model.parameters:
        if norm(spec.name) == norm(name):
            return spec
    names = ", ".join(spec.name for spec in model.parameters)
    raise PlanError(f"model {model.name!r} has no parameter {name!r}; it has: {names}")


@dataclass
class UnitSetting:
    """One parameter write: how it goes to the unit, and anything worth saying about it."""

    name: str  # the unit's own spelling
    shown: Any  # what the plan said
    unit: str  # the unit's units
    kwargs: Dict[str, Any]  # real=..., or value=... (a wire 0..1)
    note: str = ""
    wire: Optional[float] = None  # the 0..1 value that goes over the wire


def load_curves(path: Path = CURVES_PATH) -> Dict[str, Dict[str, float]]:
    """{model key: {parameter key: exponent}} from resources/import/quad-cortex-curves.json."""
    if not path.is_file():
        return {}
    document = load_json(path)
    curves: Dict[str, Dict[str, float]] = {}
    for model, params in (document.get("models") or {}).items():
        for name, spec in (params or {}).items():
            curves.setdefault(model_key(model), {})[norm(name)] = float(spec["exponent"])
    return curves


CURVES.update(load_curves())


def curve_exponent(curves: Dict[str, Dict[str, float]], model_name: str, param_name: str) -> Optional[float]:
    return curves.get(model_key(model_name), {}).get(norm(param_name))


def linear_wire(spec: Any, real: float) -> float:
    """The wire value pyquadcortex's `real=` sends: the line between the catalogue's minimum and maximum."""
    convert = getattr(spec, "to_normalized", None)
    if convert is not None:
        return float(convert(real))
    low, high = float(spec.minimum), float(spec.maximum)
    return min(1.0, max(0.0, (real - low) / (high - low))) if high > low else 0.0


def plan_setting(spec: Any, shown: Any, exponent: Optional[float] = None) -> UnitSetting:
    units = getattr(spec, "units", "") or ""
    if isinstance(shown, dict):
        return UnitSetting(spec.name, shown["wire"], "wire 0..1", {"value": float(shown["wire"])}, wire=float(shown["wire"]))
    count = getattr(spec, "option_count", None)
    if count:
        index = int(shown)
        if index != shown or not 0 <= index < count:
            raise PlanError(f"{spec.name!r} is a list of {count} options; give an index from 0 to {count - 1}")
        wire = float(spec.option_to_value(index))
        return UnitSetting(spec.name, index, f"option {index} of {count}", {"value": wire}, wire=wire)
    if getattr(spec, "range_is_placeholder", False):
        raise PlanError(
            f"{spec.name!r} publishes the placeholder range 0..1 ({units}); give it as "
            '{"wire": 0..1} (pyquadcortex cannot convert it)'
        )
    note = ""
    low, high = float(spec.minimum), float(spec.maximum)
    if not low <= float(shown) <= high:
        note = f"outside the unit's range {fmt(low)} to {fmt(high)} {units}: the library clamps it"
    if exponent and high > low:
        # The unit plays min + span * wire ^ exponent; pyquadcortex's own
        # conversion is linear (resources/import/quad-cortex-curves.json).
        fraction = min(1.0, max(0.0, (float(shown) - low) / (high - low)))
        wire = fraction ** (1.0 / exponent)
        curve = f"on the unit's curve (exponent {fmt(round(exponent, 4))}): wire {wire:.5f}"
        return UnitSetting(spec.name, shown, units, {"value": wire}, f"{note}; {curve}" if note else curve, wire=wire)
    return UnitSetting(spec.name, shown, units, {"real": float(shown)}, note, wire=linear_wire(spec, float(shown)))


@dataclass
class PreparedTake:
    take: Take
    model: Any = None
    settings: List[UnitSetting] = field(default_factory=list)
    ours: OursPlan = field(default_factory=OursPlan)
    unit_notes: List[str] = field(default_factory=list)
    error: str = ""
    after_model: Any = None  # the catalogue model of the second block, when the take has one
    after_settings: List[UnitSetting] = field(default_factory=list)

    def columns(self) -> int:
        """How many blocks this take puts on its row."""
        return 0 if self.take.bypass else (2 if self.take.after_model is not None else 1)


def prepare_take(take: Take, catalog: Any, mapping: Dict[str, Any]) -> PreparedTake:
    """Resolves a take against the unit's catalogue. A problem becomes `error`, not an exception."""
    prepared = PreparedTake(take=take)
    try:
        if take.bypass:
            prepared.ours = derive_ours(take, mapping)
            return prepared
        prepared.model = resolve_model(catalog, take.model)
        prepared.ours = derive_ours(take, mapping, prepared.model.name)
        for name, shown in take.params.items():
            exponent = curve_exponent(CURVES, prepared.model.name, name)
            prepared.settings.append(plan_setting(find_parameter(prepared.model, name), shown, exponent))
        entry = mapping_entry(mapping, prepared.model.name)
        table = {norm(k): v for k, v in ((entry or {}).get("params") or {}).items()}
        for setting in prepared.settings:
            spec_map = table.get(norm(setting.name))
            if spec_map and spec_map.get("unit") and setting.unit and spec_map["unit"].strip().lower() != setting.unit.strip().lower():
                prepared.unit_notes.append(
                    f"{setting.name}: the mapping was written for {spec_map['unit']!r}, the unit says {setting.unit!r}"
                )
            if setting.note:
                prepared.unit_notes.append(f"{setting.name}: {setting.note}")
        if take.after_model is not None:
            prepared.after_model = resolve_model(catalog, take.after_model)
            for name, shown in take.after_params.items():
                exponent = curve_exponent(CURVES, prepared.after_model.name, name)
                prepared.after_settings.append(plan_setting(find_parameter(prepared.after_model, name), shown, exponent))
    except PlanError as error:
        prepared.error = str(error)
    return prepared


def setup_take(
    qc: GuardedClient,
    kit: Toolkit,
    prepared: PreparedTake,
    row: int,
    in_port: Any,
    out_port: Any,
    occupied: int,
    sleep: Callable[[float], None],
) -> None:
    """The grid edits of one take, all on `row`: its block in column 0 and, when the take has one, its
    second block in column 1. `occupied` is how many blocks the previous take left there. Nothing here
    stores anything."""
    for column in reversed(range(occupied)):
        qc.remove_block(row=row, column=column)
        sleep(0.4)
    if not prepared.take.bypass:
        qc.set_block(row=row, column=0, model=int(prepared.model.id))
        if prepared.after_model is not None:
            qc.set_block(row=row, column=1, model=int(prepared.after_model.id))
    qc.set_chain_input(row=row, in_portid=in_port)
    qc.set_chain_output(row=row, out_portid=out_port)
    if not prepared.take.bypass:
        qc.set_bypass(row=row, column=0, bypassed=False)
        for setting in prepared.settings:
            qc.set_param(row=row, column=0, param=setting.name, model=prepared.model, **setting.kwargs)
        if prepared.after_model is not None:
            qc.set_bypass(row=row, column=1, bypassed=False)
            for setting in prepared.after_settings:
                qc.set_param(row=row, column=1, param=setting.name, model=prepared.after_model, **setting.kwargs)


def confirm_setup(qc: GuardedClient, kit: Toolkit, prepared: PreparedTake, row: int, in_port: Any, out_port: Any) -> Any:
    """Reads the live grid back (no side effects) and checks the block and the routing landed.
    Returns the live preset it read, which the parameter readback uses (one read serves both)."""
    live = qc.read_current_preset()
    found = [(b.row, b.column, b.model_id) for b in kit.blocks(live) if b.row == row]
    if prepared.take.bypass:
        if found:
            raise UnitRefused(f"row {row + 1} still holds {len(found)} block(s) after the setup")
    elif (row, 0, int(prepared.model.id)) not in found:
        raise UnitRefused(f"{prepared.model.name!r} is not at row {row + 1}, column 1 after the setup")
    if prepared.after_model is not None and (row, 1, int(prepared.after_model.id)) not in found:
        raise UnitRefused(f"{prepared.after_model.name!r} is not at row {row + 1}, column 2 after the setup")
    if len(found) > prepared.columns():
        raise UnitRefused(f"row {row + 1} holds more blocks than this take put there")
    read_in, read_out = chain_ports(live, row)
    if read_in is not None and read_in != int(in_port):
        raise UnitRefused(f"row {row + 1} input reads back as {read_in}, not {int(in_port)}")
    if read_out is not None and read_out != int(out_port):
        raise UnitRefused(f"row {row + 1} output reads back as {read_out}, not {int(out_port)}")
    return live


# ####################
# PARAMETER READBACK (read only)
# ####################


@dataclass
class ReadbackRow:
    """One parameter of a take: what was asked, what went over the wire, what the unit holds."""

    name: str
    asked: str  # the plan's value, in the plan's units
    sent: Optional[float]  # wire value sent (0..1)
    read: Any = None  # wire value the unit reports (a float, a string, or None when it is not there)
    linear: str = ""  # the read value in the parameter's units, pyquadcortex's linear conversion
    curve: str = ""  # the same on the unit's measured curve, when the curves table has one
    difference: str = ""  # converted value minus the value asked (numbers only)
    note: str = ""


def _read_value(state: Any) -> Any:
    """The value a parameter holds: the first slot (a parameter that follows scenes keeps one per scene; a
    fresh block does not, and this session never switches scene)."""
    values = list(state.values)
    return values[0] if values else None


def readback_row(setting: UnitSetting, spec: Any, stored: Any, exponent: Optional[float]) -> ReadbackRow:
    """The table row for one parameter from the value the unit holds. Pure."""
    units = getattr(spec, "units", "") or ""
    row = ReadbackRow(name=setting.name, asked=fmt(setting.shown) + (f" {units}" if units and not isinstance(setting.shown, dict) else ""),
                      sent=setting.wire)
    if stored is None:
        row.note = "the unit holds no value for it"
        return row
    if isinstance(stored, str):
        row.read = stored
        row.linear = stored
        return row
    if not isinstance(stored, (int, float)) or not math.isfinite(float(stored)):
        row.read = stored
        row.note = "the unit holds a value that cannot be converted"
        return row
    wire = float(stored)
    row.read = wire
    if setting.wire is not None and abs(wire - setting.wire) > 1.0e-4:
        row.note = f"the unit holds {wire:.5f}, not the {setting.wire:.5f} that was sent"
    count = getattr(spec, "option_count", None)
    if count:
        index = int(spec.value_to_option(wire))
        options = tuple(getattr(spec, "options", ()) or ())
        row.linear = str(options[index]) if 0 <= index < len(options) else f"option {index} of {count}"
        if isinstance(setting.shown, (int, float)) and not isinstance(setting.shown, bool):
            row.difference = fmt(index - setting.shown)
        return row
    if isinstance(setting.shown, dict) or getattr(spec, "range_is_placeholder", False):
        row.linear = "(no conversion: the range is the wire's own 0..1)"
        return row
    low, high = float(spec.minimum), float(spec.maximum)
    clamped = min(1.0, max(0.0, wire))
    linear = low + clamped * (high - low)
    row.linear = f"{fmt(clean_number(linear))} {units}".strip()
    best = linear
    if exponent and high > low:
        best = low + (high - low) * clamped**exponent
        row.curve = f"{fmt(clean_number(best))} {units}".strip()
    if isinstance(setting.shown, (int, float)) and not isinstance(setting.shown, bool):
        row.difference = fmt(round(best - float(setting.shown), 4) + 0.0)
    return row


def read_back(kit: Toolkit, live: Any, prepared: PreparedTake, row: int) -> Tuple[List[ReadbackRow], str]:
    """(rows, note) for the parameters this take set, from the live grid already read. Never raises:
    a parameter that cannot be read is a row with a note. Touches only the preset in memory."""
    if kit.param_state is None:
        return [], "the library helper for reading a parameter (param_state) is not available"
    cell = next((b for b in kit.blocks(live) if b.row == row and b.column == 0), None)
    if cell is None:
        return [], "the block was not found on the grid, so nothing could be read back"
    rows: List[ReadbackRow] = []
    for setting in prepared.settings:
        spec = find_parameter(prepared.model, setting.name)
        exponent = curve_exponent(CURVES, prepared.model.name, setting.name)
        try:
            stored = _read_value(kit.param_state(live, cell, int(spec.index)))
        except (IndexError, ValueError, KeyError, AttributeError, TypeError) as error:
            rows.append(ReadbackRow(setting.name, fmt(setting.shown), setting.wire, note=f"could not be read ({type(error).__name__})"))
            continue
        rows.append(readback_row(setting, spec, stored, exponent))
    return rows, ""


# ####################
# THE SESSION
# ####################


@dataclass
class Options:
    """Everything a session needs, from the command line."""

    plan: Optional[Path] = None
    go: bool = False
    out_dir: Path = Path("match")
    mapping: Path = DEFAULT_MAPPING
    scratch_preset: str = DEFAULT_SCRATCH_PRESET
    scratch_slot: Optional[str] = None
    scratch_setlist: str = "factory"
    use_current: bool = False
    row: Optional[int] = None  # as the unit numbers it, 1 to 4
    qc_input: str = DEFAULT_QC_INPUT
    qc_output: str = DEFAULT_QC_OUTPUT
    device: str = DEFAULT_DEVICE
    out_ch: str = DEFAULT_OUT_CH
    in_ch: str = DEFAULT_IN_CH
    signal_type: Optional[str] = None
    rate: int = DEFAULT_RATE
    buffer: Optional[int] = None
    settle: float = DEFAULT_SETTLE_S
    tail: float = DEFAULT_TAIL_S
    out_channels: Optional[int] = None
    match_tool: str = str(default_tool("PedaleiraMatch"))
    render_tool: str = str(default_tool("PedaleiraRender"))
    yes: bool = False
    overwrite: bool = False
    readback: bool = True  # read the block's parameters back from the live grid (--no-readback skips it)


@dataclass
class TakeResult:
    prepared: PreparedTake
    files: TakeFiles
    status: str = "pending"  # ok, recorded (QC only), failed
    stage: str = ""
    message: str = ""
    summary: Optional[Dict[str, Any]] = None
    attempts: int = 0
    record_check: List[str] = field(default_factory=list)  # the recorder's peak / PROBLEM / check lines
    readback: Optional[List["ReadbackRow"]] = None  # None: not read back
    readback_note: str = ""


@dataclass
class SessionInfo:
    started: str = ""
    firmware: str = ""
    scratch: str = ""
    row: int = 0
    aborted: str = ""
    recalls: List[str] = field(default_factory=list)  # every recall the session made, in order


def scratch_description(opts: Options) -> str:
    if opts.use_current:
        return "the preset on the grid now (--use-current)"
    where = "the factory library" if opts.scratch_setlist == "factory" else "My Presets"
    if opts.scratch_slot:
        return f"slot {opts.scratch_slot} of {where}"
    return f"{opts.scratch_preset!r} from {where}"


def shown(argv: Sequence[str]) -> str:
    """A command line for reading: the tool by its file name, arguments quoted where needed."""
    return shlex.join([os.path.basename(argv[0])] + list(argv[1:]))


def plan_lines(opts: Options, prepared: List[PreparedTake], dirs: SessionDirs, catalog_known: bool) -> List[str]:
    """The full plan, as text: grid edits, recordings, renders. Used by the dry run and before a real session."""
    row_text = f"row {opts.row}" if opts.row else "the first free row"
    lines = [
        f"Scratch preset: {scratch_description(opts)}, worked on {row_text}.",
        f"Routing on that row: input {opts.qc_input}, one block in column 1, output {opts.qc_output}.",
        f"Recording: device {opts.device!r}, play on channels {opts.out_ch}, record channels {opts.in_ch}, {opts.rate} Hz.",
        f"Output folder: {dirs.root}",
        "",
    ]
    signal = str(dirs.signal / DEFAULT_SIGNAL_NAME)
    lines.append("Before the takes: " + shown(signals_cmd(opts.match_tool, str(dirs.signal), opts.rate)))
    gains = sorted({it.take.signal_gain_db for it in prepared if it.take.signal_gain_db and not it.error})
    for gain in gains:
        lines.append(f"Signal copy {fmt(-gain)} dB below nominal: {Path(signal).with_name(attenuated_name(DEFAULT_SIGNAL_NAME, gain)).name} "
                     "(written next to the signal; ours is rendered from it)")
    for number, item in enumerate(prepared, 1):
        take = item.take
        files = TakeFiles.for_take(dirs, take.name)
        lines.append(f"\nTake {number}: {take.name}")
        if item.error:
            lines.append(f"  PROBLEM, this take will be skipped: {item.error}")
            continue
        if take.bypass:
            lines.append("  grid: remove any block on the row; set the row input and output (a bypass take)")
        else:
            label = item.model.name if item.model is not None else str(take.model)
            lines.append("  grid edits (unsaved, on the grid only):")
            lines.append(f"    set_block   row {opts.row or 'free'} column 1  {label}")
            if item.after_model is not None:
                lines.append(f"    set_block   row {opts.row or 'free'} column 2  {item.after_model.name}")
                for setting in item.after_settings:
                    unit = f" {setting.unit}" if setting.unit else ""
                    lines.append(f"    set_param   (column 2) {setting.name} = {fmt(setting.shown)}{unit}")
            lines.append(f"    set_chain_input {opts.qc_input}   set_chain_output {opts.qc_output}   set_bypass off")
            if item.settings:
                for setting in item.settings:
                    unit = f" {setting.unit}" if setting.unit else ""
                    lines.append(f"    set_param   {setting.name} = {fmt(setting.shown)}{unit}")
            else:
                for name, value in take.params.items():
                    lines.append(f"    set_param   {name} = {fmt(value)}  (units checked against the unit's catalogue when it runs)")
        if take.signal_gain_db:
            lines.append(f"  test signal played and rendered {fmt(-take.signal_gain_db)} dB below nominal (signalGainDb {fmt(take.signal_gain_db)})")
        if opts.readback and not take.bypass:
            lines.append("  read back the block's parameters from the live grid (read only), for the report")
        lines.append(f"  wait {opts.settle:g} s, then record:")
        lines.append("    " + shown(record_cmd(opts.match_tool, opts.device, opts.out_ch, opts.in_ch, take.name,
                                               str(dirs.ref), opts.rate, opts.signal_type, opts.buffer, take.signal_gain_db)))
        ours = item.ours
        if ours.kind is None and not take.bypass:
            lines.append("  ours: no kind, so only the recording is made and analysed")
        else:
            lines.append(f"  ours: {ours.kind or 'an empty chain (bypass)'}")
            for p in ours.params + ours.common:
                origin = f"from {p.qc_name} ({p.transform})" if p.source == "mapping" else "from the plan"
                lines.append(f"    {p.ident} = {fmt(p.value)}   {origin}")
            lines.append("    " + shown(render_cmd(opts.render_tool, files.preset,
                                                  str(Path(signal).with_name(attenuated_name(DEFAULT_SIGNAL_NAME, take.signal_gain_db)))
                                                  if take.signal_gain_db else signal, files.ours_wav, opts.tail,
                                                  files.script if ours.common else None, opts.out_channels)))
        for line in mapping_statements(ours, str(take.model)):
            lines.append(f"  note: {line}")
        for line in item.unit_notes:
            lines.append(f"  note: {line}")
        if ours.kind is not None or take.bypass:
            lines.append(f"  then: analyse both recordings and compare them (analyse, analyse, compare), report in {files.markdown}")
        else:
            lines.append("  then: analyse the recording")
    if not catalog_known:
        lines.append("\n(Dry run: the unit was not contacted, so model and parameter names are not yet checked against its catalogue.)")
    return lines


def prepare_offline(takes: List[Take], mapping: Dict[str, Any]) -> List[PreparedTake]:
    """A take without the unit: our side from the plan and the mapping table, the unit's side as the plan says it."""
    out: List[PreparedTake] = []
    for take in takes:
        item = PreparedTake(take=take)
        item.ours = derive_ours(take, mapping)
        out.append(item)
    return out


def check_files(opts: Options) -> None:
    for label, path in (("PedaleiraMatch", opts.match_tool), ("PedaleiraRender", opts.render_tool)):
        if shutil.which(path) is None and not os.path.isfile(path):
            raise PlanError(f"{label} was not found at {path}. Build it (cmake --build build --target {label}) or pass its path.")


def run_pipeline(opts: Options, item: PreparedTake, files: TakeFiles, dirs: SessionDirs, signal: str, result: TakeResult) -> None:
    """Record (one retry), render ours, analyse both, compare. Fills `result`."""
    name = item.take.name
    if os.path.exists(files.ref_wav) and not opts.overwrite:
        raise ToolFailed("record", [], 1, f"{files.ref_wav} already exists (a recording is a measurement; pass --overwrite to replace it)")
    for attempt in (1, 2):
        result.attempts = attempt
        try:
            output = run_tool("record", record_cmd(opts.match_tool, opts.device, opts.out_ch, opts.in_ch, name, str(dirs.ref),
                                                   opts.rate, opts.signal_type, opts.buffer, item.take.signal_gain_db), timeout=900)
            result.record_check = record_check_lines(output)
            break
        except ToolFailed as failure:
            print(f"  record attempt {attempt} failed: {failure}")
            if attempt == 2 or failure.code in (124, 127):
                raise
    if not os.path.isfile(files.ref_wav):
        raise ToolFailed("record", [], 1, f"the recorder exited cleanly but {files.ref_wav} is not there")
    run_tool("analyse reference", analyse_cmd(opts.match_tool, files.ref_wav, files.ref_analysis, opts.rate))

    ours = item.ours
    if ours.kind is None and not item.take.bypass:
        result.status = "recorded"
        return
    try:
        signal = attenuated_signal(signal, item.take.signal_gain_db)
    except (PlanError, OSError) as error:
        raise ToolFailed("signal gain", [], 1, f"cannot make the {fmt(item.take.signal_gain_db)} dB copy of the test signal: {error}")
    write_json(Path(files.preset), build_preset(name, ours.kind, ours.kind_values()))
    script = None
    if ours.common:
        script = files.script
        write_json(Path(files.script), build_commands(ours.common_values()))
    run_tool("render ours", render_cmd(opts.render_tool, files.preset, signal, files.ours_wav, opts.tail, script, opts.out_channels))
    run_tool("analyse ours", analyse_cmd(opts.match_tool, files.ours_wav, files.ours_analysis, opts.rate))
    run_tool("compare", compare_cmd(opts.match_tool, files.ref_analysis, files.ours_analysis, files.comparison,
                                    files.markdown, opts.rate))
    with open(files.comparison, "r", encoding="utf-8") as handle:
        result.summary = summarise_comparison(json.load(handle))
    result.status = "ok"


def record_check_lines(output: str) -> List[str]:
    """The lines of `PedaleiraMatch record` that report the take's check: peak, problems, verdict."""
    return [line.strip() for line in output.splitlines()
            if line.strip().startswith(("peak ", "PROBLEM", "check ", "test signal played"))]


def write_json(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def find_signal(dirs: SessionDirs) -> str:
    found = sorted(p for p in dirs.signal.glob("pedaleira-test-signal-v*.wav") if SIGNAL_FILE_RE.match(p.name))
    if not found:
        raise ToolFailed("signals", [], 1, f"no pedaleira-test-signal-v*.wav in {dirs.signal}")
    return str(found[-1])


@dataclass
class Deps:
    """The outside world of a session, replaceable in tests."""

    kit: Optional[Toolkit] = None
    sleep: Callable[[float], None] = time.sleep
    ask: Callable[[str], str] = input
    now: Callable[[], datetime.datetime] = datetime.datetime.now


def run_session(opts: Options, takes: List[Take], mapping: Dict[str, Any], deps: Deps) -> int:
    """The real session. Returns an exit code; always says at the end that nothing was saved."""
    dirs = SessionDirs.at(opts.out_dir)
    info = SessionInfo(started=deps.now().isoformat(timespec="seconds"))
    kit = deps.kit or pyquadcortex_toolkit()
    results: List[TakeResult] = []
    code = EXIT_OK
    qc: Optional[GuardedClient] = None
    # The scratch preset this session recalled, recalled afresh at the end so its
    # edits do not stay on the grid (the next session would refuse to start).
    recalled: Optional[Tuple[Any, Any]] = None
    try:
        check_files(opts)
        in_port = port_value(kit.inputs, opts.qc_input, "input")
        out_port = port_value(kit.outputs, opts.qc_output, "output")
        qc = open_unit(kit)
        try:
            info.firmware = str(qc.version().app_fw_version)
        except Exception:  # the version is information only
            info.firmware = ""
        print(f"Connected to the Quad Cortex{' (firmware ' + info.firmware + ')' if info.firmware else ''}.")

        if read_dirty(qc, deps.sleep):
            if not clear_scratch_edits(qc, kit, opts, deps.sleep, info.recalls):
                raise UnitRefused(DIRTY_MESSAGE, EXIT_DIRTY)
        catalog = qc.catalog
        prepared = [prepare_take(take, catalog, mapping) for take in takes]

        info.scratch = scratch_description(opts)
        print(f"\nThe unit has no unsaved edits. Scratch preset: {info.scratch}.")
        print("It will be recalled on the unit now (the audio cuts), and every edit stays unsaved.")
        print("\n".join(plan_lines(opts, prepared, dirs, True)))
        if not opts.yes:
            deps.ask("\nPress Enter to go ahead, Ctrl-C to stop with nothing touched. ")

        if opts.use_current:
            preset = qc.read_current_preset()
        else:
            setlist = scratch_setlist_of(kit, opts)
            position = opts.scratch_slot or qc.find_preset(opts.scratch_preset, setlist).index
            preset = recall(qc, setlist, position, "the scratch preset, to work on one free row of it", info.recalls)
            recalled = (setlist, position)
        info.scratch = f"{getattr(preset, 'name', '') or info.scratch} ({scratch_description(opts)})"

        free = list(kit.free_rows(preset))
        if opts.row:
            if opts.row - 1 not in free:
                raise UnitRefused(f"row {opts.row} is not free in this preset (free rows: {[r + 1 for r in free]})")
            row = opts.row - 1
        elif free:
            row = free[0]
        else:
            raise UnitRefused(
                "This preset has no free row (a row with no blocks that is not a branch lane). Pick another "
                "with --scratch-preset or --scratch-slot, or use an emptier preset with --use-current."
            )
        info.row = row + 1
        print(f"Working on row {row + 1} (free rows: {[r + 1 for r in free]}).")

        dirs.root.mkdir(parents=True, exist_ok=True)
        for folder in dirs:
            folder.mkdir(parents=True, exist_ok=True)
        run_tool("signals", signals_cmd(opts.match_tool, str(dirs.signal), opts.rate))
        signal = find_signal(dirs)

        occupied = 0
        for number, item in enumerate(prepared, 1):
            files = TakeFiles.for_take(dirs, item.take.name)
            result = TakeResult(prepared=item, files=files)
            results.append(result)
            print(f"\n[{number}/{len(prepared)}] {item.take.name}")
            if item.error:
                result.status, result.stage, result.message = "failed", "plan", item.error
                print(f"  skipped: {item.error}")
                continue
            try:
                setup_take(qc, kit, item, row, in_port, out_port, occupied, deps.sleep)
                occupied = item.columns()
                live = confirm_setup(qc, kit, item, row, in_port, out_port)
                if opts.readback and not item.take.bypass and item.settings:
                    result.readback, result.readback_note = read_back(kit, live, item, row)
                deps.sleep(opts.settle)
                run_pipeline(opts, item, files, dirs, signal, result)
            except ToolFailed as failure:
                result.status, result.stage, result.message = "failed", failure.stage, str(failure)
                if failure.stage == "record" and "clips" in str(failure) and not item.take.signal_gain_db:
                    result.message += (" (hint: play the test signal lower with signalGainDb in the plan, "
                                       "for instance -6; ours is then rendered at the same level)")
            except kit.refused_errors as refused:
                occupied = 0
                result.status, result.stage, result.message = "failed", "unit", f"the unit refused the block: {refused}"
            except kit.lost_errors:
                raise
            except UnitRefused as refused:
                result.status, result.stage, result.message = "failed", "unit", str(refused)
            except (PlanError, TimeoutError, OSError, ValueError, KeyError) as error:
                result.status, result.stage, result.message = "failed", "unit", f"{type(error).__name__}: {error}"
            score = result.summary["score"] if result.summary else None
            print(f"  {result.status}" + (f", score {score}" if score is not None else "") + (f": {result.message}" if result.message else ""))
        if any(r.status == "failed" for r in results):
            code = EXIT_TAKES_FAILED
    except UnitRefused as refused:
        info.aborted = str(refused)
        print(str(refused), file=sys.stderr)
        code = refused.code
    except PlanError as error:
        info.aborted = str(error)
        print(str(error), file=sys.stderr)
        code = EXIT_USAGE
    except KeyboardInterrupt:
        info.aborted = "stopped by the user"
        print("\nStopped.", file=sys.stderr)
        code = EXIT_TAKES_FAILED
    except Exception as error:
        if kit.lost_errors and isinstance(error, kit.lost_errors):
            info.aborted = f"the link to the unit died: {error}"
            print(info.aborted, file=sys.stderr)
            code = EXIT_TAKES_FAILED
        else:
            raise
    finally:
        if qc is not None and recalled is not None:
            # Only the session's own edits are discarded: the unit had none
            # when it started, and the scratch preset was recalled by it.
            # Recalling the preset that is already loaded does not reload it on
            # the unit (seen on CorOS d14e: the edits stayed), so another preset
            # of the same setlist is recalled first, then the scratch one.
            setlist, position = recalled
            other = 0 if position != 0 else 1
            try:
                recall(qc, setlist, other, "another preset of the setlist, so the scratch preset reloads", info.recalls)
                recall(qc, setlist, position, "the scratch preset again, which drops the session's edits", info.recalls)
                print("\nThe scratch preset was recalled again: the session's edits are gone from the grid.")
            except Exception as error:
                print(f"\nCould not recall the scratch preset again ({error}); recall any preset on the unit to clear the session's edits.",
                      file=sys.stderr)
        if qc is not None:
            try:
                qc.close()
            except Exception:
                pass
        if results or info.aborted:
            write_report(dirs, opts, info, results)
        print("\n" + NOTHING_SAVED)
    return code


# ####################
# THE REPORT (pure)
# ####################


def cell(text: Any) -> str:
    return str(text).replace("|", "/").replace("\n", " ")


def table(header: Sequence[str], rows: Sequence[Sequence[Any]]) -> List[str]:
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    lines += ["| " + " | ".join(cell(c) for c in row) + " |" for row in rows]
    return lines


def signal_gain_statement(gain_db: float) -> List[str]:
    """What the report says about a take whose test signal was played below nominal."""
    down = fmt(-gain_db)
    return [
        f"Signal level: the test signal was played {down} dB below nominal (signalGainDb {fmt(gain_db)}), and ours was rendered "
        "from a copy of the signal scaled by the same amount, so the comparison is like for like. The recorder's clip check "
        "looks at the recording itself and is unchanged. The analysis measures against the nominal signal: on both sides the "
        f"sweep level gain reads {down} dB lower than the block's true gain, and the input level of each static-curve step is "
        f"its nominal level minus {down} dB (the output levels are the true ones).",
        "",
    ]


def readback_lines(rows: List[ReadbackRow], note: str) -> List[str]:
    """The per-take table of what the unit holds for each parameter that was set."""
    lines = ["Parameters read back from the unit after the edits (read only, from the live grid):", ""]
    if not rows:
        return lines + [f"Not available: {note or 'no parameters were set'}.", ""]
    body = [[x.name, x.asked, "" if x.sent is None else f"{x.sent:.5f}", "" if x.read is None else (f"{x.read:.5f}" if isinstance(x.read, float) else x.read),
             x.linear, x.curve or "-", x.difference, x.note] for x in rows]
    lines += table(["Parameter", "Asked", "Wire sent", "Wire read back", "Converted (linear)", "Converted (unit's curve)", "Converted minus asked", "Note"], body)
    lines += ["", "\"Converted (linear)\" is the library's conversion between the catalogue minimum and maximum; \"unit's curve\" "
              "applies the exponent of resources/import/quad-cortex-curves.json where one is measured. The difference uses the "
              "curve when there is one.", ""]
    return lines


def render_report(opts: Options, info: SessionInfo, results: List[TakeResult]) -> str:
    ok = [r for r in results if r.status == "ok"]
    failed = [r for r in results if r.status == "failed"]
    lines = ["# Quad Cortex matching session", ""]
    lines.append(f"Started {info.started or 'unknown'}. Unit firmware: {info.firmware or 'unknown'}.")
    lines.append(f"Scratch preset: {info.scratch or 'none'}, row {info.row or 'none'}, "
                 f"input {opts.qc_input}, output {opts.qc_output}.")
    lines.append(f"Recording: device {opts.device!r}, play channels {opts.out_ch}, record channels {opts.in_ch}, {opts.rate} Hz.")
    lines.append("")
    lines.append(f"**{NOTHING_SAVED}**")
    if info.aborted:
        lines += ["", f"**The session stopped early:** {info.aborted}"]
    if info.recalls:
        lines += ["", "Recalls made on the unit (each cuts the audio for a moment; nothing was written):", ""]
        lines += [f"{number}. {text}" for number, text in enumerate(info.recalls, 1)]
    lines += ["", f"{len(results)} take(s): {len(ok)} compared, "
              f"{len([r for r in results if r.status == 'recorded'])} recorded only, {len(failed)} failed.", ""]

    if results:
        rows = []
        for r in results:
            p = r.prepared
            score = f"{r.summary['score']:.1f}" if r.summary and r.summary.get("score") is not None else ""
            model = p.model.name if p.model is not None else (str(p.take.model) if not p.take.bypass else "bypass")
            mapping = "n/a" if p.ours.kind is None else ("verified" if p.ours.verified else "UNVERIFIED")
            rows.append([r.prepared.take.name, model, p.ours.kind or "-", mapping, score, r.status])
        lines += ["## Summary", ""] + table(["Take", "QC model", "Our kind", "Mapping", "Score", "Result"], rows) + [""]

    lines.append("## Takes")
    for r in results:
        p = r.prepared
        lines += ["", f"### {p.take.name}", ""]
        if p.take.notes:
            lines += [p.take.notes, ""]
        if p.take.bypass:
            lines += ["Bypass take: the unit's own path with no block on the row.", ""]
        else:
            label = p.model.name if p.model is not None else str(p.take.model)
            lines += [f"Quad Cortex model: {label}" + (f" (id {p.model.id})" if p.model is not None else ""), ""]
            rows = [[s.name, fmt(s.shown), s.unit, s.note] for s in p.settings] or [[n, fmt(v), "", ""] for n, v in p.take.params.items()]
            lines += table(["QC parameter", "Value", "Unit", "Note"], rows) if rows else ["No parameters set (the unit's defaults)."]
            lines.append("")
        ours = p.ours
        if ours.kind is not None or p.take.bypass:
            lines.append(f"Our side: {ours.kind or 'an empty chain'}")
            rows = [[o.ident, fmt(o.value), "mapping: " + f"{o.qc_name} {fmt(o.qc_value)} ({o.transform})" if o.source == "mapping" else "plan"]
                    for o in ours.params + ours.common]
            if rows:
                lines += [""] + table(["Our setting", "Value", "From"], rows)
            lines.append("")
        label = str(p.model.name) if p.model is not None else str(p.take.model)
        lines += [f"- {line}" for line in mapping_statements(ours, label)]
        lines += [f"- {line}" for line in p.unit_notes]
        lines.append("")
        if p.take.signal_gain_db:
            lines += signal_gain_statement(p.take.signal_gain_db)
        if r.readback is not None:
            lines += readback_lines(r.readback, r.readback_note)
        if r.record_check:
            lines += ["Recording check (PedaleiraMatch record): " + "; ".join(r.record_check), ""]
        if r.status == "failed":
            lines += [f"**Failed at {r.stage}:** {r.message}", ""]
        elif r.status == "recorded":
            lines += ["Recorded and analysed; no kind on our side, so nothing was compared.", ""]
        elif r.summary:
            s = r.summary
            lines.append(f"**Overall score {s['score']}** ({s['passed']} of {s['judged']} judged metrics within tolerance).")
            lines.append("")
            gr = [[g, f"{c['passed']} of {c['judged']}", "" if c["score"] is None else c["score"]] for g, c in s["groups"].items()]
            lines += table(["Group", "Passed", "Score"], gr) + [""]
            if s["failed"]:
                fr = [[m.get("group"), m.get("name"), m.get("reference"), m.get("candidate"), m.get("difference"), m.get("allowed"),
                       m.get("unit", ""), m.get("note", "")] for m in s["failed"]]
                lines += ["Metrics out of tolerance:", ""] + table(
                    ["Group", "Metric", "Reference", "Ours", "Difference", "Allowed", "Unit", "Note"], fr) + [""]
            lines.append(f"Details: {r.files.markdown}")
        lines.append(f"Files: {r.files.ref_wav}, {r.files.ours_wav}")

    lines += ["", "## Failed takes", ""]
    if failed:
        lines += table(["Take", "Stage", "What happened"], [[r.prepared.take.name, r.stage, r.message] for r in failed])
    else:
        lines.append("None.")
    return "\n".join(lines) + "\n"


def write_report(dirs: SessionDirs, opts: Options, info: SessionInfo, results: List[TakeResult]) -> Path:
    path = dirs.root / "report.md"
    dirs.root.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(render_report(opts, info, results))
    print(f"Report: {path}")
    return path


# ####################
# CATALOGUE AND MAPPING CHECK
# ####################


def catalog_to_doc(catalog: Any, firmware: str = "") -> Dict[str, Any]:
    """Every model of the unit's catalogue with its parameters, as a plain JSON document."""
    models = []
    for model in all_models(catalog):
        params = []
        for spec in model.parameters:
            params.append(
                {
                    "index": int(spec.index),
                    "name": spec.name,
                    "min": clean_number(float(spec.minimum)),
                    "max": clean_number(float(spec.maximum)),
                    "default": clean_number(float(getattr(spec, "default", 0.0))),
                    "units": getattr(spec, "units", "") or "",
                    "type": getattr(spec, "type", "") or "",
                    "steps": getattr(spec, "steps", None),
                    "placeholderRange": bool(getattr(spec, "range_is_placeholder", False)),
                }
            )
        models.append(
            {
                "id": int(model.id),
                "name": model.name,
                "category": model.category,
                "factory": bool(getattr(model, "is_factory", True)),
                "hidden": bool(getattr(model, "hidden", False)),
                "superseded": bool(getattr(model, "superseded", False)),
                "parameters": params,
            }
        )
    return {"format": CATALOG_FORMAT, "version": VERSION, "source": "quad-cortex", "sourceVersion": firmware, "models": models}


def check_mapping(doc: Dict[str, Any], mapping: Dict[str, Any]) -> Dict[str, Any]:
    """The unit's catalogue document against the mapping table. The mapping is only read."""
    unit_models = doc.get("models", [])
    by_name: Dict[str, List[Dict[str, Any]]] = {}
    for model in unit_models:
        by_name.setdefault(model_key(model["name"]), []).append(model)

    report_models = []
    counts = {"modelsMapped": 0, "modelsFound": 0, "paramsMapped": 0, "paramsFound": 0, "issues": 0}
    for name, entry in (mapping.get("models") or {}).items():
        counts["modelsMapped"] += 1
        matches = by_name.get(model_key(name), [])
        item: Dict[str, Any] = {
            "mappingName": name,
            "kind": entry.get("kind"),
            "verified": bool(entry.get("verified")),
            "found": bool(matches),
            "matches": [{"id": m["id"], "name": m["name"], "category": m["category"]} for m in matches],
            "closeNames": [],
            "params": [],
            "notMappedOnUnit": [],
        }
        if matches:
            counts["modelsFound"] += 1
        else:
            item["closeNames"] = difflib.get_close_matches(name, [m["name"] for m in unit_models], n=5, cutoff=0.6)
            counts["issues"] += 1
        mapped_names = set()
        for qc_name, spec in (entry.get("params") or {}).items():
            counts["paramsMapped"] += 1
            mapped_names.add(norm(qc_name))
            row: Dict[str, Any] = {
                "name": qc_name,
                "ourParam": spec.get("param"),
                "mappingUnit": spec.get("unit", ""),
                "transform": describe_transform(spec.get("transform")),
                "found": False,
                "foundOn": [],
                "issues": [],
            }
            for model in matches:
                hit = next((p for p in model["parameters"] if norm(p["name"]) == norm(qc_name)), None)
                if hit is None:
                    continue
                row["foundOn"].append(model["id"])
                if "unit" not in row:
                    row.update(unitOnUnit=hit["units"], range=[hit["min"], hit["max"]], type=hit["type"],
                               unitName=hit["name"], placeholderRange=hit["placeholderRange"])
            row["found"] = bool(row["foundOn"])
            if matches and not row["found"]:
                row["issues"].append("no parameter of this name on the unit")
                row["available"] = sorted({p["name"] for m in matches for p in m["parameters"]})
            elif matches and len(row["foundOn"]) < len(matches):
                row["issues"].append(f"only on model ids {row['foundOn']} of {[m['id'] for m in matches]}")
            if row["found"]:
                counts["paramsFound"] += 1
                unit_text = (row["unitOnUnit"] or "").strip().lower()
                map_text = (row["mappingUnit"] or "").strip().lower()
                if map_text and unit_text != map_text:
                    row["issues"].append(f"unit mismatch: mapping says {row['mappingUnit']!r}, unit says {row['unitOnUnit']!r}")
                low, high = row["range"]
                if row["placeholderRange"]:
                    row["issues"].append("the unit publishes the placeholder range 0..1: pyquadcortex cannot convert real units")
                transform = spec.get("transform")
                if isinstance(transform, dict) and ("scale" in transform or "offset" in transform):
                    a, b = float(transform.get("scale", 1.0)), float(transform.get("offset", 0.0))
                    row["transformedRange"] = [clean_number(low * a + b), clean_number(high * a + b)]
                if map_text == "%" and high <= 1.0 and not row["placeholderRange"]:
                    row["issues"].append("unit '%' but the range is 0..1 or smaller: check whether the value is a fraction")
            counts["issues"] += len(row["issues"])
            item["params"].append(row)
        seen = set()
        for model in matches[:1]:
            for p in model["parameters"]:
                if norm(p["name"]) not in mapped_names and norm(p["name"]) not in seen:
                    seen.add(norm(p["name"]))
                    item["notMappedOnUnit"].append({"name": p["name"], "units": p["units"], "min": p["min"], "max": p["max"], "type": p["type"]})
        report_models.append(item)

    mapped_categories = set((mapping.get("categories") or {}).keys())
    unit_categories = sorted({m["category"] for m in unit_models})
    return {
        "format": "pedaleira-qc-mapping-check",
        "version": VERSION,
        "sourceVersion": doc.get("sourceVersion", ""),
        "summary": counts,
        "models": report_models,
        "categories": {
            "mappedButNotOnUnit": sorted(c for c in mapped_categories if norm(c) not in {norm(u) for u in unit_categories}),
            "onUnitButNotInMapping": [c for c in unit_categories if norm(c) not in {norm(k) for k in mapped_categories}],
        },
    }


def render_mapping_check(result: Dict[str, Any]) -> str:
    s = result["summary"]
    lines = [
        "# Quad Cortex mapping check",
        "",
        f"Unit firmware: {result.get('sourceVersion') or 'unknown'}. The mapping table (resources/import/quad-cortex.json) was only read.",
        "",
        f"- Mapped models found on the unit: {s['modelsFound']} of {s['modelsMapped']}",
        f"- Mapped parameters found on the unit: {s['paramsFound']} of {s['paramsMapped']}",
        f"- Problems listed below: {s['issues']}",
        "",
        "## Models",
    ]
    for m in result["models"]:
        status = "found" if m["found"] else "NOT ON THE UNIT"
        ids = ", ".join(f"{x['id']} ({x['category']})" for x in m["matches"])
        lines += ["", f"### {m['mappingName']}: {status}", ""]
        lines.append(f"Our kind {m['kind']}, mapping verified: {str(m['verified']).lower()}." + (f" Unit ids: {ids}." if ids else ""))
        if m["closeNames"]:
            lines.append(f"Close names on the unit: {', '.join(m['closeNames'])}.")
        if m["params"]:
            rows = []
            for p in m["params"]:
                rng = f"{fmt(p['range'][0])} to {fmt(p['range'][1])}" if "range" in p else ""
                rows.append([p["name"], p["ourParam"], "yes" if p["found"] else "NO", p["mappingUnit"],
                             p.get("unitOnUnit", ""), rng, p["transform"], "; ".join(p["issues"])])
            lines += [""] + table(["QC parameter", "Ours", "On unit", "Mapping unit", "Unit's unit", "Unit's range", "Transform", "Problems"], rows)
        for p in m["params"]:
            if p.get("available"):
                lines.append(f"\nParameters the unit has for {m['mappingName']}: {', '.join(p['available'])}.")
                break
        if m["notMappedOnUnit"]:
            rows = [[p["name"], p["units"], f"{fmt(p['min'])} to {fmt(p['max'])}", p["type"]] for p in m["notMappedOnUnit"]]
            lines += ["", "Parameters on the unit that the mapping does not use:", ""] + table(["Name", "Unit", "Range", "Type"], rows)
    cats = result["categories"]
    lines += ["", "## Categories", ""]
    lines.append("In the mapping, not on the unit: " + (", ".join(cats["mappedButNotOnUnit"]) or "none") + ".")
    lines.append("On the unit, not in the mapping: " + (", ".join(cats["onUnitButNotInMapping"]) or "none") + ".")
    return "\n".join(lines) + "\n"


def read_catalog_doc(opts_catalog_file: Optional[Path], deps: Deps) -> Dict[str, Any]:
    """The catalogue document: from a file, or read from the unit (read only)."""
    if opts_catalog_file:
        doc = load_json(opts_catalog_file)
        if doc.get("format") != CATALOG_FORMAT:
            raise PlanError(f"{opts_catalog_file} is not a catalogue file written by --catalog")
        return doc
    kit = deps.kit or pyquadcortex_toolkit()
    qc = open_unit(kit)
    try:
        firmware = ""
        try:
            firmware = f"CorOS {qc.version().app_fw_version}"
        except Exception:
            pass
        return catalog_to_doc(qc.catalog, firmware)
    finally:
        qc.close()


def run_catalog(out_dir: Path, deps: Deps) -> int:
    doc = read_catalog_doc(None, deps)
    write_json(out_dir / "qc-catalog.json", doc)
    models = doc["models"]
    print(f"{len(models)} models, {sum(len(m['parameters']) for m in models)} parameters written to {out_dir / 'qc-catalog.json'}")
    print("Read only: nothing was changed on the unit.")
    return EXIT_OK


def run_check_mapping(out_dir: Path, mapping_path: Path, catalog_file: Optional[Path], deps: Deps) -> int:
    mapping = load_json(mapping_path)
    doc = read_catalog_doc(catalog_file, deps)
    if not catalog_file:
        write_json(out_dir / "qc-catalog.json", doc)
    result = check_mapping(doc, mapping)
    write_json(out_dir / "mapping-check.json", result)
    (out_dir / "mapping-check.md").write_text(render_mapping_check(result), encoding="utf-8")
    s = result["summary"]
    print(f"Models found {s['modelsFound']} of {s['modelsMapped']}, parameters found {s['paramsFound']} of {s['paramsMapped']}, "
          f"{s['issues']} problem(s).")
    print(f"Report: {out_dir / 'mapping-check.md'} (send it back with {out_dir / 'qc-catalog.json'})")
    print("Read only: nothing was changed on the unit, and the mapping table was not modified.")
    return EXIT_OK


# ####################
# COMMAND LINE
# ####################


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Automatic effect matching sessions against your own Quad Cortex over USB. "
        "Never saves anything on the unit. Without --go a session is a dry run."
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--catalog", action="store_true", help="read the unit's block catalogue (read only) into --out/qc-catalog.json")
    mode.add_argument("--check-mapping", action="store_true", help="compare the catalogue with resources/import/quad-cortex.json")
    parser.add_argument("--plan", metavar="FILE", help="the plan: a JSON file of takes (see tools/match/README.md)")
    run = parser.add_mutually_exclusive_group()
    run.add_argument("--go", action="store_true", help="run the session on the unit (default: a dry run)")
    run.add_argument("--dry-run", action="store_true", help="print the plan without contacting the unit (the default)")
    parser.add_argument("--dir", default="match", metavar="DIR", help="session folder for signal, ref, ours, analysis and the report (default: match)")
    parser.add_argument("--out", default="qc-reports", metavar="DIR", help="folder for --catalog and --check-mapping files (default: qc-reports)")
    parser.add_argument("--catalog-file", metavar="FILE", help="with --check-mapping: use a catalogue written earlier instead of the unit")
    parser.add_argument("--mapping", default=str(DEFAULT_MAPPING), metavar="FILE", help="the mapping table (default: resources/import/quad-cortex.json)")
    scratch = parser.add_argument_group("scratch preset (recalled on the unit, never saved)")
    scratch.add_argument("--scratch-preset", default=DEFAULT_SCRATCH_PRESET, metavar="NAME", help=f"preset recalled as scratch (default: {DEFAULT_SCRATCH_PRESET!r})")
    scratch.add_argument("--scratch-slot", metavar="SLOT", help='a slot name such as "02C" instead of a name')
    scratch.add_argument("--scratch-setlist", choices=["factory", "user"], default="factory", help="default: factory")
    scratch.add_argument("--use-current", action="store_true", help="work on the preset on the grid now (recalls nothing)")
    scratch.add_argument("--row", type=int, choices=[1, 2, 3, 4], help="the row to use, as the unit numbers it (default: the first free)")
    ports = parser.add_argument_group("routing")
    ports.add_argument("--qc-input", default=DEFAULT_QC_INPUT, help=f"the row's input on the unit (pyquadcortex Input name, default {DEFAULT_QC_INPUT})")
    ports.add_argument("--qc-output", default=DEFAULT_QC_OUTPUT, help=f"the row's output on the unit (pyquadcortex Output name, default {DEFAULT_QC_OUTPUT})")
    ports.add_argument("--device", default=DEFAULT_DEVICE, help=f"the audio device name for PedaleiraMatch record (default {DEFAULT_DEVICE!r})")
    ports.add_argument("--out-ch", default=DEFAULT_OUT_CH, help=f"device output channels the signal is played on, 1 based (default {DEFAULT_OUT_CH})")
    ports.add_argument("--in-ch", default=DEFAULT_IN_CH, help=f"device input channels recorded, 1 based (default {DEFAULT_IN_CH})")
    ports.add_argument("--type", dest="signal_type", help="passed to PedaleiraMatch record --type")
    ports.add_argument("--rate", type=int, default=DEFAULT_RATE, help="sample rate (default 48000)")
    ports.add_argument("--buffer", type=int, help="passed to PedaleiraMatch record --buffer")
    parser.add_argument("--settle", type=float, default=DEFAULT_SETTLE_S, help=f"seconds to wait after the grid edits (default {DEFAULT_SETTLE_S})")
    parser.add_argument("--tail", type=float, default=DEFAULT_TAIL_S, help=f"seconds PedaleiraRender renders past the signal (default {DEFAULT_TAIL_S:g})")
    parser.add_argument("--render-out-channels", type=int, dest="out_channels", help="PedaleiraRender --out-channels (default: as many as --in-ch records)")
    parser.add_argument("--match-tool", default=str(default_tool("PedaleiraMatch")), help="path of PedaleiraMatch")
    parser.add_argument("--render-tool", default=str(default_tool("PedaleiraRender")), help="path of PedaleiraRender")
    parser.add_argument("--yes", action="store_true", help="do not ask before recalling the scratch preset")
    parser.add_argument("--overwrite", action="store_true", help="replace recordings that exist already (default: the take fails)")
    parser.add_argument("--no-readback", action="store_true",
                        help="do not read the parameters back from the unit after the edits (default: read them and put a table per take in the report)")
    return parser.parse_args(argv)


def options_from(args: argparse.Namespace) -> Options:
    return Options(
        plan=Path(args.plan) if args.plan else None,
        go=args.go,
        out_dir=Path(args.dir),
        mapping=Path(args.mapping),
        scratch_preset=args.scratch_preset,
        scratch_slot=args.scratch_slot,
        scratch_setlist=args.scratch_setlist,
        use_current=args.use_current,
        row=args.row,
        qc_input=args.qc_input,
        qc_output=args.qc_output,
        device=args.device,
        out_ch=args.out_ch,
        in_ch=args.in_ch,
        signal_type=args.signal_type,
        rate=args.rate,
        buffer=args.buffer,
        settle=args.settle,
        tail=args.tail,
        # Ours is rendered with as many channels as are recorded, so the
        # comparison sees stereo on both sides (or mono on both).
        out_channels=args.out_channels or len([c for c in args.in_ch.split(",") if c.strip()]),
        match_tool=args.match_tool,
        render_tool=args.render_tool,
        yes=args.yes,
        overwrite=args.overwrite,
        readback=not args.no_readback,
    )


def check_channels(text: str, label: str) -> None:
    if not re.fullmatch(r"\d+(,\d+)*", text):
        raise PlanError(f"{label} must be channel numbers such as 3,4 (got {text!r})")


def dry_run(opts: Options, takes: List[Take], mapping: Dict[str, Any]) -> int:
    """Prints the plan. Does not import pyquadcortex, does not contact the unit, writes nothing."""
    if opts.qc_input.upper() not in KNOWN_INPUTS:
        raise PlanError(f"unknown input port {opts.qc_input!r}; known: {', '.join(KNOWN_INPUTS)}")
    if opts.qc_output.upper() not in KNOWN_OUTPUTS:
        raise PlanError(f"unknown output port {opts.qc_output!r}; known: {', '.join(KNOWN_OUTPUTS)}")
    dirs = SessionDirs.at(opts.out_dir)
    print("DRY RUN. The unit is not contacted and nothing is written. Add --go to run it.\n")
    print("\n".join(plan_lines(opts, prepare_offline(takes, mapping), dirs, False)))
    print("\nA real session: connects, stops if the unit has unsaved edits, recalls the scratch preset,")
    print("edits the grid of one free row, and ends with: " + NOTHING_SAVED)
    return EXIT_OK


def main(argv: Optional[List[str]] = None, deps: Optional[Deps] = None) -> int:
    args = parse_args(argv)
    deps = deps or Deps()
    try:
        if args.catalog:
            return run_catalog(Path(args.out), deps)
        if args.check_mapping:
            return run_check_mapping(Path(args.out), Path(args.mapping), Path(args.catalog_file) if args.catalog_file else None, deps)
        if not args.plan:
            raise PlanError("--plan FILE is needed (or --catalog, or --check-mapping)")
        opts = options_from(args)
        check_channels(opts.out_ch, "--out-ch")
        check_channels(opts.in_ch, "--in-ch")
        takes = parse_plan(load_json(Path(args.plan)))
        mapping = load_json(opts.mapping)
        if not opts.go:
            return dry_run(opts, takes, mapping)
        return run_session(opts, takes, mapping, deps)
    except PlanError as error:
        print(str(error), file=sys.stderr)
        return EXIT_USAGE
    except UnitRefused as refused:
        print(str(refused), file=sys.stderr)
        return refused.code


if __name__ == "__main__":
    sys.exit(main())
