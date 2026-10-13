#!/usr/bin/env python3
"""Tests for qc_match with fake pyquadcortex objects and a mocked subprocess.

No unit, no pyquadcortex, no network and no PedaleiraMatch / PedaleiraRender binaries needed.
Run:  python3 tools/match/test_qc_match.py
"""

import array
import ast
import contextlib
import datetime
import enum
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace as NS
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qc_match as qm  # noqa: E402

MAPPING_PATH = Path(qm.DEFAULT_MAPPING)


# ####################
# FAKES
# ####################


class FakeInput(enum.IntEnum):
    EMPTY = 0
    INPUT_1 = 1
    USB_5_6 = 12


class FakeOutput(enum.IntEnum):
    EMPTY = 0
    XLR_1_2 = 1
    USB_3_4 = 22


class FakeSpec:
    def __init__(self, index, name, low=0.0, high=10.0, units="", options=0, placeholder=False):
        self.index = index
        self.name = name
        self.minimum, self.maximum = low, high
        self.default = low
        self.units = units
        self.type = "comboBox" if options else ""
        self.steps = options or None
        self.option_count = options or None
        self.range_is_placeholder = placeholder

    def option_to_value(self, option):
        return option / (self.option_count - 1)

    def value_to_option(self, value):
        return round(value * (self.option_count - 1))


def fake_model(model_id, name, category, *specs):
    return NS(id=model_id, name=name, category=category, parameters=tuple(specs), is_factory=True, hidden=False, superseded=False)


def fake_catalog():
    models = [
        fake_model(6005, "Digital Delay", "Delay",
                   FakeSpec(0, "TIME", 0.0, 2000.0, "ms"), FakeSpec(1, "FEEDBACK", 0.0, 100.0, "%"),
                   FakeSpec(2, "MIX", 0.0, 100.0, "%"), FakeSpec(3, "TYPE", options=3), FakeSpec(4, "LEVEL", 0.0, 1.0, "dB", placeholder=True)),
        fake_model(5005, "VCA Comp", "Compressor", FakeSpec(0, "THRESHOLD", -60.0, 12.0, "dB"), FakeSpec(1, "RATIO", 1.0, 20.0, "")),
        fake_model(8003, "Hall", "Reverb", FakeSpec(0, "PRE DELAY", 0.0, 100.0, "ms"), FakeSpec(1, "MIX", 0.0, 100.0, "%")),
        fake_model(9001, "Twin", "Reverb"),
        fake_model(9002, "Twin", "Reverb"),
        fake_model(9101, "Phaser", "Modulation"),
        fake_model(9102, "Phaser", "Modulation"),
    ]
    models[-2].superseded = True
    return NS(models={m.id: m for m in models})


class FakeMessage(NS):
    def HasField(self, field):
        return field in self.__dict__


class FakeUnit:
    """Records every call; has the save methods too, so a refusal is the proxy's doing."""

    def __init__(self, dirty=False, free=(1, 3), occupied_rows=(), current_name="Fake Preset", loaded=None, stubborn=False):
        self.calls = []
        self.dirty = dirty
        self.current_name = current_name
        self.loaded = loaded  # position of the preset that is loaded; recalling it again clears nothing
        self.stubborn = stubborn  # the unsaved-edit flag never clears
        self.values = {}
        self.overrides = {}  # (row, column, index) -> what the unit holds instead of what was sent
        self.free = list(free)
        self.catalog = fake_catalog()
        self.cells = {}
        self.ports = {}
        self.params = []
        self.saved = []
        self.closed = False
        self.occupied_rows = list(occupied_rows)

    def _note(self, call, **kw):
        self.calls.append((call, kw))

    def version(self):
        self._note("version")
        return NS(app_fw_version="4.0.1")

    def preset_dirty(self):
        self._note("preset_dirty")
        return self.dirty

    def find_preset(self, name, setlist):
        self._note("find_preset", name=name)
        return NS(index=7, name=name)

    def read_preset(self, setlist, position):
        self._note("read_preset", position=position)
        if position != self.loaded:
            self.loaded = position
            self.current_name = "Brit 2203" if position == 7 else f"Factory {position}"
            self.cells.clear()
            self.ports.clear()
            self.values.clear()
            if not self.stubborn:
                self.dirty = False
        return self._preset()

    def list_presets(self, setlist):
        self._note("list_presets")
        return [NS(index=i, name="Brit 2203" if i == 7 else f"Factory {i}") for i in range(10)]

    def read_current_preset(self):
        self._note("read_current_preset")
        return self._preset()

    def set_block(self, row, column, model, **kw):
        self._note("set_block", row=row, column=column, model=model)
        self.cells[(row, column)] = model

    def remove_block(self, row, column):
        self._note("remove_block", row=row, column=column)
        self.cells.pop((row, column), None)

    def set_chain_input(self, row, in_portid):
        self._note("set_chain_input", row=row, in_portid=in_portid)
        self.ports[row] = (int(in_portid), self.ports.get(row, (None, None))[1])

    def set_chain_output(self, row, out_portid):
        self._note("set_chain_output", row=row, out_portid=out_portid)
        self.ports[row] = (self.ports.get(row, (None, None))[0], int(out_portid))

    def set_bypass(self, row, column, bypassed, scene=None):
        self._note("set_bypass", row=row, column=column, bypassed=bypassed)

    def set_param(self, row, column, **kw):
        self._note("set_param", row=row, column=column, **{k: v for k, v in kw.items() if k != "model"})
        self.params.append(kw)
        if "model" not in kw:
            return
        spec = next(s for s in kw["model"].parameters if s.name == kw["param"])
        wire = kw["value"] if "value" in kw else (kw["real"] - spec.minimum) / (spec.maximum - spec.minimum)
        key = (row, column, spec.index)
        self.values[key] = self.overrides.get(key, wire)

    def close(self):
        self.closed = True

    # The calls the guard must keep away.
    def save_current_preset(self, *a, **kw):
        self.saved.append("save_current_preset")

    def write_preset(self, *a, **kw):
        self.saved.append("write_preset")

    def update_settings(self, **kw):
        self.saved.append("update_settings")

    def set_input_port(self, *a, **kw):
        self.saved.append("set_input_port")

    def _preset(self):
        chains = []
        for row in range(4):
            fields = {"row": row}
            in_port, out_port = self.ports.get(row, (None, None))
            if in_port is not None:
                fields["in_portid"] = in_port
            if out_port is not None:
                fields["out_portid"] = out_port
            chains.append(FakeMessage(**fields))
        blocks = [NS(row=r, column=c, model_id=int(m)) for (r, c), m in self.cells.items()]
        blocks += [NS(row=r, column=0, model_id=1) for r in self.occupied_rows]
        return NS(name=self.current_name, chains=chains, blocks=blocks, stored=dict(self.values))


def fake_kit(unit=None, connect_error=None):
    def connect():
        if connect_error is not None:
            raise connect_error
        return unit

    return qm.Toolkit(
        connect=connect,
        free_rows=lambda preset: list(unit.free),
        blocks=lambda preset: preset.blocks,
        inputs=FakeInput,
        outputs=FakeOutput,
        setlists=NS(USER="user", FACTORY="factory"),
        device_errors=(OSError,),
        refused_errors=(KeyError,),
        lost_errors=(ConnectionError,),
        param_state=lambda preset, cell, index: NS(scene_mode=False, values=(preset.stored.get((cell.row, cell.column, index)),)),
    )


def fake_deps(kit):
    return qm.Deps(kit=kit, sleep=lambda s: None, ask=lambda prompt: "", now=lambda: datetime.datetime(2026, 1, 2, 3, 4, 5))


def write_plan(folder, takes):
    path = Path(folder) / "plan.json"
    path.write_text(json.dumps({"format": qm.FORMAT, "version": 1, "takes": takes}), encoding="utf-8")
    return path


DELAY_TAKE = {"name": "delay-250", "model": "Digital Delay", "params": {"TIME": 250, "FEEDBACK": 35, "MIX": 100},
              "ours": {"params": {"timeMs": 250}}}
COMP_TAKE = {"name": "comp-r4", "model": "VCA Comp", "params": {"THRESHOLD": -30}}


# ####################
# THE GUARD
# ####################


class GuardTest(unittest.TestCase):
    def test_refuses_every_save_and_settings_call(self):
        unit = FakeUnit()
        qc = qm.GuardedClient(unit)
        for name in qm.KNOWN_WRITES + ("reroute_grid_input", "set_lane_output", "_t", "_unit", "__dict__x"):
            with self.subTest(call=name):
                with self.assertRaises(qm.NeverSaveError):
                    getattr(qc, name)
        self.assertEqual(unit.saved, [])
        self.assertEqual(unit.calls, [])

    def test_known_writes_are_not_on_the_allow_list(self):
        self.assertEqual(set(qm.KNOWN_WRITES) & qm.ALLOWED_CALLS, set())
        for name in qm.ALLOWED_CALLS:
            self.assertFalse(name.startswith(("save_", "write_", "delete_", "update_")), name)

    def test_nothing_can_be_assigned(self):
        qc = qm.GuardedClient(FakeUnit())
        with self.assertRaises(qm.NeverSaveError):
            qc.save_current_preset = lambda: None
        with self.assertRaises(qm.NeverSaveError):
            del qc.version

    def test_allowed_calls_pass_through(self):
        unit = FakeUnit()
        with qm.GuardedClient(unit) as qc:
            self.assertEqual(qc.version().app_fw_version, "4.0.1")
            qc.set_block(row=1, column=0, model=6005)
            qc.set_chain_input(row=1, in_portid=FakeInput.USB_5_6)
            qc.set_chain_output(row=1, out_portid=FakeOutput.USB_3_4)
            qc.set_param(row=1, column=0, param="TIME", real=250.0)
            qc.set_bypass(row=1, column=0, bypassed=False)
            qc.remove_block(row=1, column=0)
            self.assertFalse(qc.preset_dirty())
        self.assertTrue(unit.closed)
        self.assertEqual(unit.saved, [])

    def test_source_only_uses_allowed_calls_on_qc(self):
        """Every attribute the script reads from the guarded client `qc` is on the allow-list."""
        tree = ast.parse(Path(qm.__file__).read_text(encoding="utf-8"))
        used = {n.attr for n in ast.walk(tree) if isinstance(n, ast.Attribute) and isinstance(n.value, ast.Name) and n.value.id == "qc"}
        self.assertTrue(used)
        self.assertEqual(used - qm.ALLOWED_CALLS, set())
        everything = {n.attr for n in ast.walk(tree) if isinstance(n, ast.Attribute)}
        self.assertEqual(everything & set(qm.KNOWN_WRITES), set())

    def test_source_has_no_dashes(self):
        for path in (qm.__file__, __file__):
            text = Path(path).read_text(encoding="utf-8")
            self.assertNotIn(chr(0x2013), text)
            self.assertNotIn(chr(0x2014), text)


# ####################
# PLAN AND MAPPING
# ####################


class PlanTest(unittest.TestCase):
    def test_parses_takes(self):
        takes = qm.parse_plan({"format": qm.FORMAT, "takes": [DELAY_TAKE, {"name": "bypass", "bypass": True}]})
        self.assertEqual([t.name for t in takes], ["delay-250", "bypass"])
        self.assertEqual(takes[0].params, {"TIME": 250, "FEEDBACK": 35, "MIX": 100})
        self.assertEqual(takes[0].ours_params, {"timeMs": 250.0})
        self.assertTrue(takes[1].bypass)
        self.assertIsNone(takes[1].model)

    def test_bare_list_and_wire_values(self):
        takes = qm.parse_plan([{"name": "x", "model": 5005, "params": {"THRESHOLD": {"wire": 0.5}, "SWITCH": True}}])
        self.assertEqual(takes[0].params, {"THRESHOLD": {"wire": 0.5}, "SWITCH": 1})

    def test_reports_all_problems(self):
        with self.assertRaises(qm.PlanError) as caught:
            qm.parse_plan([
                {"name": "a b", "model": "X"},
                {"name": "a-b", "model": "X"},
                {"name": "dup", "model": "X"},
                {"name": "dup", "model": "X"},
                {"name": "nomodel"},
                {"name": "badparam", "model": "X", "params": {"T": "slow"}},
                {"name": "wire", "model": "X", "params": {"T": {"wire": 2}}},
                {"model": "X"},
            ])
        text = str(caught.exception)
        for fragment in ("letters, digits", "used twice", "model must be", "parameter 'T'", "needs a name"):
            self.assertIn(fragment, text)

    def test_empty_plan_is_refused(self):
        with self.assertRaises(qm.PlanError):
            qm.parse_plan({"takes": []})
        with self.assertRaises(qm.PlanError):
            qm.parse_plan({"format": "other", "takes": [DELAY_TAKE]})


class MappingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mapping = json.loads(MAPPING_PATH.read_text(encoding="utf-8"))

    def derive(self, raw):
        take = qm.parse_plan([raw])[0]
        return qm.derive_ours(take, self.mapping)

    def test_delay_from_mapping_and_plan(self):
        ours = self.derive(DELAY_TAKE)
        self.assertEqual(ours.kind, "delay.digital")
        self.assertEqual(ours.kind_values(), {"feedback": 35.0, "timeMs": 250.0})
        self.assertEqual(ours.common_values(), {"mix": 1.0})  # MIX 100 percent, scale 0.01
        self.assertEqual(ours.unmapped, ["TIME"])
        self.assertFalse(ours.verified)
        text = " ".join(qm.mapping_statements(ours, "Digital Delay"))
        self.assertIn("UNVERIFIED", text)
        self.assertIn("'TIME'", text)

    def test_compressor_threshold(self):
        ours = self.derive(COMP_TAKE)
        self.assertEqual(ours.kind, "dynamics.compressor")
        self.assertEqual(ours.kind_values(), {"thresholdDb": -31.28})
        self.assertEqual(ours.unmapped, [])
        self.assertFalse(ours.verified)  # the entry says verified false

    def test_verified_only_when_entry_and_everything_mapped(self):
        mapping = {"models": {"M": {"kind": "k.x", "verified": True, "params": {"A": {"param": "a", "transform": "linear"}}}}}
        take = qm.parse_plan([{"name": "t", "model": "m", "params": {"a": 3}}])[0]
        self.assertTrue(qm.derive_ours(take, mapping).verified)
        extra = qm.parse_plan([{"name": "t", "model": "m", "params": {"a": 3, "B": 1}}])[0]
        self.assertFalse(qm.derive_ours(extra, mapping).verified)
        override = qm.parse_plan([{"name": "t", "model": "m", "params": {"a": 3}, "ours": {"kind": "k.y"}}])[0]
        self.assertFalse(qm.derive_ours(override, mapping).verified)

    def test_plan_kind_for_unmapped_model(self):
        ours = self.derive({"name": "t", "model": "Nope", "params": {"X": 1}, "ours": {"kind": "k.z", "params": {"p": 2}}})
        self.assertEqual(ours.kind, "k.z")
        self.assertEqual(ours.kind_values(), {"p": 2.0})
        self.assertEqual(ours.unmapped, ["X"])
        self.assertIn("no entry", " ".join(qm.mapping_statements(ours, "Nope")))

    def test_no_kind_means_qc_only(self):
        ours = self.derive({"name": "t", "model": "Nope"})
        self.assertIsNone(ours.kind)
        self.assertIn("Only the Quad Cortex side", " ".join(ours.notes))

    def test_plan_overrides_mapped_value(self):
        ours = self.derive({"name": "t", "model": "Digital Delay", "params": {"FEEDBACK": 35}, "ours": {"params": {"feedback": 40}, "mix": 0.5}})
        self.assertEqual(ours.kind_values(), {"feedback": 40.0})
        self.assertEqual(ours.common_values(), {"mix": 0.5})

    def test_transforms(self):
        self.assertEqual(qm.apply_transform("linear", 5), (True, 5.0))
        self.assertEqual(qm.apply_transform({"scale": 0.01, "offset": 0}, 50), (True, 0.5))
        self.assertEqual(qm.apply_transform({"map": {"Free": 0, "Synced": 1}}, "synced"), (True, 1.0))
        self.assertFalse(qm.apply_transform({"map": {"Free": 0}}, 3)[0])
        self.assertFalse(qm.apply_transform("linear", "x")[0])

    def test_bypass_take(self):
        ours = self.derive({"name": "bypass", "bypass": True})
        self.assertIsNone(ours.kind)
        self.assertTrue(ours.verified)


# ####################
# COMMAND LINES AND FILES
# ####################


class CommandTest(unittest.TestCase):
    def test_a_curved_parameter_goes_to_the_unit_on_its_curve(self):
        # Measured on a unit: wire 0.14614 played 249.4 ms, where linear 250 ms played 35 ms.
        spec = FakeSpec(0, "DELAY TIME", 7.0, 6000.0, "ms")
        exponent = qm.curve_exponent(qm.CURVES, "Digital Delay (M)", "DELAY TIME")
        self.assertAlmostEqual(exponent, 5.0 / 3.0, places=5)
        setting = qm.plan_setting(spec, 250, exponent)
        self.assertAlmostEqual(setting.kwargs["value"], 0.14614, places=4)
        self.assertNotIn("real", setting.kwargs)
        self.assertIsNone(qm.curve_exponent(qm.CURVES, "VCA Comp (M)", "THRESHOLD"))
        self.assertEqual(qm.plan_setting(spec, 250).kwargs, {"real": 250.0})

    def test_ours_is_fully_wet_unless_the_plan_says_otherwise(self):
        # A half-wet block (the old default) blended a compressor 50/50 with the dry signal.
        block = qm.build_preset("c", "dynamics.compressor", {"ratio": 4})["chain"]["children"][0]["children"][0]
        self.assertEqual(block["mix"], 1.0)

    def test_ours_is_rendered_with_as_many_channels_as_are_recorded(self):
        stereo = qm.options_from(qm.parse_args(["--plan", "p.json"]))
        self.assertEqual(stereo.out_channels, 2)
        mono = qm.options_from(qm.parse_args(["--plan", "p.json", "--in-ch", "3"]))
        self.assertEqual(mono.out_channels, 1)
        given = qm.options_from(qm.parse_args(["--plan", "p.json", "--render-out-channels", "1"]))
        self.assertEqual(given.out_channels, 1)

    def test_record(self):
        self.assertEqual(
            qm.record_cmd("PM", "Quad Cortex", "5,6", "3,4", "delay-250", "/s/ref"),
            ["PM", "record", "--device", "Quad Cortex", "--out-ch", "5,6", "--in-ch", "3,4", "--name", "delay-250", "--dir", "/s/ref", "--rate", "48000"],
        )
        self.assertEqual(
            qm.record_cmd("PM", "QC", "5,6", "3,4", "t", "/s/ref", 48000, "effect", 256),
            ["PM", "record", "--device", "QC", "--out-ch", "5,6", "--in-ch", "3,4", "--name", "t", "--dir", "/s/ref",
             "--type", "effect", "--rate", "48000", "--buffer", "256"],
        )

    def test_render(self):
        self.assertEqual(
            qm.render_cmd("PR", "/p/t.json", "/sig.wav", "/o/t.wav"),
            ["PR", "--preset", "/p/t.json", "--in", "/sig.wav", "--out", "/o/t.wav", "--tail", "4"],
        )
        self.assertEqual(
            qm.render_cmd("PR", "/p/t.json", "/sig.wav", "/o/t.wav", 2.5, "/p/t.commands.json", 2),
            ["PR", "--preset", "/p/t.json", "--run", "/p/t.commands.json", "--in", "/sig.wav", "--out", "/o/t.wav",
             "--tail", "2.5", "--out-channels", "2"],
        )

    def test_signals_analyse_compare(self):
        self.assertEqual(qm.signals_cmd("PM", "/s/signal", 48000), ["PM", "signals", "--out", "/s/signal", "--rate", "48000"])
        self.assertEqual(qm.analyse_cmd("PM", "/r.wav", "/r.json"), ["PM", "analyse", "--in", "/r.wav", "--out", "/r.json", "--rate", "48000"])
        self.assertEqual(
            qm.compare_cmd("PM", "/r.json", "/o.json", "/c.json", "/c.md"),
            ["PM", "compare", "--reference", "/r.json", "--candidate", "/o.json", "--out", "/c.json", "--markdown", "/c.md", "--rate", "48000"],
        )

    def test_preset_and_commands(self):
        preset = qm.build_preset("delay-250", "delay.digital", {"timeMs": 250.0, "feedback": 35.0})
        self.assertEqual(json.loads(json.dumps(preset)), preset)
        block = preset["chain"]["children"][0]["children"][0]
        self.assertEqual(block["id"], "b1")
        state = block["children"][0]
        self.assertEqual(state["kind"], "delay.digital")
        self.assertEqual(state["children"], [{"node": "Param", "id": "feedback", "value": 35.0}, {"node": "Param", "id": "timeMs", "value": 250.0}])
        self.assertEqual(qm.build_preset("bypass", None, {})["chain"]["children"][0]["children"], [])
        self.assertEqual(
            qm.build_commands({"mix": 1.0, "inputGain": 0.5}),
            [{"command": "block.setParam", "args": {"blockId": "b1", "param": "mix", "value": 1.0}},
             {"command": "block.setParam", "args": {"blockId": "b1", "param": "inputGain", "value": 0.5}}],
        )

    def test_summary_of_a_comparison(self):
        summary = qm.summarise_comparison(COMPARISON)
        self.assertEqual(summary["score"], 75.0)
        self.assertEqual(summary["groups"]["sweep"], {"passed": 2, "judged": 2, "score": 100.0})
        self.assertEqual(summary["groups"]["static"]["score"], 50.0)
        self.assertEqual([m["name"] for m in summary["failed"]], ["step 3"])


COMPARISON = {
    "score": 75.0, "allPass": False, "judged": 4, "passed": 3,
    "metrics": [
        {"group": "sweep", "name": "band 1k", "unit": "dB", "reference": 0, "candidate": 0.2, "difference": 0.2, "allowed": 1, "judged": True, "pass": True, "note": ""},
        {"group": "sweep", "name": "level", "unit": "dB", "reference": 0, "candidate": 0.1, "difference": 0.1, "allowed": 1, "judged": True, "pass": True, "note": ""},
        {"group": "static", "name": "step 2", "unit": "dB", "reference": -3, "candidate": -3, "difference": 0, "allowed": 1, "judged": True, "pass": True, "note": ""},
        {"group": "static", "name": "step 3", "unit": "dB", "reference": -6, "candidate": -9, "difference": -3, "allowed": 1, "judged": True, "pass": False, "note": ""},
        {"group": "alignment", "name": "latencyMs", "unit": "ms", "reference": 5, "candidate": 0, "difference": -5, "allowed": 1, "judged": False, "pass": True, "note": "not judged"},
    ],
}


# ####################
# THE SESSION
# ####################


class ToolMock:
    """Stands in for subprocess.run: PedaleiraMatch and PedaleiraRender write the files they would."""

    def __init__(self, fail_record_once=(), always_fail_record=(), record_output="", fail_output=""):
        self.record_output = record_output
        self.fail_output = fail_output
        self.calls = []
        self.fail_record_once = set(fail_record_once)
        self.always_fail_record = set(always_fail_record)
        self.record_attempts = {}

    def __call__(self, argv, **kwargs):
        argv = list(argv)
        self.calls.append(argv)
        tool = os.path.basename(argv[0])
        if tool == "PedaleiraRender":
            Path(argv[argv.index("--out") + 1]).parent.mkdir(parents=True, exist_ok=True)
            Path(argv[argv.index("--out") + 1]).write_bytes(b"RIFF")
            return self.done(argv, 0)
        command = argv[1]
        if command == "signals":
            out = Path(argv[argv.index("--out") + 1])
            out.mkdir(parents=True, exist_ok=True)
            qm.write_float_wav(out / qm.DEFAULT_SIGNAL_NAME, 48000, 1, array.array("f", [0.0, 1.0, -0.5, 0.25]))
            return self.done(argv, 0)
        if command == "record":
            name = argv[argv.index("--name") + 1]
            self.record_attempts[name] = self.record_attempts.get(name, 0) + 1
            if name in self.always_fail_record or (name in self.fail_record_once and self.record_attempts[name] == 1):
                return self.done(argv, 3, self.fail_output, "take check failed: no sync marker")
            folder = Path(argv[argv.index("--dir") + 1])
            folder.mkdir(parents=True, exist_ok=True)
            (folder / f"{name}.wav").write_bytes(b"RIFF")
            return self.done(argv, 0, self.record_output)
        if command == "analyse":
            Path(argv[argv.index("--out") + 1]).write_text("{}", encoding="utf-8")
            return self.done(argv, 0)
        if command == "compare":
            Path(argv[argv.index("--out") + 1]).write_text(json.dumps(COMPARISON), encoding="utf-8")
            Path(argv[argv.index("--markdown") + 1]).write_text("# Effect match\n", encoding="utf-8")
            return self.done(argv, 0, "", "score 75")
        raise AssertionError(f"unexpected command {argv}")

    @staticmethod
    def done(argv, code, out="", err=""):
        return subprocess.CompletedProcess(argv, code, out, err)

    def names(self, command):
        return [c for c in self.calls if os.path.basename(c[0]) == "PedaleiraMatch" and c[1] == command]


class SessionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.match_tool = self.root / "PedaleiraMatch"
        self.render_tool = self.root / "PedaleiraRender"
        for tool in (self.match_tool, self.render_tool):
            tool.write_text("", encoding="utf-8")

    def argv(self, plan, *extra):
        return ["--plan", str(plan), "--dir", str(self.root / "session"), "--match-tool", str(self.match_tool),
                "--render-tool", str(self.render_tool), "--settle", "0", *extra]

    def run_main(self, argv, deps, tools=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            with mock.patch.object(qm.subprocess, "run", side_effect=tools or ToolMock()):
                code = qm.main(argv, deps)
        return code, out.getvalue(), err.getvalue()

    def test_dry_run_makes_no_grid_edits_and_does_not_connect(self):
        unit = FakeUnit()
        kit = fake_kit(unit, connect_error=AssertionError("a dry run must not connect"))
        plan = write_plan(self.root, [DELAY_TAKE, COMP_TAKE, {"name": "bypass", "bypass": True}])
        for extra in ([], ["--dry-run"]):
            tools = ToolMock()
            code, out, err = self.run_main(self.argv(plan, *extra), fake_deps(kit), tools)
            self.assertEqual(code, 0, err)
            self.assertEqual(unit.calls, [])
            self.assertEqual(tools.calls, [])
            self.assertFalse((self.root / "session").exists())
            self.assertIn("DRY RUN", out)
            self.assertIn("set_block", out)
            self.assertIn("PedaleiraMatch record", out.replace(str(self.match_tool), "PedaleiraMatch"))
            self.assertIn("Nothing was saved", out)

    def test_dry_run_checks_port_names_offline(self):
        plan = write_plan(self.root, [COMP_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--qc-input", "USB_99"), fake_deps(fake_kit(FakeUnit())))
        self.assertEqual(code, qm.EXIT_USAGE)
        self.assertIn("unknown input port", err)

    def test_go_and_dry_run_conflict(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            qm.parse_args(["--plan", "p.json", "--go", "--dry-run"])

    def test_dirty_preset_stops_before_any_edit(self):
        unit = FakeUnit(dirty=True)
        plan = write_plan(self.root, [COMP_TAKE])
        tools = ToolMock()
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)), tools)
        self.assertEqual(code, qm.EXIT_DIRTY)
        self.assertIn("unsaved edits", err)
        names = [c[0] for c in unit.calls]
        self.assertEqual(names, ["version", "preset_dirty", "read_current_preset"])  # a preset that is not the scratch
        self.assertEqual(tools.calls, [])
        self.assertTrue(unit.closed)
        self.assertEqual(unit.saved, [])
        self.assertIn("Nothing was saved", out)

    def test_device_that_cannot_be_opened(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit, connect_error=OSError("busy"))))
        self.assertEqual(code, qm.EXIT_NO_DEVICE)
        self.assertIn("Cortex Control", err)
        self.assertEqual(unit.calls, [])

    def test_no_free_row(self):
        unit = FakeUnit(free=())
        plan = write_plan(self.root, [COMP_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_REFUSED)
        self.assertIn("no free row", err)
        self.assertNotIn("set_block", [c[0] for c in unit.calls])

    def test_missing_tools_are_reported_before_connecting(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE])
        argv = self.argv(plan, "--go", "--yes", "--match-tool", str(self.root / "nope"))
        code, out, err = self.run_main(argv, fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_USAGE)
        self.assertIn("was not found", err)
        self.assertEqual(unit.calls, [])

    def test_full_session(self):
        unit = FakeUnit()
        takes = [
            DELAY_TAKE,
            COMP_TAKE,
            {"name": "bypass", "bypass": True},
            {"name": "bad-model", "model": "No Such Model"},
            {"name": "retry", "model": "Hall", "params": {"PRE DELAY": 20, "MIX": 100}},
            {"name": "hard-fail", "model": "Hall", "params": {"MIX": 50}},
            {"name": "last", "model": "Hall", "params": {"MIX": 10}, "ours": {}, "notes": "x"},
        ]
        plan = write_plan(self.root, takes)
        tools = ToolMock(fail_record_once={"retry"}, always_fail_record={"hard-fail"})
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes", "--row", "4"), fake_deps(fake_kit(unit)), tools)
        self.assertEqual(code, qm.EXIT_TAKES_FAILED, err)

        # The never-save rule held, and the unit was left alone otherwise.
        self.assertEqual(unit.saved, [])
        self.assertTrue(unit.closed)
        allowed = qm.ALLOWED_CALLS
        self.assertTrue({c[0] for c in unit.calls} <= allowed)
        self.assertIn("Nothing was saved", out)

        by_name = {}
        for name, kw in unit.calls:
            by_name.setdefault(name, []).append(kw)
        self.assertEqual([kw["position"] if "position" in kw else kw["name"] for kw in by_name["find_preset"] + by_name["read_preset"]],
                         ["Brit 2203", 7, 0, 7])
        # The scratch preset is recalled again at the end, after every grid edit.
        names_in_order = [c[0] for c in unit.calls]
        last_edit = max(i for i, n in enumerate(names_in_order) if n.startswith("set_") or n == "remove_block")
        self.assertIn("read_preset", names_in_order[last_edit:])
        self.assertIn("recalled again", out)
        # Row 4 (index 3), column 0, USB 5/6 in, USB 3/4 out.
        for kw in by_name["set_block"]:
            self.assertEqual((kw["row"], kw["column"]), (3, 0))
        self.assertEqual(by_name["set_block"][0]["model"], 6005)
        self.assertTrue(all(kw["in_portid"] == FakeInput.USB_5_6 and kw["row"] == 3 for kw in by_name["set_chain_input"]))
        self.assertTrue(all(kw["out_portid"] == FakeOutput.USB_3_4 for kw in by_name["set_chain_output"]))
        delay_params = unit.params[:3]
        self.assertEqual([(p["param"], p["real"]) for p in delay_params], [("TIME", 250.0), ("FEEDBACK", 35.0), ("MIX", 100.0)])

        # Recording, with one retry for "retry" and a failure for "hard-fail".
        self.assertEqual(tools.record_attempts["retry"], 2)
        self.assertEqual(tools.record_attempts["hard-fail"], 2)
        self.assertEqual(tools.record_attempts["delay-250"], 1)
        self.assertNotIn("bad-model", tools.record_attempts)
        record = tools.names("record")[0]
        self.assertEqual(record[record.index("--out-ch") + 1], "5,6")
        self.assertEqual(record[record.index("--in-ch") + 1], "3,4")
        self.assertEqual(record[record.index("--dir") + 1], str(self.root / "session" / "ref"))

        # Our side: a preset per compared take, a command script where there are common settings.
        presets = self.root / "session" / "presets"
        preset = json.loads((presets / "delay-250.json").read_text(encoding="utf-8"))
        self.assertEqual(preset["chain"]["children"][0]["children"][0]["children"][0]["kind"], "delay.digital")
        commands = json.loads((presets / "delay-250.commands.json").read_text(encoding="utf-8"))
        self.assertEqual(commands[0]["args"], {"blockId": "b1", "param": "mix", "value": 1.0})
        self.assertFalse((presets / "comp-r4.commands.json").exists())
        renders = [c for c in tools.calls if os.path.basename(c[0]) == "PedaleiraRender"]
        self.assertEqual(len(renders), 5)  # delay, comp, bypass, retry and the mapped Hall; hard-fail never got past the recorder
        self.assertIn("--run", renders[0])
        self.assertNotIn("--run", renders[1])

        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        self.assertIn("Nothing was saved", report)
        self.assertIn("UNVERIFIED", report)
        self.assertIn("Overall score 75.0", report)
        self.assertIn("No mapping for the Quad Cortex parameter 'TIME'", report)
        self.assertIn("step 3", report)
        self.assertIn("## Failed takes", report)
        self.assertIn("hard-fail", report.split("## Failed takes")[1])
        self.assertIn("bad-model", report.split("## Failed takes")[1])
        self.assertIn("No Such Model", report)
        self.assertIn("take check failed", report)

    def test_existing_recording_is_not_replaced(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE])
        ref = self.root / "session" / "ref"
        ref.mkdir(parents=True)
        (ref / "comp-r4.wav").write_bytes(b"old")
        tools = ToolMock()
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)), tools)
        self.assertEqual(code, qm.EXIT_TAKES_FAILED)
        self.assertEqual(tools.names("record"), [])
        self.assertEqual((ref / "comp-r4.wav").read_bytes(), b"old")
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes", "--overwrite"), fake_deps(fake_kit(FakeUnit())), tools)
        self.assertEqual(code, qm.EXIT_OK, err)

    def test_bypass_take_removes_blocks_and_places_none(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE, {"name": "bypass", "bypass": True}])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_OK, err)
        names = [c[0] for c in unit.calls]
        self.assertEqual(names.count("set_block"), 1)
        self.assertEqual(names.count("remove_block"), 1)
        self.assertEqual(unit.cells, {})

    def test_use_current_recalls_nothing(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes", "--use-current"), fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_OK, err)
        names = [c[0] for c in unit.calls]
        self.assertNotIn("read_preset", names)
        self.assertNotIn("find_preset", names)
        self.assertIn("read_current_preset", names)

    def test_asks_before_recalling(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE])
        asked = []
        deps = fake_deps(fake_kit(unit))
        deps.ask = lambda prompt: asked.append(prompt) or ""
        code, out, err = self.run_main(self.argv(plan, "--go"), deps)
        self.assertEqual(code, qm.EXIT_OK, err)
        self.assertEqual(len(asked), 1)
        self.assertIn("Brit 2203", out)

    def test_stopping_at_the_prompt_touches_nothing(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [COMP_TAKE])
        deps = fake_deps(fake_kit(unit))

        def stop(prompt):
            raise KeyboardInterrupt

        deps.ask = stop
        code, out, err = self.run_main(self.argv(plan, "--go"), deps)
        self.assertEqual(code, qm.EXIT_TAKES_FAILED)
        self.assertEqual([c[0] for c in unit.calls], ["version", "preset_dirty"])


class SignalGainTest(SessionTest):
    def test_plan_default_and_per_take_gain(self):
        plan = {"format": qm.FORMAT, "signalGainDb": -6, "takes": [
            {"name": "a", "model": "Hall"}, {"name": "b", "model": "Hall", "signalGainDb": -12}, {"name": "c", "model": "Hall", "signalGainDb": 0}]}
        self.assertEqual([t.signal_gain_db for t in qm.parse_plan(plan)], [-6.0, -12.0, 0.0])
        self.assertEqual(qm.parse_plan([{"name": "a", "model": "Hall"}])[0].signal_gain_db, 0.0)

    def test_gain_must_be_a_number_from_minus_60_to_0(self):
        for bad in (3, 0.5, -61, "loud", True, float("nan")):
            with self.subTest(bad=bad):
                with self.assertRaises(qm.PlanError) as caught:
                    qm.parse_plan({"takes": [{"name": "a", "model": "Hall", "signalGainDb": bad}]})
                self.assertIn("signalGainDb", str(caught.exception))
        with self.assertRaises(qm.PlanError):
            qm.parse_plan({"signalGainDb": 2, "takes": [{"name": "a", "model": "Hall"}]})

    def test_record_command_carries_the_gain_only_when_set(self):
        self.assertNotIn("--gain-db", qm.record_cmd("PM", "QC", "5,6", "3,4", "t", "/r"))
        cmd = qm.record_cmd("PM", "QC", "5,6", "3,4", "t", "/r", gain_db=-6.0)
        self.assertEqual(cmd[cmd.index("--gain-db") + 1], "-6")

    def test_attenuated_copy_is_scaled_and_never_taken_for_the_signal(self):
        src = self.root / "pedaleira-test-signal-v1.wav"
        qm.write_float_wav(src, 48000, 1, array.array("f", [0.0, 1.0, -0.5, 0.25]))
        copy = Path(qm.attenuated_signal(str(src), -6.0))
        self.assertEqual(copy.name, "pedaleira-test-signal-v1-atten-6dB.wav")
        rate, channels, samples = qm.read_float_wav(copy)
        self.assertEqual((rate, channels), (48000, 1))
        factor = 10 ** (-6.0 / 20)
        for got, want in zip(samples, [0.0, 1.0, -0.5, 0.25]):
            self.assertAlmostEqual(got, want * factor, places=6)
        self.assertEqual(qm.attenuated_signal(str(src), 0.0), str(src))
        dirs = qm.SessionDirs.at(self.root)
        dirs.signal.mkdir(parents=True)
        for path in (src, copy):
            (dirs.signal / path.name).write_bytes(path.read_bytes())
        self.assertEqual(Path(qm.find_signal(dirs)).name, src.name)

    def test_session_plays_and_renders_the_attenuated_signal(self):
        unit = FakeUnit()
        takes = [dict(COMP_TAKE, signalGainDb=-6), {"name": "loud", "model": "Hall", "params": {"MIX": 100}}]
        plan = write_plan(self.root, takes)
        tools = ToolMock(record_output="wrote x\n  peak -7.2 dBFS, both sync markers found, latency 5.00 ms, level -0.1 dB at 1 kHz\n  check passed\n")
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)), tools)
        self.assertEqual(code, qm.EXIT_OK, err)
        records = {c[c.index("--name") + 1]: c for c in tools.names("record")}
        self.assertEqual(records["comp-r4"][records["comp-r4"].index("--gain-db") + 1], "-6")
        self.assertNotIn("--gain-db", records["loud"])
        renders = {c[c.index("--out") + 1].rsplit(os.sep, 1)[-1]: c for c in tools.calls if os.path.basename(c[0]) == "PedaleiraRender"}
        self.assertTrue(renders["comp-r4.wav"][renders["comp-r4.wav"].index("--in") + 1].endswith("pedaleira-test-signal-v1-atten-6dB.wav"))
        self.assertTrue(renders["loud.wav"][renders["loud.wav"].index("--in") + 1].endswith("pedaleira-test-signal-v1.wav"))
        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        self.assertEqual(report.count("Signal level: the test signal was played 6 dB below nominal"), 1)
        self.assertIn("static-curve step is its nominal level minus 6 dB", report)
        self.assertIn("Recording check (PedaleiraMatch record): peak -7.2 dBFS", report)
        self.assertIn("check passed", report)

    def test_dry_run_shows_the_gain(self):
        plan = write_plan(self.root, [dict(COMP_TAKE, signalGainDb=-6)])
        code, out, err = self.run_main(self.argv(plan), fake_deps(fake_kit(FakeUnit())))
        self.assertEqual(code, 0, err)
        self.assertIn("--gain-db -6", out)
        self.assertIn("pedaleira-test-signal-v1-atten-6dB.wav", out)

    def test_a_clipped_take_is_reported_with_a_hint(self):
        plan = write_plan(self.root, [COMP_TAKE, dict(COMP_TAKE, name="quiet", signalGainDb=-6)])
        problem = "  peak 0.0 dBFS\n  PROBLEM: the take clips (it holds full scale, peak 0.0 dBFS): lower the unit's output level\n  check FAILED\n"
        tools = ToolMock(always_fail_record={"comp-r4", "quiet"}, fail_output=problem)
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(FakeUnit())), tools)
        self.assertEqual(code, qm.EXIT_TAKES_FAILED)
        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        failed = report.split("## Failed takes")[1]
        self.assertIn("the take clips", failed)
        row_loud = next(line for line in failed.splitlines() if "comp-r4" in line)
        row_quiet = next(line for line in failed.splitlines() if "quiet" in line)
        self.assertIn("signalGainDb", row_loud)
        self.assertNotIn("hint", row_quiet)


class ReadbackTest(SessionTest):
    def test_report_has_the_table_and_the_read_is_read_only(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [DELAY_TAKE, {"name": "bypass", "bypass": True}])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_OK, err)
        self.assertEqual(unit.saved, [])
        self.assertTrue({c[0] for c in unit.calls} <= qm.ALLOWED_CALLS)
        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        self.assertIn("Parameters read back from the unit", report)
        self.assertIn("| Parameter | Asked | Wire sent | Wire read back |", report)
        self.assertIn("| TIME | 250 ms | 0.12500 | 0.12500 | 250 ms | - | 0 |  |", report)
        self.assertIn("| FEEDBACK | 35 % | 0.35000 | 0.35000 | 35 % | - | 0 |  |", report)
        self.assertEqual(report.count("Parameters read back"), 1)  # not for the bypass take

    def test_a_value_the_unit_holds_differently_is_flagged(self):
        unit = FakeUnit()
        unit.overrides[(3, 0, 0)] = 0.25  # TIME 250 ms of 2000 sent as 0.125; the unit holds 0.25
        plan = write_plan(self.root, [DELAY_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes", "--row", "4"), fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_OK, err)
        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        self.assertIn("| TIME | 250 ms | 0.12500 | 0.25000 | 500 ms | - | 250 | the unit holds 0.25000, not the 0.12500 that was sent |", report)

    def test_no_readback_skips_it(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [DELAY_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes", "--no-readback"), fake_deps(fake_kit(unit)))
        self.assertEqual(code, qm.EXIT_OK, err)
        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        self.assertNotIn("read back", report)
        self.assertFalse(qm.options_from(qm.parse_args(["--plan", "p.json", "--no-readback"])).readback)
        self.assertTrue(qm.options_from(qm.parse_args(["--plan", "p.json"])).readback)

    def test_one_read_of_the_grid_serves_the_setup_check_and_the_readback(self):
        unit = FakeUnit()
        plan = write_plan(self.root, [DELAY_TAKE])
        self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(fake_kit(unit)))
        self.assertEqual([c[0] for c in unit.calls].count("read_current_preset"), 1)

    def test_curve_and_option_conversion(self):
        spec = FakeSpec(0, "DELAY TIME", 7.0, 6000.0, "ms")
        exponent = 5.0 / 3.0
        setting = qm.plan_setting(spec, 250, exponent)
        row = qm.readback_row(setting, spec, setting.wire, exponent)
        self.assertEqual(row.note, "")
        self.assertEqual(row.curve, "250 ms")
        self.assertNotEqual(row.linear, row.curve)  # linear conversion would show 7 + 5993 * 0.14614
        self.assertEqual(row.difference, "0")
        # The unit holds a value that is not what was sent (a unit that played 500 ms for 300 asked).
        held = qm.readback_row(qm.plan_setting(spec, 300, exponent), spec, qm.plan_setting(spec, 500, exponent).wire, exponent)
        self.assertIn("not the", held.note)
        self.assertEqual(held.difference, "200")
        options = FakeSpec(3, "TYPE", options=3)
        listed = qm.readback_row(qm.plan_setting(options, 2), options, 1.0, None)
        self.assertEqual(listed.linear, "option 2 of 3")
        self.assertEqual(qm.readback_row(qm.plan_setting(spec, 250), spec, None, None).note, "the unit holds no value for it")

    def test_a_missing_helper_or_block_is_a_note_not_a_failure(self):
        unit = FakeUnit()
        kit = fake_kit(unit)._replace(param_state=None)
        plan = write_plan(self.root, [DELAY_TAKE])
        code, out, err = self.run_main(self.argv(plan, "--go", "--yes"), fake_deps(kit))
        self.assertEqual(code, qm.EXIT_OK, err)
        self.assertIn("param_state", (self.root / "session" / "report.md").read_text(encoding="utf-8"))


class ScratchEditsTest(SessionTest):
    def run_dirty(self, unit, *extra):
        plan = write_plan(self.root, [COMP_TAKE])
        return self.run_main(self.argv(plan, "--go", "--yes", *extra), fake_deps(fake_kit(unit)))

    def test_leftover_edits_on_the_scratch_are_cleared_through_another_preset(self):
        unit = FakeUnit(dirty=True, current_name="Brit 2203", loaded=7)
        code, out, err = self.run_dirty(unit)
        self.assertEqual(code, qm.EXIT_OK, err)
        self.assertEqual(unit.saved, [])
        recalls = [kw["position"] for name, kw in unit.calls if name == "read_preset"]
        self.assertEqual(recalls[:2], [0, 7])  # the first factory preset that is not the scratch, then the scratch
        names = [c[0] for c in unit.calls]
        self.assertLess(names.index("read_preset"), names.index("set_block"))
        self.assertGreater(names.count("preset_dirty"), 1)  # re-checked after the detour
        self.assertIn("leftovers of an earlier session", out)
        self.assertIn("recall 0 of the", out)
        report = (self.root / "session" / "report.md").read_text(encoding="utf-8")
        self.assertIn("Recalls made on the unit", report)
        self.assertIn("1. recall 0 of", report)
        self.assertIn("routing matches what the scratch preset stores", report)
        self.assertIn("the scratch preset again, which drops the session's edits", report)

    def test_another_preset_with_edits_still_refuses(self):
        unit = FakeUnit(dirty=True, current_name="My Own Preset", loaded=3)
        code, out, err = self.run_dirty(unit)
        self.assertEqual(code, qm.EXIT_DIRTY)
        self.assertIn("unsaved edits", err)
        self.assertNotIn("read_preset", [c[0] for c in unit.calls])
        self.assertEqual(unit.saved, [])

    def test_edits_that_stay_after_the_detour_refuse(self):
        unit = FakeUnit(dirty=True, current_name="Brit 2203", loaded=7, stubborn=True)
        code, out, err = self.run_dirty(unit)
        self.assertEqual(code, qm.EXIT_DIRTY)
        self.assertIn("still reports unsaved edits", err)
        self.assertNotIn("set_block", [c[0] for c in unit.calls])
        self.assertEqual(unit.saved, [])

    def test_use_current_never_clears_edits(self):
        unit = FakeUnit(dirty=True, current_name="Brit 2203", loaded=7)
        code, out, err = self.run_dirty(unit, "--use-current")
        self.assertEqual(code, qm.EXIT_DIRTY)
        self.assertNotIn("read_preset", [c[0] for c in unit.calls])


class PrepareTakeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mapping = json.loads(MAPPING_PATH.read_text(encoding="utf-8"))
        cls.catalog = fake_catalog()

    def prepare(self, raw):
        return qm.prepare_take(qm.parse_plan([raw])[0], self.catalog, self.mapping)

    def test_resolves_names_and_ids(self):
        item = self.prepare({"name": "t", "model": "digital delay", "params": {"time": 250}})
        self.assertEqual(item.model.id, 6005)
        self.assertEqual(item.settings[0].kwargs, {"real": 250.0})
        self.assertEqual(self.prepare({"name": "t", "model": 5005}).model.name, "VCA Comp")

    def test_a_shared_name_means_the_current_model(self):
        self.assertEqual(self.prepare({"name": "t", "model": "phaser"}).model.id, 9102)
        self.assertEqual(self.prepare({"name": "t", "model": 9101}).model.id, 9101)

    def test_problems_become_errors_not_exceptions(self):
        self.assertIn("no model named", self.prepare({"name": "t", "model": "Nope"}).error)
        self.assertIn("several models", self.prepare({"name": "t", "model": "Twin"}).error)
        self.assertIn("no parameter", self.prepare({"name": "t", "model": "Hall", "params": {"WIDTH": 1}}).error)
        self.assertIn("placeholder", self.prepare({"name": "t", "model": "Digital Delay", "params": {"LEVEL": 0.5}}).error)
        self.assertIn("options", self.prepare({"name": "t", "model": "Digital Delay", "params": {"TYPE": 5}}).error)

    def test_list_wire_and_range_notes(self):
        item = self.prepare({"name": "t", "model": "Digital Delay", "params": {"TYPE": 2, "LEVEL": {"wire": 0.7}, "TIME": 5000}})
        kinds = {s.name: s.kwargs for s in item.settings}
        self.assertEqual(kinds["TYPE"], {"value": 1.0})
        self.assertEqual(kinds["LEVEL"], {"value": 0.7})
        self.assertEqual(kinds["TIME"], {"real": 5000.0})
        self.assertTrue(any("clamps" in n for n in item.unit_notes))

    def test_unit_mismatch_is_noted(self):
        mapping = {"models": {"Hall": {"kind": "reverb.hall", "verified": False, "params": {"PRE DELAY": {"param": "preDelayMs", "unit": "s", "transform": "linear"}}}}}
        item = qm.prepare_take(qm.parse_plan([{"name": "t", "model": "Hall", "params": {"PRE DELAY": 20}}])[0], self.catalog, mapping)
        self.assertTrue(any("mapping was written for 's'" in n for n in item.unit_notes))


# ####################
# CATALOGUE AND MAPPING CHECK
# ####################


class CatalogTest(unittest.TestCase):
    def test_catalog_document(self):
        doc = qm.catalog_to_doc(fake_catalog(), "CorOS 4.0.1")
        self.assertEqual(doc["format"], qm.CATALOG_FORMAT)
        self.assertEqual(json.loads(json.dumps(doc, allow_nan=False)), doc)
        delay = next(m for m in doc["models"] if m["id"] == 6005)
        self.assertEqual(delay["parameters"][0], {"index": 0, "name": "TIME", "min": 0.0, "max": 2000.0, "default": 0.0, "units": "ms",
                                                  "type": "", "steps": None, "placeholderRange": False})
        self.assertTrue(delay["parameters"][4]["placeholderRange"])

    def test_check_mapping(self):
        doc = qm.catalog_to_doc(fake_catalog(), "CorOS 4.0.1")
        mapping = {
            "categories": {"Delay": "delay.digital", "Wah": "filter.wah"},
            "models": {
                "Digital Delay": {"kind": "delay.digital", "verified": False, "params": {
                    "FEEDBACK": {"param": "feedback", "unit": "%", "transform": "linear"},
                    "MIX": {"param": "mix", "unit": "dB", "transform": {"scale": 0.01, "offset": 0}},
                    "SPEED": {"param": "rate", "unit": "Hz", "transform": "linear"}}},
                "Digtal Delay": {"kind": "delay.digital", "verified": False},
                "Hall": {"kind": "reverb.hall", "verified": False, "params": {"LEVEL": {"param": "x", "unit": "dB", "transform": "linear"}}},
            },
        }
        result = qm.check_mapping(doc, mapping)
        self.assertEqual(result["summary"]["modelsMapped"], 3)
        self.assertEqual(result["summary"]["modelsFound"], 2)
        delay = result["models"][0]
        params = {p["name"]: p for p in delay["params"]}
        self.assertTrue(params["FEEDBACK"]["found"])
        self.assertEqual(params["FEEDBACK"]["issues"], [])
        self.assertTrue(any("unit mismatch" in i for i in params["MIX"]["issues"]))
        self.assertEqual(params["MIX"]["transformedRange"], [0.0, 1.0])
        self.assertFalse(params["SPEED"]["found"])
        self.assertIn("TIME", params["SPEED"]["available"])
        self.assertIn("TIME", [p["name"] for p in delay["notMappedOnUnit"]])
        self.assertIn("Digital Delay", result["models"][1]["closeNames"])
        self.assertEqual(result["categories"]["mappedButNotOnUnit"], ["Wah"])
        self.assertIn("Compressor", result["categories"]["onUnitButNotInMapping"])
        text = qm.render_mapping_check(result)
        self.assertIn("NOT ON THE UNIT", text)
        self.assertIn("unit mismatch", text)

    def test_mono_and_stereo_models_are_one_mapping_entry(self):
        # A real unit names many blocks "Digital Delay (M)" and "Digital Delay (ST)"; the
        # importer looks both up as "Digital Delay", and so does the check.
        self.assertEqual(qm.model_key("Digital Delay (M)"), qm.model_key("Digital Delay"))
        self.assertEqual(qm.model_key("VCA Comp (S/C)"), qm.model_key("vca comp"))
        self.assertNotEqual(qm.model_key("Plate (Lush)"), qm.model_key("Plate"))
        models = [
            fake_model(6005, "Digital Delay (M)", "Delay", FakeSpec(0, "FEEDBACK", 0.0, 100.0, "%")),
            fake_model(6011, "Digital Delay (ST)", "Delay", FakeSpec(0, "FEEDBACK", 0.0, 100.0, "%")),
        ]
        doc = qm.catalog_to_doc(NS(models={m.id: m for m in models}), "CorOS d14e")
        mapping = {"models": {"Digital Delay": {"kind": "delay.digital", "verified": False, "params": {
            "FEEDBACK": {"param": "feedback", "unit": "%", "transform": "linear"}}}}}
        result = qm.check_mapping(doc, mapping)
        self.assertEqual(result["summary"]["modelsFound"], 1)
        self.assertEqual([m["id"] for m in result["models"][0]["matches"]], [6005, 6011])
        self.assertEqual(result["models"][0]["params"][0]["foundOn"], [6005, 6011])
        self.assertIs(qm.mapping_entry(mapping, "Digital Delay (ST)"), mapping["models"]["Digital Delay"])

    def test_real_mapping_runs_and_is_not_modified(self):
        before = hashlib.sha256(MAPPING_PATH.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory() as temp:
            unit = FakeUnit()
            deps = fake_deps(fake_kit(unit))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(qm.main(["--catalog", "--out", temp], deps), 0)
                self.assertTrue((Path(temp) / "qc-catalog.json").is_file())
                self.assertEqual(qm.main(["--check-mapping", "--out", temp], deps), 0)
                self.assertEqual(qm.main(["--check-mapping", "--out", temp, "--catalog-file", str(Path(temp) / "qc-catalog.json")], deps), 0)
            report = (Path(temp) / "mapping-check.md").read_text(encoding="utf-8")
            self.assertIn("# Quad Cortex mapping check", report)
            self.assertIn("Digital Delay: found", report)
            self.assertIn("VCA Comp: found", report)
            self.assertIn("Spring: NOT ON THE UNIT", report)
            # Read only: the catalogue modes used reads only, and never touched the grid.
            self.assertTrue({c[0] for c in unit.calls} <= {"version"})
            self.assertEqual(unit.saved, [])
            self.assertTrue(unit.closed)
        self.assertEqual(hashlib.sha256(MAPPING_PATH.read_bytes()).hexdigest(), before)

    def test_bad_catalog_file(self):
        with tempfile.TemporaryDirectory() as temp:
            bad = Path(temp) / "x.json"
            bad.write_text("{}", encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(qm.main(["--check-mapping", "--out", temp, "--catalog-file", str(bad)], fake_deps(fake_kit(FakeUnit()))), qm.EXIT_USAGE)
            self.assertIn("not a catalogue file", err.getvalue())


if __name__ == "__main__":
    unittest.main()
