#!/usr/bin/env python3
"""Fetch the pinned, checksum-verified Zstd source into the workspace build cache."""
from pathlib import Path
import hashlib
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
VERSION = '1.5.7'
SHA256 = 'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'
BUILD = ROOT / '.build'


def prepare():
    BUILD.mkdir(exist_ok=True)
    archive = BUILD / f'zstd-{VERSION}.tar.gz'
    if not archive.exists():
        url = f'https://github.com/facebook/zstd/releases/download/v{VERSION}/{archive.name}'
        with urllib.request.urlopen(url) as response:
            data = response.read(16 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise ValueError('Zstd download checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('Cached Zstd archive checksum mismatch')
    source = BUILD / f'zstd-{VERSION}'
    if not (source / 'lib/zstd.h').exists():
        with tarfile.open(archive) as files:
            members = []
            for member in files.getmembers():
                path = Path(member.name)
                # CLI test aliases are not part of the firmware dependency.
                if member.issym() and member.name in (
                    f'{source.name}/tests/cli-tests/bin/unzstd',
                    f'{source.name}/tests/cli-tests/bin/zstdcat',
                ):
                    continue
                if (path.is_absolute() or '..' in path.parts or
                        path.parts[0] != source.name or not (member.isfile() or member.isdir())):
                    raise ValueError('Unexpected path or link in Zstd archive')
                members.append(member)
            files.extractall(BUILD, members=members, filter='data')
    return source


if __name__ == '__main__':
    print(prepare())
