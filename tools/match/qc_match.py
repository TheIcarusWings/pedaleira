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
import datetime
import difflib
import json
import math
import os
import re
import shlex
import shutil
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


def parse_plan(document: Any) -> List[Take]:
    """The takes of a plan: {"format": ..., "takes": [...]} or a bare list of takes."""
    if isinstance(document, dict):
        if document.get("format") not in (None, FORMAT):
            raise PlanError(f"the plan's format must be {FORMAT!r}")
        raw_takes = document.get("takes")
    else:
        raw_takes = document
    if not isinstance(raw_takes, list) or not raw_takes:
        raise PlanError("the plan needs a non-empty list of takes")

    problems: List[str] = []
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

        params: Dict[str, Any] = {}
        raw_params = raw.get("params", {})
        if not isinstance(raw_params, dict):
            problems.append(f"{where}: params must be an object of name: value")
            raw_params = {}
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
        if bypass and params:
            problems.append(f"{where}: a bypass take has no params")

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
        takes.append(
            Take(
                name=name,
                model=None if bypass else model,
                params=params,
                bypass=bypass,
                ours_kind=ours_kind,
                ours_params=ours_params,
                notes=str(raw.get("notes", "")),
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
                "mix": 0.5,
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
) -> List[str]:
    """PedaleiraMatch record: plays the test signal on out_ch and records in_ch into out_dir/name.wav."""
    cmd = [match_tool, "record", "--device", device, "--out-ch", out_ch, "--in-ch", in_ch, "--name", name, "--dir", out_dir]
    if signal_type:
        cmd += ["--type", signal_type]
    cmd += ["--rate", str(rate)]
    if buffer:
        cmd += ["--buffer", str(buffer)]
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


def ensure_clean(qc: GuardedClient, sleep: Callable[[float], None]) -> None:
    """Stops when the grid holds unsaved edits. The first request after connecting is sometimes dropped."""
    for attempt in range(3):
        try:
            dirty = qc.preset_dirty()
            break
        except TimeoutError:
            sleep(0.5)
    else:
        raise UnitRefused(
            "The unit did not say whether the preset on the grid has unsaved edits (three tries). "
            "Nothing was touched, because a session never works on an unknown state. Run this again.",
            EXIT_DIRTY,
        )
    if dirty:
        raise UnitRefused(DIRTY_MESSAGE, EXIT_DIRTY)


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
    """A catalogue model by id or by name (case, spaces and punctuation ignored). Ambiguity is an error."""
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


def plan_setting(spec: Any, shown: Any) -> UnitSetting:
    units = getattr(spec, "units", "") or ""
    if isinstance(shown, dict):
        return UnitSetting(spec.name, shown["wire"], "wire 0..1", {"value": float(shown["wire"])})
    count = getattr(spec, "option_count", None)
    if count:
        index = int(shown)
        if index != shown or not 0 <= index < count:
            raise PlanError(f"{spec.name!r} is a list of {count} options; give an index from 0 to {count - 1}")
        return UnitSetting(spec.name, index, f"option {index} of {count}", {"value": float(spec.option_to_value(index))})
    if getattr(spec, "range_is_placeholder", False):
        raise PlanError(
            f"{spec.name!r} publishes the placeholder range 0..1 ({units}); give it as "
            '{"wire": 0..1} (pyquadcortex cannot convert it)'
        )
    note = ""
    low, high = float(spec.minimum), float(spec.maximum)
    if not low <= float(shown) <= high:
        note = f"outside the unit's range {fmt(low)} to {fmt(high)} {units}: the library clamps it"
    return UnitSetting(spec.name, shown, units, {"real": float(shown)}, note)


@dataclass
class PreparedTake:
    take: Take
    model: Any = None
    settings: List[UnitSetting] = field(default_factory=list)
    ours: OursPlan = field(default_factory=OursPlan)
    unit_notes: List[str] = field(default_factory=list)
    error: str = ""


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
            prepared.settings.append(plan_setting(find_parameter(prepared.model, name), shown))
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
    occupied: bool,
    sleep: Callable[[float], None],
) -> None:
    """The grid edits of one take, all on `row`, column 0. Nothing here stores anything."""
    if occupied:
        qc.remove_block(row=row, column=0)
        sleep(0.4)
    if not prepared.take.bypass:
        qc.set_block(row=row, column=0, model=int(prepared.model.id))
    qc.set_chain_input(row=row, in_portid=in_port)
    qc.set_chain_output(row=row, out_portid=out_port)
    if not prepared.take.bypass:
        qc.set_bypass(row=row, column=0, bypassed=False)
        for setting in prepared.settings:
            qc.set_param(row=row, column=0, param=setting.name, model=prepared.model, **setting.kwargs)


def confirm_setup(qc: GuardedClient, kit: Toolkit, prepared: PreparedTake, row: int, in_port: Any, out_port: Any) -> None:
    """Reads the live grid back (no side effects) and checks the block and the routing landed."""
    live = qc.read_current_preset()
    found = [(b.row, b.column, b.model_id) for b in kit.blocks(live) if b.row == row]
    if prepared.take.bypass:
        if found:
            raise UnitRefused(f"row {row + 1} still holds {len(found)} block(s) after the setup")
    elif (row, 0, int(prepared.model.id)) not in found:
        raise UnitRefused(f"{prepared.model.name!r} is not at row {row + 1}, column 1 after the setup")
    if len(found) > (0 if prepared.take.bypass else 1):
        raise UnitRefused(f"row {row + 1} holds more blocks than this take put there")
    read_in, read_out = chain_ports(live, row)
    if read_in is not None and read_in != int(in_port):
        raise UnitRefused(f"row {row + 1} input reads back as {read_in}, not {int(in_port)}")
    if read_out is not None and read_out != int(out_port):
        raise UnitRefused(f"row {row + 1} output reads back as {read_out}, not {int(out_port)}")


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


@dataclass
class TakeResult:
    prepared: PreparedTake
    files: TakeFiles
    status: str = "pending"  # ok, recorded (QC only), failed
    stage: str = ""
    message: str = ""
    summary: Optional[Dict[str, Any]] = None
    attempts: int = 0


@dataclass
class SessionInfo:
    started: str = ""
    firmware: str = ""
    scratch: str = ""
    row: int = 0
    aborted: str = ""


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
            lines.append(f"    set_chain_input {opts.qc_input}   set_chain_output {opts.qc_output}   set_bypass off")
            if item.settings:
                for setting in item.settings:
                    unit = f" {setting.unit}" if setting.unit else ""
                    lines.append(f"    set_param   {setting.name} = {fmt(setting.shown)}{unit}")
            else:
                for name, value in take.params.items():
                    lines.append(f"    set_param   {name} = {fmt(value)}  (units checked against the unit's catalogue when it runs)")
        lines.append(f"  wait {opts.settle:g} s, then record:")
        lines.append("    " + shown(record_cmd(opts.match_tool, opts.device, opts.out_ch, opts.in_ch, take.name,
                                               str(dirs.ref), opts.rate, opts.signal_type, opts.buffer)))
        ours = item.ours
        if ours.kind is None and not take.bypass:
            lines.append("  ours: no kind, so only the recording is made and analysed")
        else:
            lines.append(f"  ours: {ours.kind or 'an empty chain (bypass)'}")
            for p in ours.params + ours.common:
                origin = f"from {p.qc_name} ({p.transform})" if p.source == "mapping" else "from the plan"
                lines.append(f"    {p.ident} = {fmt(p.value)}   {origin}")
            lines.append("    " + shown(render_cmd(opts.render_tool, files.preset, signal, files.ours_wav, opts.tail,
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
            run_tool("record", record_cmd(opts.match_tool, opts.device, opts.out_ch, opts.in_ch, name, str(dirs.ref),
                                          opts.rate, opts.signal_type, opts.buffer), timeout=900)
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


def write_json(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def find_signal(dirs: SessionDirs) -> str:
    found = sorted(dirs.signal.glob("pedaleira-test-signal-v*.wav"))
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
    # The scratch preset this session recalled, recalled again at the end so its
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

        ensure_clean(qc, deps.sleep)
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
            setlist = kit.setlists.FACTORY if opts.scratch_setlist == "factory" else kit.setlists.USER
            position = opts.scratch_slot or qc.find_preset(opts.scratch_preset, setlist).index
            preset = qc.read_preset(setlist, position)
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

        occupied = False
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
                occupied = not item.take.bypass
                confirm_setup(qc, kit, item, row, in_port, out_port)
                deps.sleep(opts.settle)
                run_pipeline(opts, item, files, dirs, signal, result)
            except ToolFailed as failure:
                result.status, result.stage, result.message = "failed", failure.stage, str(failure)
            except kit.refused_errors as refused:
                occupied = False
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
            try:
                qc.read_preset(*recalled)
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
    parser.add_argument("--render-out-channels", type=int, dest="out_channels", help="PedaleiraRender --out-channels (default: the input's)")
    parser.add_argument("--match-tool", default=str(default_tool("PedaleiraMatch")), help="path of PedaleiraMatch")
    parser.add_argument("--render-tool", default=str(default_tool("PedaleiraRender")), help="path of PedaleiraRender")
    parser.add_argument("--yes", action="store_true", help="do not ask before recalling the scratch preset")
    parser.add_argument("--overwrite", action="store_true", help="replace recordings that exist already (default: the take fails)")
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
        out_channels=args.out_channels,
        match_tool=args.match_tool,
        render_tool=args.render_tool,
        yes=args.yes,
        overwrite=args.overwrite,
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
