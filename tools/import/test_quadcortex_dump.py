#!/usr/bin/env python3
"""Tests for quadcortex_dump.build_foreign_preset with fake pyquadcortex objects.

No unit and no pyquadcortex needed. Run:  python3 tools/import/test_quadcortex_dump.py
"""

import os
import sys
import unittest
from types import SimpleNamespace as NS

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quadcortex_dump as qd  # noqa: E402


class FakeMessage(NS):
    """A protobuf-like message: HasField is true for fields that were given."""

    def HasField(self, field):
        if field not in self.__dict__:
            return False
        return True


class FakeSpec:
    """A catalog Parameter: a linear range, an optional list of options."""

    def __init__(self, index, name, low=0.0, high=10.0, units="", options=(), hidden=False):
        self.index = index
        self.name = name
        self.minimum, self.maximum = low, high
        self.units = units
        self.options = tuple(options)
        self.hidden = hidden

    @property
    def option_count(self):
        return len(self.options) if self.options else None

    def value_to_option(self, wire):
        return 0 if len(self.options) == 1 else round(wire * (len(self.options) - 1))

    def to_real(self, wire):
        if not 0.0 <= wire <= 1.0:
            raise ValueError("outside the wire range")
        return self.minimum + (self.maximum - self.minimum) * wire


def model(name, category, *specs):
    return NS(name=name, category=category, parameters=tuple(specs))


class Catalog(dict):
    pass


def param_state(scene_mode, *values):
    return NS(scene_mode=scene_mode, values=tuple(values))


def make_kit(blocks, bypass, params, splits=(), bpm=None):
    return qd.Toolkit(
        blocks=lambda preset: blocks,
        splits=lambda preset: list(splits),
        bypass_state=lambda preset, cell: bypass[(cell.row, cell.column)],
        param_state=lambda preset, cell, index: params[(cell.row, cell.column, index)],
        input_name=lambda port: {1: "INPUT_1", 0: "EMPTY"}.get(port, f"UNKNOWN_{port}"),
        output_name=lambda port: {1: "XLR_1_2", 19: "MULTIPLE"}.get(port, f"UNKNOWN_{port}"),
        tempo_bpm=lambda preset: bpm,
    )


def cell(row, column, model_id):
    return NS(row=row, column=column, model_id=model_id)


def fake_preset(chains, name="Test Preset", labels=(), **extra):
    return FakeMessage(name=name, chains=chains, scene_labels=list(labels), **extra)


def chain(row=None, in_port=None, out_port=None):
    fields = {}
    if row is not None:
        fields["row"] = row
    if in_port is not None:
        fields["in_portid"] = in_port
    if out_port is not None:
        fields["out_portid"] = out_port
    return FakeMessage(**fields)


class BuildForeignPresetTest(unittest.TestCase):
    def setUp(self):
        self.catalog = Catalog(
            {
                1: model("VCA Comp", "Compressor", FakeSpec(0, "THRESHOLD", -60.0, 12.0, "dB"), FakeSpec(1, "RATIO", 1.0, 20.0)),
                2: model("Digital Delay", "Delay", FakeSpec(0, "TIME", 0.0, 1000.0, "ms"), FakeSpec(1, "TYPE", options=("MONO", "PING PONG"))),
                3: model("Neural Capture", "Neural Capture", FakeSpec(0, "file_name"), FakeSpec(1, "LEVEL", -20.0, 20.0, "dB", hidden=True)),
            }
        )

    def basic(self):
        blocks = [cell(0, 0, 1), cell(0, 3, 2), cell(1, 0, 3)]
        bypass = {
            (0, 0): NS(scene_mode=False, scenes=(False,) * 8),
            (0, 3): NS(scene_mode=True, scenes=(False, True, False, False, False, False, False, False)),
            (1, 0): NS(scene_mode=False, scenes=(True,) * 8),
        }
        params = {
            (0, 0, 0): param_state(False, 0.5),  # -24 dB
            (0, 0, 1): param_state(False, float("nan")),  # factory NaN: dropped
            (0, 3, 0): param_state(True, 0.25, 0.5, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25),  # 250 ms, scene B 500 ms
            (0, 3, 1): param_state(False, 1.0),  # option 1
            (1, 0, 0): param_state(False, "My Capture"),
            (1, 0, 1): param_state(False, 0.5),
        }
        chains = [chain(0, 1, 19), chain(1, 0, 1), chain(2), chain(3)]
        preset = fake_preset(chains, labels=["Clean", " ", "", "Lead"], default_scene=0)
        return preset, make_kit(blocks, bypass, params, splits=[NS(row=0, split_column=2, mix_column=6)], bpm=96.0)

    def test_header_and_blocks(self):
        preset, kit = self.basic()
        out = qd.build_foreign_preset(preset, self.catalog, kit, "CorOS 4.0.1")
        self.assertEqual(out["format"], "pedaleira-foreign-preset")
        self.assertEqual(out["version"], 1)
        self.assertEqual(out["source"], "quad-cortex")
        self.assertEqual(out["sourceVersion"], "CorOS 4.0.1")
        self.assertEqual(out["name"], "Test Preset")
        self.assertEqual(out["tempo"], 96.0)
        self.assertEqual([r["index"] for r in out["rows"]], [0, 1])
        first = out["rows"][0]["blocks"][0]
        self.assertEqual(first["model"], "VCA Comp")
        self.assertEqual(first["category"], "Compressor")
        self.assertEqual(first["position"], 0)
        self.assertTrue(first["enabled"])
        self.assertEqual(first["params"]["THRESHOLD"], {"value": -24.0, "unit": "dB"})
        self.assertNotIn("RATIO", first["params"])  # NaN dropped

    def test_options_strings_and_hidden(self):
        preset, kit = self.basic()
        out = qd.build_foreign_preset(preset, self.catalog, kit)
        delay = out["rows"][0]["blocks"][1]
        self.assertEqual(delay["params"]["TYPE"], {"value": "PING PONG"})
        self.assertEqual(delay["params"]["TIME"], {"value": 250.0, "unit": "ms"})
        capture = out["rows"][1]["blocks"][0]
        self.assertFalse(capture["enabled"])
        self.assertEqual(capture["params"]["file_name"], {"value": "My Capture"})
        self.assertNotIn("LEVEL", capture["params"])  # hidden

    def test_scenes(self):
        preset, kit = self.basic()
        out = qd.build_foreign_preset(preset, self.catalog, kit)
        self.assertEqual(len(out["scenes"]), 8)
        self.assertEqual([s["name"] for s in out["scenes"][:4]], ["Clean", "Scene B", "Scene C", "Lead"])
        # Only the block with a per-scene bypass appears in "enabled".
        self.assertEqual(out["scenes"][0]["enabled"], {"0:3": True})
        self.assertEqual(out["scenes"][1]["enabled"], {"0:3": False})
        # Only scene B differs in the delay time.
        self.assertEqual(out["scenes"][1]["params"], {"0:3": {"TIME": 500.0}})
        self.assertEqual(out["scenes"][2]["params"], {})

    def test_routing(self):
        preset, kit = self.basic()
        out = qd.build_foreign_preset(preset, self.catalog, kit)
        rows = out["routing"]["rows"]
        self.assertEqual(
            rows[0],
            {
                "index": 0,
                "input": {"type": "device", "channel": 1, "stereo": False, "name": "INPUT_1"},
                "output": {"type": "other", "name": "MULTIPLE"},
            },
        )
        self.assertEqual(
            rows[1],
            {
                "index": 1,
                "input": {"type": "none", "name": "EMPTY"},
                "output": {"type": "device", "pair": 1, "name": "XLR_1_2"},
            },
        )
        self.assertEqual(rows[2], {"index": 2})
        self.assertEqual(out["routing"]["splits"], [{"row": 0, "splitAt": 2, "mixAt": 6}])

    def test_split_without_rejoin_has_null_mix(self):
        preset, kit = self.basic()
        kit = kit._replace(splits=lambda p: [NS(row=2, split_column=3, mix_column=-1)])
        out = qd.build_foreign_preset(preset, self.catalog, kit)
        self.assertEqual(out["routing"]["splits"], [{"row": 2, "splitAt": 3, "mixAt": None}])

    def test_unknown_model_warns_and_is_skipped(self):
        blocks = [cell(0, 0, 99)]
        kit = make_kit(blocks, {(0, 0): NS(scene_mode=False, scenes=(False,) * 8)}, {})
        out = qd.build_foreign_preset(fake_preset([chain(0)]), self.catalog, kit)
        self.assertEqual(out["rows"], [])
        self.assertEqual(len(out["warnings"]), 1)
        self.assertIn("99", out["warnings"][0])

    def test_no_tempo_no_warnings_keys(self):
        kit = make_kit([], {}, {})
        out = qd.build_foreign_preset(fake_preset([]), self.catalog, kit)
        self.assertNotIn("tempo", out)
        self.assertNotIn("warnings", out)
        self.assertEqual(out["rows"], [])

    def test_output_is_plain_json(self):
        import json

        preset, kit = self.basic()
        out = qd.build_foreign_preset(preset, self.catalog, kit)
        text = json.dumps(out, allow_nan=False)
        self.assertEqual(json.loads(text), out)


class HelpersTest(unittest.TestCase):
    def test_convert_value_rejects_nan_and_out_of_range(self):
        spec = FakeSpec(0, "X", 0.0, 1.0)
        self.assertIsNone(qd.convert_value(spec, float("nan")))
        self.assertIsNone(qd.convert_value(spec, 1.5))
        self.assertIsNone(qd.convert_value(spec, None))
        self.assertEqual(qd.convert_value(spec, 0.5), {"value": 0.5})

    def test_safe_file_name(self):
        self.assertEqual(qd.safe_file_name("28C", "AC/DC: Live?"), "28C AC-DC- Live.json")
        self.assertEqual(qd.safe_file_name("", ""), "preset.json")

    def test_all_needs_yes(self):
        self.assertEqual(qd.main(["--all", "--out", "x"]), 2)


if __name__ == "__main__":
    unittest.main()
