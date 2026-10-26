#!/usr/bin/env python3
"""Pack a prepared game directory into a game.s2a bundle (schema 1).

Same container BundleInstaller reads: a STORED zip whose first entry is
index.json, then one entry per file. Unlike s2a/bundle.py this accepts a title
with no offline shader cache at all -- a title whose shaders only exist inside a
single engine archive (Koei Tecmo's LINKDATA, for instance) has nothing to
extract ahead of time, and its shaders arrive through the runtime capture loop
instead. The engine reads .xgs from shaders/vk, which ships separately in
shader-bundles.zip, so an empty shader set here is consistent rather than a
special case.

Usage:
    make-s2a.py --game <dir with ExeFS/ and RomFS/> --title-id 01001c500e72e000 \
        --output <game.s2a> [--name "Display Name"] [--shaders <dir>]
"""
import argparse
import hashlib
import json
import zipfile
from pathlib import Path

BLOCK = 4 * 1024 * 1024


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(BLOCK):
            h.update(block)
    return h.hexdigest()


def collect(game):
    files = []
    for part in ('ExeFS', 'RomFS', 'AddOnContent'):
        for path in sorted((game / part).rglob('*')):
            if path.is_symlink():
                raise SystemExit(f'symbolic links are not allowed: {path}')
            if path.is_file():
                files.append((f'game/{path.relative_to(game).as_posix()}', path))
    build = game / 'game-build.json'
    if not build.is_file():
        raise SystemExit('game-build.json missing; run tools/game_profile.py first')
    files.append(('game/game-build.json', build))
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--title-id', required=True)
    ap.add_argument('--name', default='')
    ap.add_argument('--shaders', type=Path)
    args = ap.parse_args()

    files = collect(args.game)
    if args.shaders:
        for path in sorted(args.shaders.rglob('*')):
            if path.is_file():
                files.append(('shaders/' + path.relative_to(args.shaders).as_posix(), path))

    records = [dict(path=name, size=path.stat().st_size, sha256=digest(path))
               for name, path in files]
    index = dict(schema=1, title_id=args.title_id.lower(), total_bytes=sum(r['size'] for r in records),
                 directories=[], files=records)
    raw = json.dumps(index, ensure_ascii=True, separators=(',', ':')).encode()
    identity = hashlib.sha256(raw).hexdigest()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_suffix('.partial')
    try:
        with zipfile.ZipFile(partial, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as z:
            z.writestr('index.json', raw)
            for (name, path), record in zip(files, records):
                with path.open('rb') as source, z.open(name, 'w', force_zip64=record['size'] >= zipfile.ZIP64_LIMIT) as target:
                    while block := source.read(BLOCK):
                        target.write(block)
        partial.replace(args.output)
    finally:
        partial.unlink(missing_ok=True)

    summary = dict(schema=1, title_id=args.title_id.lower(), bundle_id=identity,
                   total_bytes=index['total_bytes'], name=args.name)
    print(json.dumps(summary, indent=2))
    print(f'{args.output}  {len(records)} files  {index["total_bytes"]} bytes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
