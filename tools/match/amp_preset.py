#!/usr/bin/env python3
"""Writes a JSON preset that holds one amp.custom block with an amp description
and chosen knob settings, so that PedaleiraRender can render the test signal
through it (docs/amp-framework.md, "Measuring an amp or a capture").

    amp_preset.py --description my-amp.json --out preset.json \
                  --set gain=60 --set bass=40 --set voicing=1 [--bright] [--name my-amp]

Knobs are the parameters of the block: gain, bass, middle, treble, presence,
depth, master (0 to 100), levelDb (-48 to 18), bright (0 or 1) and voicing
(0 to 2). The description's text is embedded in the block's state, the way the
app saves it, so the preset does not depend on the file afterwards.

Standard library only.
"""
import argparse
import base64
import json
import sys

KNOBS = {'gain', 'bass', 'middle', 'treble', 'presence', 'depth', 'master', 'levelDb', 'bright', 'voicing'}


def build_preset(description_text, knobs, name='amp'):
    """The preset as a dict. `description_text` must be valid JSON text."""
    json.loads(description_text)  # fail early, with Python's message, on a typo
    params = [{'node': 'Param', 'id': key, 'value': float(value)} for key, value in sorted(knobs.items())]
    chunk = {'$base64': base64.b64encode(description_text.encode('utf-8')).decode('ascii')}
    return {
        'format': 'pedaleira.preset', 'version': 1, 'schemaVersion': 1,
        'name': name, 'id': name,
        'chain': {
            'stereoEnabled': False, 'branchSide': 'left', 'branchAfterBlockId': '',
            'children': [
                {'node': 'ChainBlocks', 'children': [
                    {'node': 'ChainBlock', 'id': 'amp1', 'type': 'kind', 'enabled': True, 'mix': 1.0,
                     'children': [{'node': 'KindState', 'kind': 'amp.custom', 'version': 1, 'name': 'Custom Amp',
                                   'chunk': chunk, 'children': params}]}]},
                {'node': 'RightChainBlocks'},
            ],
        },
        'params': {},
    }


def parse_knobs(settings, bright):
    knobs = {}
    for item in settings:
        key, sep, value = item.partition('=')
        if not sep or key not in KNOBS:
            raise SystemExit('--set expects knob=value with knob one of: ' + ', '.join(sorted(KNOBS)))
        try:
            knobs[key] = float(value)
        except ValueError:
            raise SystemExit('--set %s: %r is not a number' % (key, value))
    if bright:
        knobs['bright'] = 1.0
    return knobs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--description', required=True, help='the amp description file (JSON)')
    parser.add_argument('--out', required=True, help='the preset to write')
    parser.add_argument('--set', action='append', default=[], metavar='KNOB=VALUE')
    parser.add_argument('--bright', action='store_true', help='the same as --set bright=1')
    parser.add_argument('--name', default='amp')
    args = parser.parse_args(argv)
    with open(args.description, encoding='utf-8') as f:
        text = f.read()
    preset = build_preset(text, parse_knobs(args.set, args.bright), args.name)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(preset, f, indent=1)
        f.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
