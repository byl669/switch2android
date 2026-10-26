#!/usr/bin/env python3
"""Translate shaders the title built at run time and add them to the offline cache.

Why this exists
---------------
A title can build shader programs that never appear in RomFS, so the offline
extractor cannot have seen them. When the packaged cache is complete, such a
shader resolves to a placeholder and every draw that needs it is skipped -- the
frame still presents, so the screen shows only the clear colour while the log
reports a steady frame rate. The host already keeps the control section and the
machine code of those shaders under ``shaders/captures``; this script turns them
into SPIR-V and appends them to the cache.

The captured shaders carry no assembly text, so the texture and image slot types
that ``prepare_shaders.py`` normally infers from it are unknown. Translation is
still attempted with those tables empty: a shader that samples a texture raises
and is reported as a failure, while one that does not needs no facts at all.
That is what makes this work without a console-side disassembly.

Adding one batch commonly reveals the next: draws that were skipped never bound
their later programs, so those programs were never captured either. Run this
again after a boot to collect the next layer.

Usage
-----
    import-captured-shaders.py --serial <host:port> --package <pkg> \
        [--game <title-id>] [--cache s2a-work/cache-merged] [--dry-run]

Requires a built translator (tools/shader_translate) and spirv-val on PATH.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent
WORKSPACE = NATIVE.parent


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kwargs)


def adb(serial, *args, binary=False):
    cmd = ['adb', '-s', serial] + list(args)
    if binary:
        return subprocess.run(cmd, check=True, capture_output=True).stdout
    return run(cmd).stdout


def device_shader_dirs(serial, package):
    """Locate the extracted content directory that holds the shader cache."""
    root = f'/storage/emulated/0/Android/data/{package}/files/content'
    listing = adb(serial, 'shell', f'ls {root}')
    for entry in listing.split():
        candidate = f'{root}/{entry}/shaders'
        try:
            adb(serial, 'shell', f'ls {candidate}/manifest.json')
        except subprocess.CalledProcessError:
            continue
        return candidate
    raise SystemExit('No packaged shader cache found; run the full build once first')


def pull_captures(serial, package, game, destination):
    """Fetch every captured shader as one archive: a per-file pull is far slower."""
    captures = f'files/games/{game}/shaders/captures'
    archive = f'files/games/{game}/shader-captures.tar'
    adb(serial, 'shell',
        f"run-as {package} sh -c 'cd {captures} && tar cf ../../shader-captures.tar .'")
    data = adb(serial, 'shell', f'run-as {package} cat {archive}', binary=True)
    adb(serial, 'shell', f'run-as {package} rm -f {archive}')
    path = destination / 'captures.tar'
    path.write_bytes(data)
    extracted = destination / 'captures'
    extracted.mkdir()
    # Only regular files are extracted: the archive starts with a "./" entry, and
    # asking tarfile to recreate the directory it is extracting into fails.
    with tarfile.open(path) as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            name = pathlib.PurePosixPath(member.name).name
            (extracted / name).write_bytes(tar.extractfile(member).read())
    return extracted


def translate(pending, workdir, translator):
    """Run the offline pipeline over the pending shaders; returns (ok, failed)."""
    source = workdir / 'src'
    jobs = workdir / 'jobs'
    source.mkdir()
    jobs.mkdir()
    for key in pending:
        for suffix in ('.control', '.code'):
            shutil.copy2(workdir / 'captures' / f'{key}{suffix}', source / f'{key}{suffix}')
    run([sys.executable, str(NATIVE / 'tools/prepare_shaders.py'), str(source), str(jobs)])
    result = subprocess.run(['dotnet', str(translator), str(jobs / 'jobs.json'), str(jobs)],
                            capture_output=True, text=True)
    failures = {}
    for line in (result.stdout + result.stderr).splitlines():
        if '：' in line and line.startswith('【红】'):
            key, _, reason = line[3:].partition('：')
            failures[key.strip()] = reason.strip()
    produced = {}
    for spv in jobs.glob('*.spv'):
        key = spv.stem
        reflection = json.loads((jobs / f'{key}.res.json').read_text())
        if reflection.get('notes'):
            # A note means a resource type or format had to be guessed, so the
            # binary is not something to ship: the draw would read the wrong
            # view. Treat it like a failure and report why.
            failures[key] = 'reflection notes: ' + '; '.join(reflection['notes'][:3])
            continue
        produced[key] = (spv, jobs / f'{key}.res.json', reflection['stage'])
    return produced, failures


def validate(produced, spirv_val):
    kept, rejected = {}, {}
    for key, (spv, res, stage) in produced.items():
        check = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)],
                               capture_output=True, text=True)
        if check.returncode:
            rejected[key] = check.stderr.strip().splitlines()[0] if check.stderr else 'invalid SPIR-V'
            continue
        kept[key] = (spv, res, stage)
    return kept, rejected


def merge(cache, produced):
    """Add the translated shaders to the cache and re-fingerprint the manifest."""
    manifest_path = cache / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    appended = set(manifest.get('appended_captures', []))
    for key, (spv, res, stage) in produced.items():
        shutil.copy2(spv, cache / f'{key}.spv')
        shutil.copy2(res, cache / f'{key}.res.json')
        manifest['shaders'][key] = {
            'spv': hashlib.sha256(spv.read_bytes()).hexdigest(),
            'reflection': hashlib.sha256(res.read_bytes()).hexdigest(),
            'stage': stage,
        }
        appended.add(key)
    # jobs_sha256 fingerprints the RomFS-derived job list only; these shaders came
    # from run-time captures that no job list describes, so they are recorded
    # under their own key rather than folded into a value that would then be wrong.
    manifest['appended_captures'] = sorted(appended)
    payload = {k: v for k, v in manifest.items() if k != 'id'}
    manifest['id'] = hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()
    manifest_path.write_text(json.dumps(manifest, indent=2))
    return manifest, sorted(produced)


def deploy(serial, package, target, cache, keys):
    adb(serial, 'shell', f'am force-stop {package}')
    for key in keys:
        for suffix in ('.spv', '.res.json'):
            adb(serial, 'push', str(cache / f'{key}{suffix}'), f'{target}/{key}{suffix}')
    adb(serial, 'push', str(cache / 'manifest.json'), f'{target}/manifest.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--game', default='0100620012d6e000')
    parser.add_argument('--cache', type=pathlib.Path, default=WORKSPACE / 's2a-work/cache-merged')
    parser.add_argument('--translator', type=pathlib.Path,
                        default=NATIVE / 'tools/shader_translate/bin/Release/net10.0/dmsh_xlate.dll')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    if not args.translator.exists():
        raise SystemExit(f'Translator not built: {args.translator} (dotnet build -c Release)')
    if not args.cache.exists():
        raise SystemExit(f'Cache directory missing: {args.cache}')

    environ = dict(os.environ, ADB_MDNS='0')
    os.environ.update(environ)

    target = device_shader_dirs(args.serial, args.package)
    cache_manifest = json.loads((args.cache / 'manifest.json').read_text())
    known = set(cache_manifest['shaders'])

    with tempfile.TemporaryDirectory(prefix='ot2-shaders-') as temp:
        workdir = pathlib.Path(temp)
        captures = pull_captures(args.serial, args.package, args.game, workdir)
        captured = {p.stem for p in captures.glob('*.control')}
        pending = sorted(captured - known)
        print(f'captured={len(captured)} cached={len(known)} pending={len(pending)}')
        if not pending:
            print('Nothing to import.')
            return
        produced, failures = translate(pending, workdir, args.translator)
        kept, rejected = validate(produced, 'spirv-val')
        for key, reason in rejected.items():
            failures[key] = f'invalid SPIR-V: {reason}'
        print(f'translated={len(kept)} failed={len(failures)}')
        if failures:
            report = workdir / 'failures.json'
            report.write_text(json.dumps(failures, indent=2, sort_keys=True))
            print(f'  failures recorded in {report}')
            for key, reason in sorted(failures.items())[:8]:
                print(f'  {key}: {reason}')
        if not kept:
            raise SystemExit('No shader translated; the cache is unchanged')
        if args.dry_run:
            print('Dry run: cache left untouched.')
            return
        manifest, added = merge(args.cache, kept)
        deploy(args.serial, args.package, target, args.cache, added)
        print(f'added={len(added)} cache_entries={len(manifest["shaders"])} id={manifest["id"][:16]}')
        print('Restart the title, then run this again: the next layer of shaders is '
              'only captured once the draws this batch unblocked have run.')


if __name__ == '__main__':
    main()
