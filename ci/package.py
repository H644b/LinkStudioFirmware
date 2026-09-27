#!/usr/bin/env python3
"""Create release assets from a complete ESP-IDF USB/MIDI build; never include runtime data."""
from pathlib import Path
import argparse, base64, hashlib, json, re, shutil

def package(build, output, version, source_commit):
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,80}',version):
        raise ValueError('Invalid build version')
    output.mkdir(parents=True,exist_ok=True)
    images=[];payloads={}
    for address, name in ((0x1000,'bootloader/bootloader.bin'),(0x8000,'partition_table/partition-table.bin'),
                          (0xd000,'ota_data_initial.bin'),(0x10000,'pokeldn_radio.bin')):
        path=build/name;data=path.read_bytes();filename=path.name
        if not data or len(data)%4:
            raise ValueError('Invalid image length: '+name)
        if address in (0x1000,0x10000) and (len(data)<32 or data[0]!=0xe9 or data[2]!=2 or data[3]>>4!=2 or data[12:14]!=b'\x02\0'):
            raise ValueError('Expected ESP32-S2, DIO, 4 MB image: '+name)
        images.append(dict(address=address,file=filename,bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
        payloads[filename]=base64.b64encode(data).decode('ascii')
        shutil.copyfile(path,output/filename)
    for i,image in enumerate(images):
        end=images[i+1]['address'] if i+1<len(images) else 0x1f0000
        if image['address']+image['bytes']>end:
            raise ValueError('Image exceeds partition boundary')
    manifest=dict(schema_version=1,repository='H644b/LinkStudioFirmware',version=version,
        target='esp32s2',board='Flipper Wi-Fi Developer Board',variant='usb-midi',layout='ota-v1',
        flash_bytes=0x400000,idf_commit='fff9895c82d744c7237be8847347bdd1b07c6643',
        source_commit=source_commit,capabilities=['usbpace-v1','midi-v1','bootcmd-v1','ota-v1','led-v1','uart-v1','wifi-update-v1','wifi-profiles-v1'],
        source_hardware_verified=False,images=images)
    (output/'firmware.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # One immutable asset avoids mismatched manifests/images and keeps browser
    # requests to two public GitHub API calls per firmware check.
    (output/'firmware.bundle.json').write_text(json.dumps(dict(manifest=manifest,payloads=payloads),separators=(',',':'))+'\n')
    names=[image['file'] for image in images]+['firmware.json','firmware.bundle.json']
    (output/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256((output/name).read_bytes()).hexdigest()+'  '+name+'\n' for name in names))
    print(f'Packaged {version}: {len(images)} ESP32-S2 USB/MIDI images')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--version',required=True)
    p.add_argument('--source-commit',required=True);a=p.parse_args()
    package(a.build,a.output,a.version,a.source_commit)
