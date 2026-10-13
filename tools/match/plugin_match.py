#!/usr/bin/env python3
"""Matching sessions against a plugin you own, offline (docs/matching.md, "Matching against a plugin").

The reference is a VST3 plugin (or an Audio Unit on macOS) hosted by PedaleiraMatch
(`plugin-render`) instead of a unit played over USB. For each take of a plan the script renders the
test signal through the plugin with the take's parameters, renders our equivalent effect with
PedaleiraRender over the same signal, analyses and compares both with PedaleiraMatch, and writes a
Markdown report in the same form as qc_match.py. It exists to measure references and tune our own
models. Nothing a third-party plugin produces is shipped or committed:

  * the output folder (--dir) must be outside this repository, and the script refuses it otherwise;
  * a plugin or state file inside this repository is refused (the repository's own test plugin,
    PedaleiraTestFx, excepted);
  * nothing here uses the network.

    python3 plugin_match.py --plan plan.json --dir ~/PedaleiraMatch/amp1            # dry run
    python3 plugin_match.py --plan plan.json --dir ~/PedaleiraMatch/amp1 --go       # render

You need a licence for the plugin and have to check its terms: measuring it for your own use is
what this is for. The planning, sweep expansion, command building and report code are pure and
tested without any plugin or binary (test_plugin_match.py).
"""

import argparse
import datetime
import json
import math
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qc_match as qm  # noqa: E402
from qc_match import (  # noqa: E402
    COMMON_PARAMS,
    DEFAULT_RATE,
    DEFAULT_TAIL_S,
    EXIT_OK,
    EXIT_TAKES_FAILED,
    EXIT_USAGE,
    PlanError,
    SessionDirs,
    TakeFiles,
    ToolFailed,
    cell,
    default_tool,
    fmt,
    norm,
    run_tool,
    safe_stem,
    shown,
    table,
)

FORMAT = "pedaleira-plugin-match-plan"
VERSION = 1
DEFAULT_BLOCK = 512

REPO = qm.REPO
# The one plugin allowed to live inside the repository: the test suite's own.
TEST_PLUGIN_NAME = "PedaleiraTestFx.vst3"

NOTHING_SHIPPED = (
    "References rendered from a third-party plugin stay in your data folder: nothing the plugin "
    "produced (audio, presets, impulse responses, states) goes into the repository or a release."
)


# ####################
# PATHS AND SAFETY (pure)
# ####################


def _is_within(path: Path, folder: Path) -> bool:
    try:
        path.relative_to(folder)
        return True
    except ValueError:
        return False


def inside_repo(path: Any, repo: Path = REPO) -> bool:
    """True when `path` is inside this repository's checkout (or any checkout of it: a folder holding
    .git and plugin/src/EffectMatch.cpp), whether or not it exists yet."""
    resolved = Path(os.path.expanduser(str(path))).resolve()
    if _is_within(resolved, repo.resolve()):
        return True
    for folder in [resolved] + list(resolved.parents):
        if (folder / ".git").exists() and (folder / "plugin" / "src" / "EffectMatch.cpp").is_file():
            return True
    return False


def is_test_plugin(path: Any) -> bool:
    """The repository's own test plugin, wherever the build tree puts it."""
    resolved = Path(os.path.expanduser(str(path)))
    return TEST_PLUGIN_NAME in resolved.parts


def check_output_dir(path: Path, repo: Path = REPO) -> None:
    if inside_repo(path, repo):
        raise PlanError(
            f"--dir {path} is inside the repository. What a plugin produces must stay outside it: "
            "pick a folder in your own data folder."
        )


def check_plugin_files(plugin: str, state: Optional[str], repo: Path = REPO) -> None:
    """Refuses a plugin or state file inside the repository (the test plugin excepted)."""
    if plugin.startswith("AudioUnit:"):
        return
    if inside_repo(plugin, repo) and not is_test_plugin(plugin):
        raise PlanError(f"the plugin {plugin} is inside the repository, which only the test plugin may be")
    if state and inside_repo(state, repo):
        raise PlanError(f"the state file {state} is inside the repository; keep plugin states in your data folder")


# ####################
# THE PLAN (pure)
# ####################


@dataclass(frozen=True)
class Setting:
    """One parameter of the plugin to set: by name or by row, to a normalised value or to text."""

    by: str  # "name" or "index"
    key: Any  # the name, or the row
    kind: str  # "value" or "text"
    value: Any  # a float in 0..1, or the text

    def args(self) -> List[str]:
        if self.kind == "text":
            return ["--set-text", f"{self.key}={self.value}"]
        number = format(float(self.value), ".10g")
        return ["--set-index" if self.by == "index" else "--set", f"{self.key}={number}"]

    def slot(self) -> Tuple[str, Any]:
        return (self.by, self.key if self.by == "index" else norm(self.key))

    def describe(self) -> str:
        who = f"#{self.key}" if self.by == "index" else str(self.key)
        what = f"{self.value!r} (text)" if self.kind == "text" else f"{fmt(float(self.value))} (normalised)"
        return f"{who} = {what}"


def parse_setting(where: str, key: Any, raw: Any) -> Setting:
    """A plan's parameter entry: name: number | name: {"text": "..."} | label: {"index": n, "value": v}."""
    if isinstance(key, str) and ("=" in key or not key.strip()):
        raise PlanError(f"{where}: parameter name {key!r} is empty or holds '='; use the index form")
    if isinstance(raw, bool):
        raw = 1.0 if raw else 0.0
    if isinstance(raw, (int, float)):
        return Setting("name", str(key).strip(), "value", unit_value(where, key, raw))
    if isinstance(raw, str):
        return Setting("name", str(key).strip(), "text", raw)
    if isinstance(raw, dict):
        extra = set(raw) - {"text", "index", "value"}
        if extra:
            raise PlanError(f"{where}: parameter {key!r} has unknown field(s) {sorted(extra)}")
        has_index = "index" in raw
        if has_index:
            index = raw["index"]
            if isinstance(index, bool) or not isinstance(index, int) or index < 0:
                raise PlanError(f"{where}: parameter {key!r}: index must be a row number from 0")
            if "value" not in raw or "text" in raw:
                raise PlanError(f'{where}: parameter {key!r}: an index entry is {{"index": n, "value": 0..1}}')
            return Setting("index", index, "value", unit_value(where, key, raw["value"]))
        if "text" in raw and "value" not in raw:
            if not isinstance(raw["text"], str) or not raw["text"].strip():
                raise PlanError(f"{where}: parameter {key!r}: text must be a non-empty string")
            return Setting("name", str(key).strip(), "text", raw["text"])
        if "value" in raw and "text" not in raw:
            return Setting("name", str(key).strip(), "value", unit_value(where, key, raw["value"]))
    raise PlanError(
        f"{where}: parameter {key!r} must be a number 0..1, a text, {{\"text\": \"...\"}} or {{\"index\": n, \"value\": v}}"
    )


def unit_value(where: str, key: Any, value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise PlanError(f"{where}: parameter {key!r} must be a number")
    if not 0.0 <= float(value) <= 1.0:
        raise PlanError(
            f'{where}: parameter {key!r} = {value}: a normalised value is 0 to 1 (use {{"text": "..."}} for the plugin\'s own units)'
        )
    return float(value)


def parse_params(where: str, params: Any) -> List[Setting]:
    if params is None:
        return []
    if not isinstance(params, dict):
        raise PlanError(f"{where}: params must be an object")
    return [parse_setting(where, key, raw) for key, raw in params.items()]


@dataclass
class Take:
    """One take after sweeps are expanded: one reference render and one render of ours."""

    name: str
    plugin: str
    state: Optional[str] = None
    settings: List[Setting] = field(default_factory=list)
    ours_kind: Optional[str] = None
    ours_params: Dict[str, float] = field(default_factory=dict)
    notes: str = ""
    sweep_group: str = ""  # the plan take a sweep take came from
    sweep_label: str = ""  # "Gain = 0.25"


def merge_settings(*groups: Sequence[Setting]) -> List[Setting]:
    """Settings in order; a later one for the same parameter replaces the earlier one in place."""
    merged: Dict[Tuple[str, Any], Setting] = {}
    for group in groups:
        for setting in group:
            merged[setting.slot()] = setting
    return list(merged.values())


def value_label(raw: Any) -> str:
    if isinstance(raw, str):
        return safe_stem(raw)[:24] or "text"
    return f"{float(raw):g}".replace(".", "p").replace("-", "m").replace("+", "")


def parse_sweep(where: str, raw: Any) -> Tuple[Setting, List[Setting]]:
    """A sweep: {"param": name or row, "values": [...]}. Returns the settings, one per value."""
    if not isinstance(raw, dict) or set(raw) - {"param", "values"}:
        raise PlanError(f'{where}: a sweep is {{"param": "Gain", "values": [0, 0.5, 1]}}')
    param, values = raw.get("param"), raw.get("values")
    if isinstance(param, bool) or not isinstance(param, (str, int)) or param == "":
        raise PlanError(f"{where}: sweep.param must be a parameter name or a row number")
    if not isinstance(values, list) or not values:
        raise PlanError(f"{where}: sweep.values must be a non-empty list")
    out: List[Setting] = []
    for number, value in enumerate(values, 1):
        what = f"{where} sweep value {number}"
        if isinstance(param, int):
            if param < 0:
                raise PlanError(f"{what}: row numbers start at 0")
            if isinstance(value, dict) or isinstance(value, str):
                raise PlanError(f"{what}: a sweep by row takes normalised numbers (sweep by name to use text)")
            out.append(Setting("index", param, "value", unit_value(what, param, value)))
        else:
            if isinstance(value, dict) and set(value) == {"text"} and isinstance(value["text"], str) and value["text"].strip():
                out.append(Setting("name", param.strip(), "text", value["text"]))
            elif isinstance(value, str) and value.strip():
                out.append(Setting("name", param.strip(), "text", value))
            elif isinstance(value, dict):
                raise PlanError(f'{what}: a text value is {{"text": "..."}} or a string')
            else:
                out.append(Setting("name", param.strip(), "value", unit_value(what, param, value)))
    return out[0], out


def parse_ours(where: str, raw: Any) -> Tuple[Optional[str], Dict[str, float], Optional[Tuple[str, List[float]]]]:
    """ours: kind, params, the common settings and an optional parallel sweep."""
    if raw is None:
        return None, {}, None
    if not isinstance(raw, dict):
        raise PlanError(f"{where}: ours must be an object")
    kind = raw.get("kind")
    if kind is not None and (not isinstance(kind, str) or not kind):
        raise PlanError(f"{where}: ours.kind must be a kind id such as delay.digital")
    merged = dict(raw.get("params") or {})
    for ident in COMMON_PARAMS:
        if ident in raw:
            merged[ident] = raw[ident]
    params: Dict[str, float] = {}
    for ident, value in merged.items():
        if isinstance(value, bool):
            value = 1 if value else 0
        if isinstance(value, (int, float)) and math.isfinite(float(value)):
            params[ident] = float(value)
        else:
            raise PlanError(f"{where}: ours setting {ident!r} must be a number")
    sweep = None
    if "sweep" in raw:
        spec = raw["sweep"]
        if not isinstance(spec, dict) or set(spec) - {"param", "values"}:
            raise PlanError(f'{where}: ours.sweep is {{"param": "gain", "values": [1, 2, 3]}}')
        ident, values = spec.get("param"), spec.get("values")
        if not isinstance(ident, str) or not ident:
            raise PlanError(f"{where}: ours.sweep.param must be one of our parameter ids")
        if not isinstance(values, list) or not values or not all(
            isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(float(v)) for v in values
        ):
            raise PlanError(f"{where}: ours.sweep.values must be a non-empty list of numbers")
        sweep = (ident, [float(v) for v in values])
    if kind is None and (params or sweep):
        raise PlanError(f"{where}: ours has settings but no kind")
    return kind, params, sweep


def parse_plugin(where: str, raw: Any, base: Dict[str, Any]) -> Dict[str, Any]:
    """The plugin block of the plan or of a take, laid over `base` (path, state, params)."""
    out = {"path": base.get("path"), "state": base.get("state"), "params": list(base.get("params", []))}
    if raw is None:
        return out
    if not isinstance(raw, dict):
        raise PlanError(f"{where}: plugin must be an object")
    extra = set(raw) - {"path", "state", "params"}
    if extra:
        raise PlanError(f"{where}: unknown plugin field(s) {sorted(extra)}")
    if "path" in raw:
        if not isinstance(raw["path"], str) or not raw["path"].strip():
            raise PlanError(f"{where}: plugin.path must be a file path (or an AudioUnit: identifier)")
        out["path"] = raw["path"].strip()
    if "state" in raw:
        if raw["state"] is not None and (not isinstance(raw["state"], str) or not raw["state"].strip()):
            raise PlanError(f"{where}: plugin.state must be a file path")
        out["state"] = raw["state"].strip() if raw["state"] else None
    if "params" in raw:
        out["params"] = merge_settings(out["params"], parse_params(where, raw["params"]))
    return out


def parse_plan(document: Any) -> List[Take]:
    """The takes of a plan, sweeps expanded: {"format", "version", "plugin": {...}, "takes": [...]}."""
    if not isinstance(document, dict):
        raise PlanError("the plan must be an object with a list of takes")
    if document.get("format") not in (None, FORMAT):
        raise PlanError(f"the plan's format must be {FORMAT!r}")
    raw_takes = document.get("takes")
    if not isinstance(raw_takes, list) or not raw_takes:
        raise PlanError("the plan needs a non-empty list of takes")

    problems: List[str] = []
    shared = parse_plugin("plugin", document.get("plugin"), {})

    takes: List[Take] = []
    seen: Dict[str, str] = {}
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
        extra = set(raw) - {"name", "plugin", "ours", "sweep", "notes"}
        if extra:
            problems.append(f"{where}: unknown field(s) {sorted(extra)}")
        try:
            plugin = parse_plugin(where, raw.get("plugin"), shared)
            if not plugin["path"]:
                raise PlanError(f"{where}: no plugin.path (give one at the top of the plan or in the take)")
            kind, ours_params, ours_sweep = parse_ours(where, raw.get("ours"))
            sweep_settings: List[Setting] = []
            if "sweep" in raw:
                _, sweep_settings = parse_sweep(where, raw["sweep"])
            if ours_sweep is not None:
                if not sweep_settings:
                    raise PlanError(f"{where}: ours.sweep needs the take to sweep a plugin parameter too")
                if len(ours_sweep[1]) != len(sweep_settings):
                    raise PlanError(
                        f"{where}: ours.sweep has {len(ours_sweep[1])} values but the plugin sweep has {len(sweep_settings)}"
                    )
        except PlanError as error:
            problems.append(str(error))
            continue

        variants: List[Tuple[str, List[Setting], Dict[str, float]]] = []
        labels: List[str] = []
        if not sweep_settings:
            variants.append(("", plugin["params"], ours_params))
            labels.append("")
        else:
            sweep_param = raw["sweep"]["param"]
            for position, (setting, value) in enumerate(zip(sweep_settings, raw["sweep"]["values"])):
                text = value["text"] if isinstance(value, dict) else value
                ours_here = dict(ours_params)
                if ours_sweep is not None:
                    ours_here[ours_sweep[0]] = ours_sweep[1][position]
                variants.append((f"{position + 1:02d}-{value_label(text)}", merge_settings(plugin["params"], [setting]), ours_here))
                labels.append(f"{sweep_param} = {text}")

        for (suffix, settings, ours_here), label in zip(variants, labels):
            take_name = f"{name}-{suffix}" if suffix else name
            if norm(take_name) in seen:
                problems.append(f"{where}: the name {take_name!r} is used twice (also by {seen[norm(take_name)]})")
            seen[norm(take_name)] = where
            takes.append(
                Take(
                    name=take_name,
                    plugin=plugin["path"],
                    state=plugin["state"],
                    settings=settings,
                    ours_kind=kind,
                    ours_params=ours_here,
                    notes=str(raw.get("notes", "")),
                    sweep_group=name if suffix else "",
                    sweep_label=label,
                )
            )
    if problems:
        raise PlanError("the plan has problems:\n  " + "\n  ".join(problems))
    return takes


# ####################
# THE TOOLS (command lines, pure)
# ####################


def plugin_render_cmd(
    match_tool: str,
    take: Take,
    signal: str,
    out_wav: str,
    tail: float = DEFAULT_TAIL_S,
    block: int = DEFAULT_BLOCK,
    channels: Optional[int] = None,
) -> List[str]:
    """PedaleiraMatch plugin-render for a take: state first, then the settings in order."""
    cmd = [match_tool, "plugin-render", "--plugin", expand(take.plugin)]
    if take.state:
        cmd += ["--state", expand(take.state)]
    for setting in take.settings:
        cmd += setting.args()
    cmd += ["--in", signal, "--out", out_wav, "--tail", f"{tail:g}", "--block", str(block)]
    if channels:
        cmd += ["--channels", str(channels)]
    return cmd


def expand(path: str) -> str:
    return path if path.startswith("AudioUnit:") else os.path.expanduser(path)


def reference_channels(info: Dict[str, Any]) -> int:
    value = info.get("channels")
    return value if isinstance(value, int) and value in (1, 2) else 2


# ####################
# SESSION
# ####################


@dataclass
class Options:
    plan: Optional[Path] = None
    go: bool = False
    out_dir: Optional[Path] = None
    tail: float = DEFAULT_TAIL_S
    block: int = DEFAULT_BLOCK
    channels: Optional[int] = None
    rate: int = DEFAULT_RATE
    match_tool: str = str(default_tool("PedaleiraMatch"))
    render_tool: str = str(default_tool("PedaleiraRender"))
    overwrite: bool = False


@dataclass
class TakeResult:
    take: Take
    files: TakeFiles
    status: str = "pending"  # ok, recorded (reference only), failed
    stage: str = ""
    message: str = ""
    summary: Optional[Dict[str, Any]] = None
    reference: Dict[str, Any] = field(default_factory=dict)  # the JSON PedaleiraMatch wrote beside the wav


def plan_lines(opts: Options, takes: Sequence[Take], dirs: SessionDirs) -> List[str]:
    out = [f"Output folder: {dirs.root}", f"{len(takes)} take(s), tail {opts.tail:g} s, block {opts.block}, {opts.rate} Hz."]
    out.append(shown(qm.signals_cmd(opts.match_tool, str(dirs.signal), opts.rate)))
    for number, take in enumerate(takes, 1):
        files = TakeFiles.for_take(dirs, take.name)
        out += ["", f"[{number}/{len(takes)}] {take.name}" + (f"  ({take.sweep_label})" if take.sweep_label else "")]
        if take.notes:
            out.append(f"  notes: {take.notes}")
        out.append(f"  plugin: {take.plugin}" + (f", state {take.state}" if take.state else ""))
        for setting in take.settings:
            out.append(f"    set {setting.describe()}")
        out.append("  reference: " + shown(plugin_render_cmd(opts.match_tool, take, "SIGNAL", files.ref_wav, opts.tail, opts.block, opts.channels)))
        out.append("  analyse:   " + shown(qm.analyse_cmd(opts.match_tool, files.ref_wav, files.ref_analysis, opts.rate)))
        if take.ours_kind is None:
            out.append("  ours: none in the plan, the reference is recorded and analysed only")
            continue
        common = {k: v for k, v in take.ours_params.items() if k in COMMON_PARAMS}
        out.append(f"  ours: {take.ours_kind} " + (", ".join(f"{k}={fmt(v)}" for k, v in sorted(take.ours_params.items())) or "(defaults)"))
        out.append("  render ours: " + shown(qm.render_cmd(opts.render_tool, files.preset, "SIGNAL", files.ours_wav, opts.tail,
                                                           files.script if common else None, opts.channels or 2)))
        out.append("  compare:   " + shown(qm.compare_cmd(opts.match_tool, files.ref_analysis, files.ours_analysis,
                                                         files.comparison, files.markdown, opts.rate)))
    return out


def check_inputs(opts: Options, takes: Sequence[Take], strict: bool) -> List[str]:
    """The safety checks (always raise) and the existence checks (raise when `strict`, else notes)."""
    notes: List[str] = []
    for plugin, state in sorted({(t.plugin, t.state) for t in takes}, key=str):
        check_plugin_files(plugin, state)
        if not plugin.startswith("AudioUnit:") and not os.path.exists(os.path.expanduser(plugin)):
            message = f"plugin not found: {plugin}"
            if strict:
                raise PlanError(message)
            notes.append(message)
        if state and not os.path.isfile(os.path.expanduser(state)):
            message = f"state file not found: {state}"
            if strict:
                raise PlanError(message)
            notes.append(message)
    return notes


def check_tools(opts: Options, takes: Sequence[Take]) -> None:
    tools = [("PedaleiraMatch", opts.match_tool)]
    if any(t.ours_kind for t in takes):
        tools.append(("PedaleiraRender", opts.render_tool))
    for label, path in tools:
        if not os.path.isfile(path) and not shutil_which(path):
            raise PlanError(f"{label} was not found at {path}. Build it (cmake --build build --target {label}) or pass its path.")


def shutil_which(path: str) -> Optional[str]:
    import shutil

    return shutil.which(path)


def write_json(path: Path, document: Any) -> None:
    qm.write_json(path, document)


def run_take(opts: Options, take: Take, files: TakeFiles, dirs: SessionDirs, signal: str, result: TakeResult) -> None:
    """Reference render (plugin), then ours, analyse both, compare. Fills `result`."""
    if os.path.exists(files.ref_wav) and not opts.overwrite:
        raise ToolFailed("reference", [], 1, f"{files.ref_wav} already exists (a reference is a measurement; pass --overwrite to replace it)")
    run_tool("reference", plugin_render_cmd(opts.match_tool, take, signal, files.ref_wav, opts.tail, opts.block, opts.channels), timeout=1800)
    if not os.path.isfile(files.ref_wav):
        raise ToolFailed("reference", [], 1, f"plugin-render exited cleanly but {files.ref_wav} is not there")
    info_path = Path(files.ref_wav).with_suffix(".json")
    if info_path.is_file():
        with open(info_path, "r", encoding="utf-8") as handle:
            result.reference = json.load(handle)
    run_tool("analyse reference", qm.analyse_cmd(opts.match_tool, files.ref_wav, files.ref_analysis, opts.rate))
    if take.ours_kind is None:
        result.status = "recorded"
        return
    common = {k: v for k, v in take.ours_params.items() if k in COMMON_PARAMS}
    kind_params = {k: v for k, v in take.ours_params.items() if k not in COMMON_PARAMS}
    write_json(Path(files.preset), qm.build_preset(take.name, take.ours_kind, kind_params))
    script = None
    if common:
        script = files.script
        write_json(Path(files.script), qm.build_commands(common))
    run_tool("render ours", qm.render_cmd(opts.render_tool, files.preset, signal, files.ours_wav, opts.tail, script,
                                          reference_channels(result.reference) if opts.channels is None else opts.channels))
    run_tool("analyse ours", qm.analyse_cmd(opts.match_tool, files.ours_wav, files.ours_analysis, opts.rate))
    run_tool("compare", qm.compare_cmd(opts.match_tool, files.ref_analysis, files.ours_analysis, files.comparison,
                                       files.markdown, opts.rate))
    with open(files.comparison, "r", encoding="utf-8") as handle:
        result.summary = qm.summarise_comparison(json.load(handle))
    result.status = "ok"


def run_session(opts: Options, takes: List[Take], now: Optional[datetime.datetime] = None) -> int:
    dirs = SessionDirs.at(opts.out_dir)
    started = (now or datetime.datetime.now()).isoformat(timespec="seconds")
    check_inputs(opts, takes, strict=True)
    check_tools(opts, takes)
    for folder in dirs:
        folder.mkdir(parents=True, exist_ok=True)
    results: List[TakeResult] = []
    aborted = ""
    try:
        run_tool("signals", qm.signals_cmd(opts.match_tool, str(dirs.signal), opts.rate))
        signal = qm.find_signal(dirs)
        for number, take in enumerate(takes, 1):
            files = TakeFiles.for_take(dirs, take.name)
            result = TakeResult(take=take, files=files)
            results.append(result)
            print(f"\n[{number}/{len(takes)}] {take.name}")
            try:
                run_take(opts, take, files, dirs, signal, result)
            except ToolFailed as failure:
                result.status, result.stage, result.message = "failed", failure.stage, str(failure)
            except (OSError, ValueError, KeyError) as error:
                result.status, result.stage, result.message = "failed", "files", f"{type(error).__name__}: {error}"
            score = result.summary["score"] if result.summary else None
            print(f"  {result.status}" + (f", score {score}" if score is not None else "") + (f": {result.message}" if result.message else ""))
    except ToolFailed as failure:
        aborted = str(failure)
        print(aborted, file=sys.stderr)
    except KeyboardInterrupt:
        aborted = "stopped by the user"
        print("\nStopped.", file=sys.stderr)
    finally:
        if results or aborted:
            path = dirs.root / "report.md"
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(render_report(opts, started, aborted, results))
            print(f"Report: {path}")
        print("\n" + NOTHING_SHIPPED)
    if aborted or any(r.status == "failed" for r in results):
        return EXIT_TAKES_FAILED
    return EXIT_OK


# ####################
# THE REPORT (pure)
# ####################


def render_report(opts: Options, started: str, aborted: str, results: List[TakeResult]) -> str:
    ok = [r for r in results if r.status == "ok"]
    failed = [r for r in results if r.status == "failed"]
    lines = ["# Plugin matching session", ""]
    lines.append(f"Started {started or 'unknown'}. {opts.rate} Hz, tail {opts.tail:g} s, block {opts.block}.")
    lines += ["", f"**{NOTHING_SHIPPED}**"]
    if aborted:
        lines += ["", f"**The session stopped early:** {aborted}"]
    lines += ["", f"{len(results)} take(s): {len(ok)} compared, "
              f"{len([r for r in results if r.status == 'recorded'])} reference only, {len(failed)} failed.", ""]

    if results:
        rows = []
        for r in results:
            score = f"{r.summary['score']:.1f}" if r.summary and r.summary.get("score") is not None else ""
            plugin = (r.reference.get("plugin") or {}).get("name", "") or os.path.basename(r.take.plugin)
            rows.append([r.take.name, plugin, r.take.sweep_label or "-", r.take.ours_kind or "-", score, r.status])
        lines += ["## Summary", ""] + table(["Take", "Plugin", "Sweep", "Our kind", "Score", "Result"], rows) + [""]

    lines.append("## Takes")
    for r in results:
        t = r.take
        lines += ["", f"### {t.name}", ""]
        if t.notes:
            lines += [t.notes, ""]
        info = r.reference
        plug = info.get("plugin") or {}
        label = plug.get("name") or os.path.basename(t.plugin)
        version = f" {plug['version']}" if plug.get("version") else ""
        lines.append(f"Plugin: {label}{version} ({plug.get('format', 'format unknown')}), file {t.plugin}")
        if t.state:
            lines.append(f"State: {t.state}")
        if info:
            lines.append(f"Latency: {info.get('latencySamples', '?')} samples, compensated. "
                         f"Channels: {info.get('channels', '?')} (plugin {info.get('inputChannels', '?')} in, {info.get('outputChannels', '?')} out).")
        lines.append("")
        shown_params = info.get("parameters")
        if shown_params:
            rows = [[p.get("name"), json.dumps(p.get("requested", {}), ensure_ascii=False), fmt(p.get("value")), p.get("text", "")]
                    for p in shown_params]
            lines += table(["Plugin parameter", "Requested", "Normalised", "Shown as"], rows) + [""]
        elif t.settings:
            lines += table(["Plugin parameter", "Requested"], [[s.key, s.describe()] for s in t.settings]) + [""]
        else:
            lines += ["No parameters set (the plugin's own state).", ""]
        if t.ours_kind is not None:
            lines.append(f"Our side: {t.ours_kind}")
            rows = [[k, fmt(v), "plan"] for k, v in sorted(t.ours_params.items())]
            if rows:
                lines += [""] + table(["Our setting", "Value", "From"], rows)
            lines.append("")
        if r.status == "failed":
            lines += [f"**Failed at {r.stage}:** {r.message}", ""]
        elif r.status == "recorded":
            lines += ["Rendered and analysed; no kind on our side, so nothing was compared.", ""]
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
        lines += table(["Take", "Stage", "What happened"], [[r.take.name, r.stage, r.message] for r in failed])
    else:
        lines.append("None.")
    return "\n".join(lines) + "\n"


# ####################
# COMMAND LINE
# ####################


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Match your effects against a plugin you own, offline. Without --go this is a dry run. "
        "Nothing the plugin produces goes into the repository."
    )
    parser.add_argument("--plan", required=True, metavar="FILE", help="the plan: a JSON file (see tools/match/README.md)")
    run = parser.add_mutually_exclusive_group()
    run.add_argument("--go", action="store_true", help="render the references and ours (default: a dry run)")
    run.add_argument("--dry-run", action="store_true", help="print the plan only (the default)")
    parser.add_argument("--dir", metavar="DIR", help="where the session goes: signal/, ref/, ours/, analysis/, presets/ and report.md. "
                        "Outside the repository; required with --go")
    parser.add_argument("--tail", type=float, default=DEFAULT_TAIL_S, help=f"seconds rendered past the signal (default {DEFAULT_TAIL_S:g})")
    parser.add_argument("--block", type=int, default=DEFAULT_BLOCK, help=f"the plugin's block size (default {DEFAULT_BLOCK})")
    parser.add_argument("--channels", type=int, choices=[1, 2], help="channels of the references and of ours (default: the plugin's output width)")
    parser.add_argument("--match-tool", default=str(default_tool("PedaleiraMatch")), help="path of PedaleiraMatch")
    parser.add_argument("--render-tool", default=str(default_tool("PedaleiraRender")), help="path of PedaleiraRender")
    parser.add_argument("--overwrite", action="store_true", help="replace references that exist already (default: the take fails)")
    return parser.parse_args(argv)


def options_from(args: argparse.Namespace) -> Options:
    return Options(
        plan=Path(args.plan),
        go=args.go,
        out_dir=Path(os.path.expanduser(args.dir)) if args.dir else None,
        tail=args.tail,
        block=args.block,
        channels=args.channels,
        match_tool=args.match_tool,
        render_tool=args.render_tool,
        overwrite=args.overwrite,
    )


def dry_run(opts: Options, takes: List[Take]) -> int:
    notes = check_inputs(opts, takes, strict=False)
    dirs = SessionDirs.at(opts.out_dir or Path("DIR"))
    print("DRY RUN. No plugin is loaded and nothing is written. Add --go to render.\n")
    print("\n".join(plan_lines(opts, takes, dirs)))
    for note in notes:
        print(f"\nNOTE: {note}")
    if opts.out_dir is None:
        print("\nNOTE: --dir is needed with --go (a folder outside the repository).")
    print("\n" + NOTHING_SHIPPED)
    return EXIT_OK


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    try:
        opts = options_from(args)
        if opts.tail < 0 or opts.tail > 60:
            raise PlanError("--tail must be 0 to 60 seconds")
        if opts.block < 16 or opts.block > 16384:
            raise PlanError("--block must be 16 to 16384")
        if opts.out_dir is not None:
            check_output_dir(opts.out_dir)
        takes = parse_plan(qm.load_json(opts.plan))
        if not opts.go:
            return dry_run(opts, takes)
        if opts.out_dir is None:
            raise PlanError("--dir DIR is needed with --go")
        return run_session(opts, takes)
    except PlanError as error:
        print(str(error), file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
