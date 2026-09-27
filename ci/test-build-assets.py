"""Release inputs fail closed; manifests match the bytes they distribute."""
from pathlib import Path
import copy
import hashlib
import importlib.util
import json
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

def module(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

profiles = module('prepare-host-profile')
packager = module('package')

class AssetsTests(unittest.TestCase):
    def test_bounded_profiles_reject_extra_fields_wrong_title_and_bad_sizes(self):
        valid = dict(format=1, protocol=1, communication_id='0100f43008c44000',
                     scene_id=1, app_version=6, profiles=[dict.fromkeys(
                         ('ssid', 'server_random', 'advertise_key', 'data_key'), '01' * 16)])
        self.assertIn('#define LS_PROFILE_APP_VERSION 6', profiles.prepare(json.dumps(valid)))
        bad = []
        for field, value in (('format', True), ('protocol', 2), ('communication_id', '0' * 16),
                             ('app_version', 0), ('app_version', True), ('profiles', []),
                             ('profiles', valid['profiles'] * 257), ('profiles', [None])):
            case = copy.deepcopy(valid); case[field] = value; bad.append(case)
        case = copy.deepcopy(valid); case['keys'] = 'not allowed'; bad.append(case)
        case = copy.deepcopy(valid); case['profiles'][0]['ssid'] = '01' * 15; bad.append(case)
        bad.extend(([], None, 'not a profile'))
        for case in bad:
            with self.assertRaises((TypeError, ValueError)):
                profiles.prepare(json.dumps(case))

    def test_manifest_hashes_and_standalone_capability_match_image(self):
        scratch = Path('.build/firmware-asset-tests'); scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            root = Path(temporary); build = root / 'build'; build.mkdir()
            header = bytearray(32); header[0] = 0xe9; header[2] = 2; header[3] = 0x20; header[12] = 2
            images = {'bootloader/bootloader.bin': bytes(header),
                      'partition_table/partition-table.bin': bytes(32),
                      'ota_data_initial.bin': bytes(32), 'pokeldn_radio.bin': bytes(header)}
            for name, data in images.items():
                target = build / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'compiled runtime'):
                packager.package(build, root / 'invalid', 'test', 'fixture', standalone=True)
            app = bytes(header) + b'standalone-v1\0\0\0'
            app += bytes(-len(app) % 4)
            (build / 'pokeldn_radio.bin').write_bytes(app)
            output = root / 'release'
            packager.package(build, output, 'test', 'fixture', standalone=True)
            manifest = json.loads((output / 'firmware.json').read_text())
            self.assertIn('standalone-v1', manifest['capabilities'])
            self.assertFalse(manifest['source_hardware_verified'])
            for image in manifest['images']:
                data = (output / image['file']).read_bytes()
                self.assertEqual(len(data), image['bytes'])
                self.assertEqual(hashlib.sha256(data).hexdigest(), image['sha256'])

if __name__ == '__main__':
    unittest.main()
