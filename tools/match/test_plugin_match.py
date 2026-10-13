#!/usr/bin/env python3
"""Tests for plugin_match: plan parsing, sweep expansion, safety checks, command lines and the session
with the tools mocked. No plugin and no PedaleiraMatch / PedaleiraRender binary is needed.

Run:  python3 -m unittest tools/match/test_plugin_match.py   (or: python3 tools/match/test_plugin_match.py)
"""

import ast
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import plugin_match as pm  # noqa: E402
import qc_match as qm  # noqa: E402

PLUGIN = "/Library/Audio/Plug-Ins/VST3/Some Amp.vst3"


def plan(takes, **shared):
    document = {"format": pm.FORMAT, "version": 1, "plugin": {"path": PLUGIN}, "takes": takes}
    document.update(shared)
    return document


def args_of(take):
    return [a for s in take.settings for a in s.args()]


class ParseSettings(unittest.TestCase):
    def test_every_form(self):
        takes = pm.parse_plan(plan([{
            "name": "t",
            "plugin": {"params": {
                "Gain": 0.5,
                "Tone": {"text": "3 kHz"},
                "cab": {"index": 7, "value": 1},
                "Mute": False,
                "Mode": "Lead",
            }},
        }]))
        self.assertEqual(len(takes), 1)
        self.assertEqual(
            args_of(takes[0]),
            ["--set", "Gain=0.5", "--set-text", "Tone=3 kHz", "--set-index", "7=1", "--set", "Mute=0", "--set-text", "Mode=Lead"],
        )
        self.assertEqual(takes[0].plugin, PLUGIN)
        self.assertIsNone(takes[0].ours_kind)

    def test_take_overrides_the_shared_plugin_block(self):
        document = plan(
            [{"name": "a", "plugin": {"params": {"gain": 0.9, "Level": 0.1}}}, {"name": "b"}],
            plugin={"path": PLUGIN, "state": "/s/amp.vstpreset", "params": {"Gain": 0.2, "Tone": 0.5}},
        )
        a, b = pm.parse_plan(document)
        # The same parameter by another spelling replaces the shared one in place.
        self.assertEqual(args_of(a), ["--set", "gain=0.9", "--set", "Tone=0.5", "--set", "Level=0.1"])
        self.assertEqual(args_of(b), ["--set", "Gain=0.2", "--set", "Tone=0.5"])
        self.assertEqual(a.state, "/s/amp.vstpreset")
        # A take can drop the state or use another plugin.
        c, = pm.parse_plan(plan([{"name": "c", "plugin": {"path": "/x/Other.vst3", "state": None}}],
                                plugin={"path": PLUGIN, "state": "/s/amp.vstpreset"}))
        self.assertEqual((c.plugin, c.state), ("/x/Other.vst3", None))

    def test_problems_are_listed_together(self):
        with self.assertRaises(qm.PlanError) as caught:
            pm.parse_plan(plan([
                {"name": "bad name"},
                {"name": "range", "plugin": {"params": {"Gain": 1.5}}},
                {"name": "eq", "plugin": {"params": {"A=B": 0.5}}},
                {"name": "idx", "plugin": {"params": {"x": {"index": 2, "text": "1"}}}},
                {"name": "extra", "colour": "red"},
                {"name": "ours", "ours": {"params": {"gain": 1}}},
            ]))
        message = str(caught.exception)
        for fragment in ("bad name", "normalised value is 0 to 1", "'='", "index entry", "unknown field", "no kind"):
            self.assertIn(fragment, message)

    def test_needs_a_plugin_path_and_takes(self):
        with self.assertRaises(qm.PlanError):
            pm.parse_plan({"takes": [{"name": "t"}]})
        with self.assertRaises(qm.PlanError):
            pm.parse_plan({"takes": []})
        with self.assertRaises(qm.PlanError):
            pm.parse_plan({"format": "something-else", "takes": [{"name": "t"}]})

    def test_duplicate_names(self):
        with self.assertRaises(qm.PlanError) as caught:
            pm.parse_plan(plan([{"name": "a"}, {"name": "A"}]))
        self.assertIn("used twice", str(caught.exception))


class SweepExpansion(unittest.TestCase):
    def test_one_take_per_value(self):
        takes = pm.parse_plan(plan([{
            "name": "gain-sweep",
            "plugin": {"params": {"Tone": 0.5}},
            "sweep": {"param": "Gain", "values": [0, 0.25, 0.5, 1]},
        }]))
        self.assertEqual([t.name for t in takes], ["gain-sweep-01-0", "gain-sweep-02-0p25", "gain-sweep-03-0p5", "gain-sweep-04-1"])
        self.assertEqual(args_of(takes[1]), ["--set", "Tone=0.5", "--set", "Gain=0.25"])
        self.assertEqual({t.sweep_group for t in takes}, {"gain-sweep"})
        self.assertEqual(takes[2].sweep_label, "Gain = 0.5")

    def test_sweep_replaces_the_same_parameter(self):
        takes = pm.parse_plan(plan([{
            "name": "s", "plugin": {"params": {"gain": 0.9, "Tone": 0.5}},
            "sweep": {"param": "Gain", "values": [0.1, 0.2]},
        }]))
        self.assertEqual(args_of(takes[0]), ["--set", "Gain=0.1", "--set", "Tone=0.5"])

    def test_both_sides_sweep(self):
        takes = pm.parse_plan(plan([{
            "name": "drive",
            "sweep": {"param": "Drive", "values": [0, 0.5, 1]},
            "ours": {"kind": "amp.test", "params": {"bass": 5}, "mix": 1,
                     "sweep": {"param": "gain", "values": [1, 4, 8]}},
        }]))
        self.assertEqual([t.ours_params for t in takes], [
            {"bass": 5.0, "mix": 1.0, "gain": 1.0},
            {"bass": 5.0, "mix": 1.0, "gain": 4.0},
            {"bass": 5.0, "mix": 1.0, "gain": 8.0},
        ])
        self.assertEqual({t.ours_kind for t in takes}, {"amp.test"})

    def test_only_the_reference_sweeps_without_ours_sweep(self):
        takes = pm.parse_plan(plan([{
            "name": "d", "sweep": {"param": "Drive", "values": [0, 1]},
            "ours": {"kind": "amp.test", "params": {"gain": 3}},
        }]))
        self.assertEqual([t.ours_params for t in takes], [{"gain": 3.0}, {"gain": 3.0}])

    def test_text_and_row_sweeps(self):
        takes = pm.parse_plan(plan([
            {"name": "txt", "sweep": {"param": "Cutoff", "values": [{"text": "100 Hz"}, "1 kHz"]}},
            {"name": "row", "sweep": {"param": 3, "values": [0, 1]}},
        ]))
        self.assertEqual([args_of(t) for t in takes], [
            ["--set-text", "Cutoff=100 Hz"], ["--set-text", "Cutoff=1 kHz"],
            ["--set-index", "3=0"], ["--set-index", "3=1"],
        ])
        self.assertEqual(len({t.name for t in takes}), 4)

    def test_bad_sweeps(self):
        for take, fragment in (
            ({"name": "a", "sweep": {"param": "G", "values": []}}, "non-empty"),
            ({"name": "a", "sweep": {"param": "G", "values": [2]}}, "normalised value"),
            ({"name": "a", "sweep": {"param": "", "values": [0]}}, "parameter name"),
            ({"name": "a", "sweep": {"param": 1, "values": ["x"]}}, "by row"),
            ({"name": "a", "sweep": {"param": "G", "values": [0, 1]}, "ours": {"kind": "k", "sweep": {"param": "g", "values": [1]}}}, "2"),
            ({"name": "a", "ours": {"kind": "k", "sweep": {"param": "g", "values": [1]}}}, "needs the take to sweep"),
        ):
            with self.subTest(fragment=fragment):
                with self.assertRaises(qm.PlanError) as caught:
                    pm.parse_plan(plan([take]))
                self.assertIn(fragment, str(caught.exception))


class Safety(unittest.TestCase):
    def test_output_inside_the_repository_is_refused(self):
        with self.assertRaises(qm.PlanError):
            pm.check_output_dir(pm.REPO / "match")
        with self.assertRaises(qm.PlanError):
            pm.check_output_dir(pm.REPO / "docs" / "nested" / "dir")
        with tempfile.TemporaryDirectory() as tmp:
            pm.check_output_dir(Path(tmp) / "match")  # outside: fine

    def test_another_checkout_is_recognised_by_its_markers(self):
        with tempfile.TemporaryDirectory() as tmp:
            checkout = Path(tmp) / "other-checkout"
            (checkout / ".git").mkdir(parents=True)
            (checkout / "plugin" / "src").mkdir(parents=True)
            (checkout / "plugin" / "src" / "EffectMatch.cpp").write_text("//")
            self.assertTrue(pm.inside_repo(checkout / "match" / "ref"))
            self.assertFalse(pm.inside_repo(Path(tmp) / "elsewhere"))

    def test_plugin_and_state_inside_the_repository(self):
        with self.assertRaises(qm.PlanError):
            pm.check_plugin_files(str(pm.REPO / "third-party" / "Amp.vst3"), None)
        with self.assertRaises(qm.PlanError):
            pm.check_plugin_files(PLUGIN, str(pm.REPO / "amp.vstpreset"))
        # The repository's own test plugin is the one exception, wherever the build tree puts it.
        pm.check_plugin_files(str(pm.REPO / "build" / "test" / "PedaleiraTestFx_artefacts" / "VST3" / "PedaleiraTestFx.vst3"), None)
        pm.check_plugin_files("AudioUnit:Effects/aufx,amp ,acme", None)
        pm.check_plugin_files(PLUGIN, "/Users/me/states/amp.vstpreset")

    def test_main_refuses_a_dir_in_the_repo_and_plugins_in_the_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan_path = Path(tmp) / "plan.json"
            plan_path.write_text(json.dumps(plan([{"name": "t"}])))
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                code = pm.main(["--plan", str(plan_path), "--dir", str(pm.REPO / "match"), "--go"])
            self.assertEqual(code, qm.EXIT_USAGE)
            self.assertIn("inside the repository", err.getvalue())
            self.assertFalse((pm.REPO / "match").exists())

            inside = plan([{"name": "t"}], plugin={"path": str(pm.REPO / "x" / "Amp.vst3")})
            plan_path.write_text(json.dumps(inside))
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                code = pm.main(["--plan", str(plan_path), "--dir", str(Path(tmp) / "out")])
            self.assertEqual(code, qm.EXIT_USAGE)
            self.assertIn("inside the repository", err.getvalue())

    def test_no_network_in_the_modules(self):
        banned = {"socket", "urllib", "http", "requests", "ftplib", "smtplib", "ssl", "asyncio", "websocket", "websockets", "aiohttp"}
        for name in ("plugin_match.py", "qc_match.py"):
            tree = ast.parse((Path(__file__).parent / name).read_text(encoding="utf-8"))
            imported = set()
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    imported |= {a.name.split(".")[0] for a in node.names}
                elif isinstance(node, ast.ImportFrom) and node.module:
                    imported.add(node.module.split(".")[0])
            self.assertFalse(imported & banned, f"{name} imports {imported & banned}")


class CommandLines(unittest.TestCase):
    def test_plugin_render_command(self):
        take = pm.parse_plan(plan([{
            "name": "t", "plugin": {"state": "~/states/amp.vstpreset", "params": {"Gain": 0.5, "Tone": {"text": "3 kHz"}}},
        }]))[0]
        cmd = pm.plugin_render_cmd("PM", take, "sig.wav", "ref/t.wav", 4.0, 256, 1)
        home = os.path.expanduser("~")
        self.assertEqual(cmd, [
            "PM", "plugin-render", "--plugin", PLUGIN, "--state", f"{home}/states/amp.vstpreset",
            "--set", "Gain=0.5", "--set-text", "Tone=3 kHz",
            "--in", "sig.wav", "--out", "ref/t.wav", "--tail", "4", "--block", "256", "--channels", "1",
        ])
        self.assertNotIn("--channels", pm.plugin_render_cmd("PM", take, "s", "o", 4.0, 512, None))


def fake_tools(calls, channels=2, fail_stage=None):
    """A run_tool that writes the files each real tool would write."""

    def run(stage, argv, timeout=600.0):
        calls.append((stage, list(argv)))
        if stage == fail_stage:
            raise qm.ToolFailed(stage, argv, 1, "boom")
        if stage == "signals":
            out = Path(argv[argv.index("--out") + 1])
            out.mkdir(parents=True, exist_ok=True)
            (out / "pedaleira-test-signal-v1.wav").write_bytes(b"RIFF")
        elif stage == "reference":
            wav = Path(argv[argv.index("--out") + 1])
            wav.parent.mkdir(parents=True, exist_ok=True)
            wav.write_bytes(b"RIFF")
            wav.with_suffix(".json").write_text(json.dumps({
                "plugin": {"name": "Some Amp", "version": "1.2.3", "format": "VST3"},
                "latencySamples": 64, "channels": channels, "inputChannels": 2, "outputChannels": 2,
                "parameters": [{"name": "Gain", "requested": {"name": "Gain", "value": 0.5}, "value": 0.5, "text": "-6.0 dB"}],
            }))
        elif stage in ("render ours",):
            Path(argv[argv.index("--out") + 1]).parent.mkdir(parents=True, exist_ok=True)
            Path(argv[argv.index("--out") + 1]).write_bytes(b"RIFF")
        elif stage == "compare":
            Path(argv[argv.index("--out") + 1]).write_text(json.dumps({
                "score": 80.0, "judged": 10, "passed": 8,
                "metrics": [{"group": "tone", "name": "x", "judged": True, "pass": False, "reference": 1, "candidate": 2,
                             "difference": 1, "allowed": 0.5}] + [{"group": "tone", "name": f"y{i}", "judged": True, "pass": True} for i in range(9)],
            }))
        return ""

    return run


class Session(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.plugin = self.root / "Amp.vst3"
        self.plugin.mkdir()
        self.match_tool = self.root / "PedaleiraMatch"
        self.render_tool = self.root / "PedaleiraRender"
        for tool in (self.match_tool, self.render_tool):
            tool.write_text("#!/bin/sh\n")

    def options(self, **kwargs):
        defaults = dict(out_dir=self.root / "out", match_tool=str(self.match_tool), render_tool=str(self.render_tool))
        defaults.update(kwargs)
        return pm.Options(**defaults)

    def takes(self):
        return pm.parse_plan(plan([
            {"name": "sweep", "sweep": {"param": "Gain", "values": [0.25, 0.5]},
             "ours": {"kind": "amp.test", "params": {"gain": 2}, "mix": 1, "sweep": {"param": "gain", "values": [2, 4]}}},
            {"name": "ref-only"},
        ], plugin={"path": str(self.plugin)}))

    def test_session_runs_every_stage_in_order(self):
        calls = []
        takes = self.takes()
        with mock.patch.object(pm, "run_tool", fake_tools(calls)), contextlib.redirect_stdout(io.StringIO()):
            code = pm.run_session(self.options(), takes)
        self.assertEqual(code, qm.EXIT_OK)
        stages = [stage for stage, _ in calls]
        self.assertEqual(stages[0], "signals")
        self.assertEqual(stages[1:6], ["reference", "analyse reference", "render ours", "analyse ours", "compare"])
        self.assertEqual(stages.count("reference"), 3)
        self.assertEqual(stages.count("render ours"), 2)  # the reference-only take has no kind
        # Each reference command carries that take's value, in the right file.
        refs = [argv for stage, argv in calls if stage == "reference"]
        self.assertIn("Gain=0.25", refs[0])
        self.assertIn("Gain=0.5", refs[1])
        self.assertTrue(refs[0][refs[0].index("--out") + 1].endswith(os.path.join("ref", "sweep-01-0p25.wav")))
        # Ours takes the matching value and as many channels as the reference.
        presets = sorted((self.root / "out" / "presets").glob("sweep-0*.json"))
        values = [json.loads(p.read_text())["chain"]["children"][0]["children"][0]["children"][0]["children"][0]["value"] for p in presets if "commands" not in p.name]
        self.assertEqual(values, [2.0, 4.0])
        renders = [argv for stage, argv in calls if stage == "render ours"]
        self.assertEqual(renders[0][renders[0].index("--out-channels") + 1], "2")
        report = (self.root / "out" / "report.md").read_text()
        for fragment in ("# Plugin matching session", "Some Amp 1.2.3", "-6.0 dB", "Latency: 64 samples, compensated",
                         "sweep-02-0p5", "Overall score 80.0", "Metrics out of tolerance", "recorded", "stay in your data folder"):
            self.assertIn(fragment, report)

    def test_mono_reference_makes_ours_mono(self):
        calls = []
        with mock.patch.object(pm, "run_tool", fake_tools(calls, channels=1)), contextlib.redirect_stdout(io.StringIO()):
            pm.run_session(self.options(), self.takes())
        renders = [argv for stage, argv in calls if stage == "render ours"]
        self.assertEqual(renders[0][renders[0].index("--out-channels") + 1], "1")

    def test_a_failed_take_is_reported_and_the_session_goes_on(self):
        calls = []
        with mock.patch.object(pm, "run_tool", fake_tools(calls, fail_stage="compare")), contextlib.redirect_stdout(io.StringIO()):
            code = pm.run_session(self.options(), self.takes())
        self.assertEqual(code, qm.EXIT_TAKES_FAILED)
        report = (self.root / "out" / "report.md").read_text()
        self.assertIn("Failed takes", report)
        self.assertIn("compare", report)
        self.assertEqual([stage for stage, _ in calls].count("reference"), 3)

    def test_an_existing_reference_is_not_replaced(self):
        calls = []
        ref = self.root / "out" / "ref"
        ref.mkdir(parents=True)
        (ref / "ref-only.wav").write_bytes(b"old")
        with mock.patch.object(pm, "run_tool", fake_tools(calls)), contextlib.redirect_stdout(io.StringIO()):
            code = pm.run_session(self.options(), self.takes())
        self.assertEqual(code, qm.EXIT_TAKES_FAILED)
        self.assertEqual((ref / "ref-only.wav").read_bytes(), b"old")
        with mock.patch.object(pm, "run_tool", fake_tools([])), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(pm.run_session(self.options(overwrite=True), self.takes()), qm.EXIT_OK)

    def test_missing_plugin_or_tool_stops_before_anything_runs(self):
        takes = pm.parse_plan(plan([{"name": "t"}], plugin={"path": str(self.root / "Missing.vst3")}))
        calls = []
        with mock.patch.object(pm, "run_tool", fake_tools(calls)):
            with self.assertRaises(qm.PlanError):
                pm.run_session(self.options(), takes)
            with self.assertRaises(qm.PlanError):
                pm.run_session(self.options(match_tool=str(self.root / "nope")), self.takes())
        self.assertEqual(calls, [])

    def test_dry_run_writes_nothing_and_runs_nothing(self):
        plan_path = self.root / "plan.json"
        plan_path.write_text(json.dumps(plan([
            {"name": "g", "sweep": {"param": "Gain", "values": [0, 1]}, "ours": {"kind": "amp.test", "mix": 1}},
        ], plugin={"path": str(self.plugin)})))
        out = io.StringIO()
        with mock.patch.object(pm, "run_tool", side_effect=AssertionError("ran a tool")), \
                mock.patch("subprocess.run", side_effect=AssertionError("ran a process")), contextlib.redirect_stdout(out):
            code = pm.main(["--plan", str(plan_path), "--dir", str(self.root / "out")])
        self.assertEqual(code, qm.EXIT_OK)
        text = out.getvalue()
        for fragment in ("DRY RUN", "plugin-render", "--set Gain=0", "g-02-1", "render ours", "compare"):
            self.assertIn(fragment, text)
        self.assertFalse((self.root / "out").exists())

    def test_go_needs_a_dir(self):
        plan_path = self.root / "plan.json"
        plan_path.write_text(json.dumps(plan([{"name": "g"}], plugin={"path": str(self.plugin)})))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(pm.main(["--plan", str(plan_path), "--go"]), qm.EXIT_USAGE)
        self.assertIn("--dir", err.getvalue())

    def test_the_example_plan_parses(self):
        example = Path(__file__).parent / "example-plugin-plan.json"
        takes = pm.parse_plan(json.loads(example.read_text(encoding="utf-8")))
        self.assertGreaterEqual(len(takes), 5)
        self.assertTrue(all(t.ours_kind for t in takes))


if __name__ == "__main__":
    unittest.main()
