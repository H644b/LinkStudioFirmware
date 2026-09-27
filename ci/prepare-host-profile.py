#!/usr/bin/env python3
"""Package the existing bounded public Z-A identities; never read console root keys."""
import argparse
import base64
import gzip
import io
import json
import os
from pathlib import Path

def prepare(data):
    value = json.loads(data)
    expected = {'format', 'protocol', 'communication_id', 'scene_id', 'app_version', 'profiles'}
    if (not isinstance(value, dict) or set(value) != expected or
            any(type(value[k]) is not int or value[k] != 1 for k in ('format', 'protocol', 'scene_id'))):
        raise ValueError('Unsupported bounded host profile')
    if value['communication_id'] != '0100f43008c44000' or type(value['app_version']) is not int or not 1 <= value['app_version'] <= 65535:
        raise ValueError('Host profile is not for Z-A protocol 1')
    entries = value['profiles']
    if not isinstance(entries, list) or not 1 <= len(entries) <= 256:
        raise ValueError('Expected 1..256 bounded identities')
    names = ('ssid', 'server_random', 'advertise_key', 'data_key')
    rows = []
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != set(names): raise ValueError('Unexpected identity fields')
        row = []
        for name in names:
            raw = bytes.fromhex(entry[name])
            if len(raw) != 16: raise ValueError('Each bounded field must have 16 bytes')
            row.append('{' + ','.join(f'0x{b:02x}' for b in raw) + '}')
        rows.append('    {' + ','.join(row) + '}')
    return ('/* Generated bounded public connection identities; no console root keys. */\n'
            '#pragma once\n#include <stdint.h>\n'
            'typedef struct { uint8_t ssid[16], server_random[16], advertise_key[16], data_key[16]; } LsHostProfile;\n'
            f'#define LS_PROFILE_APP_VERSION {value["app_version"]}\n'
            'static const LsHostProfile ls_host_profiles[] = {\n' + ',\n'.join(rows) + '\n};\n')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    try:
        if a.input:
            if a.input.stat().st_size > 200000: raise ValueError('Profile file too large')
            data = a.input.read_bytes()
        else:
            encoded = os.environ.get('LS_HOST_PROFILES_GZIP_B64', '')
            if not encoded or len(encoded) > 48000: raise ValueError('Bounded CI profile is missing or too large')
            # The release owner supplies this same public build asset as the web app.
            with gzip.GzipFile(fileobj=io.BytesIO(base64.b64decode(encoded, validate=True))) as stream:
                data = stream.read(200001)
            if len(data) > 200000: raise ValueError('Expanded profile too large')
        header = prepare(data)
    except (ValueError, TypeError, KeyError, OSError):
        p.error('Invalid bounded Z-A host profile; profile contents were not logged.')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(header)
    print('Prepared bounded host identities; console root keys are not used or packaged.')

if __name__ == '__main__': main()
