import base64
import json
import os
import tempfile
import unittest

import amp_preset


class AmpPresetTest(unittest.TestCase):
    def test_the_description_travels_as_the_blocks_chunk(self):
        text = '{"format": "pedaleira-amp", "version": 1}'
        preset = amp_preset.build_preset(text, {'gain': 60, 'bass': 40}, 'x')
        block = preset['chain']['children'][0]['children'][0]
        state = block['children'][0]
        self.assertEqual(state['kind'], 'amp.custom')
        self.assertEqual(base64.b64decode(state['chunk']['$base64']).decode('utf-8'), text)
        self.assertEqual({p['id']: p['value'] for p in state['children']}, {'bass': 40.0, 'gain': 60.0})

    def test_bad_json_is_refused_early(self):
        with self.assertRaises(ValueError):
            amp_preset.build_preset('{', {})

    def test_knobs_are_checked(self):
        self.assertEqual(amp_preset.parse_knobs(['gain=10'], True), {'gain': 10.0, 'bright': 1.0})
        with self.assertRaises(SystemExit):
            amp_preset.parse_knobs(['volume=10'], False)
        with self.assertRaises(SystemExit):
            amp_preset.parse_knobs(['gain=loud'], False)

    def test_command_line_writes_the_file(self):
        with tempfile.TemporaryDirectory() as folder:
            source = os.path.join(folder, 'amp.json')
            target = os.path.join(folder, 'preset.json')
            with open(source, 'w') as f:
                f.write('{"version": 1}')
            self.assertEqual(amp_preset.main(['--description', source, '--out', target, '--set', 'gain=5']), 0)
            with open(target) as f:
                self.assertEqual(json.load(f)['format'], 'pedaleira.preset')


if __name__ == '__main__':
    unittest.main()
