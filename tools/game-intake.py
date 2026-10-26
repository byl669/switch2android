#!/usr/bin/env python3
"""Static intake check for a candidate title.

Why this exists
---------------
Most of what decides "will this title boot on our host" can be read off the
executable before any build:

  1. Module build IDs. The profile names them and the loader keys its rewrites
     on them. No build ID, no profile, no package.
  2. NVN entry names. A title reaches graphics through nvnBootstrapLoader and
     nvnDeviceGetProcAddress, so the names it asks for sit in .rodata as plain
     strings. A name the host does not implement becomes a gate with no function
     and stops the title at its first call. This diff is the strongest single
     predictor of graphics-side work.
  3. SVC traffic. The host interposes svc instructions wholesale, so this is not
     a gate, but the count says how much kernel surface the title leans on.

Missing nn:: *imports* are deliberately not reported: those live in the title's
own SDK module (subsdk1) and run as guest code. False alarms there are worse
than silence.

Usage
-----
    tools/game-intake.py --exefs <dir with main, subsdk0, ...> [--json out.json]
    tools/game-intake.py --nso <single module>
"""
import argparse
import json
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent / 'native'


def host_nvn_names():
    names = set()
    for path in list(NATIVE.rglob('*.c')) + list(NATIVE.rglob('*.cpp')):
        try:
            text = path.read_text(encoding='utf-8', errors='ignore')
        except OSError:
            continue
        names.update(re.findall(r'\bNVN\(\s*(nvn\w+)', text))
    protos = NATIVE / 'graphics/nvn_protos.h'
    if protos.exists():
        names.update(re.findall(r'\bNVNT_(nvn\w+)\s*\)', protos.read_text(errors='ignore')))
    return names


def host_nn_symbols():
    names = set()
    for path in list(NATIVE.rglob('*.cpp')) + list(NATIVE.rglob('*.c')) + list(NATIVE.rglob('*.inc')):
        try:
            text = path.read_text(encoding='utf-8', errors='ignore')
        except OSError:
            continue
        names.update(re.findall(r'"(_ZN2nn[0-9A-Za-z_]+)"', text))
    return names


def parse_nso(path):
    import lz4.block

    raw = path.read_bytes()
    if raw[:4] != b'NSO0':
        raise ValueError(f'{path.name}: not an NSO0 module')
    flags, = struct.unpack_from('<I', raw, 0x0C)
    t_off, _t_mem, t_size = struct.unpack_from('<III', raw, 0x10)
    r_off, _r_mem, r_size = struct.unpack_from('<III', raw, 0x20)
    build_id = raw[0x40:0x60].hex()
    str_off, str_size, sym_off, sym_size = struct.unpack_from('<IIII', raw, 0x90)

    def segment(off, size, compressed_size_at, is_compressed):
        if is_compressed:
            csize, = struct.unpack_from('<I', raw, compressed_size_at)
            return lz4.block.decompress(raw[off:off + csize], uncompressed_size=size)
        return raw[off:off + size]

    text = segment(t_off, t_size, 0x60, flags & 0x1)
    rodata = segment(r_off, r_size, 0x64, flags & 0x2)

    dynstr = rodata[str_off:str_off + str_size]
    dynsym = rodata[sym_off:sym_off + sym_size]
    defined, imported = [], []
    for i in range(0, len(dynsym) - 23, 24):
        name_off, info, _other, shndx = struct.unpack_from('<IBBH', dynsym, i)
        if name_off == 0 or name_off >= len(dynstr):
            continue
        end = dynstr.find(b'\0', name_off)
        name = dynstr[name_off:end if end >= 0 else len(dynstr)].decode('ascii', 'replace')
        if not name:
            continue
        (defined if shndx != 0 else imported).append(name)

    # SVC is 0xd4000001 with the immediate in bits 5..20. Count distinct ids.
    svc_ids = {}
    for i in range(0, len(text) - 3, 4):
        word = int.from_bytes(text[i:i + 4], 'little')
        if word & 0xFFE0001F == 0xD4000001:
            sid = (word >> 5) & 0xFFFF
            svc_ids[sid] = svc_ids.get(sid, 0) + 1

    asked = sorted(set(re.findall(rb'nvn[A-Z][A-Za-z0-9]{2,40}', rodata)))
    return {
        'file': path.name,
        'build_id': build_id,
        'text_bytes': t_size,
        'rodata_bytes': r_size,
        'svc_ids': svc_ids,
        'nvn_asked': [n.decode() for n in asked],
        'nn_defined': sum(1 for n in defined if n.startswith('_ZN2nn')),
        'nn_imported': sum(1 for n in imported if n.startswith('_ZN2nn')),
        'imports': imported,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', type=pathlib.Path)
    ap.add_argument('--nso', type=pathlib.Path)
    ap.add_argument('--json', type=pathlib.Path)
    args = ap.parse_args()
    if not args.exefs and not args.nso:
        ap.error('need --exefs or --nso')

    nvn_hosts = host_nvn_names()
    nn_hosts = host_nn_symbols()
    print(f'host: {len(nvn_hosts)} NVN entries, {len(nn_hosts)} nn:: overrides\n')

    paths = [args.nso] if args.nso else [
        p for p in sorted(args.exefs.iterdir())
        if p.is_file() and p.read_bytes()[:4] == b'NSO0']
    report = {'host_nvn': len(nvn_hosts), 'host_nn': len(nn_hosts), 'modules': []}
    for path in paths:
        mod = parse_nso(path)
        asked, missing = mod['nvn_asked'], [n for n in mod['nvn_asked'] if n not in nvn_hosts]
        overridden = sum(1 for n in mod['imports'] if n in nn_hosts)
        svc = mod['svc_ids']
        print(f'{mod["file"]:<12} build={mod["build_id"][:32]}')
        print(f'  text={mod["text_bytes"]/1e6:.1f} MB  rodata={mod["rodata_bytes"]/1e6:.1f} MB'
              f'  nn:: defined={mod["nn_defined"]} imported={mod["nn_imported"]}'
              f'  host-overridden imports={overridden}')
        print(f'  NVN asked={len(asked):<4} missing={len(missing)}'
              + ('  -> ' + ', '.join(missing[:12]) + (' …' if len(missing) > 12 else '') if missing else ''))
        print(f'  SVC calls={sum(svc.values()):<5} distinct={len(svc):<3} ids=' +
              ', '.join(f'0x{k:x}' for k in sorted(svc)[:14]) + (' …' if len(svc) > 14 else ''))
        report['modules'].append({
            'file': mod['file'], 'build_id': mod['build_id'],
            'text_mb': round(mod['text_bytes'] / 1e6, 2), 'rodata_mb': round(mod['rodata_bytes'] / 1e6, 2),
            'nn_defined': mod['nn_defined'], 'nn_imported': mod['nn_imported'],
            'host_overridden_imports': overridden,
            'nvn_asked': len(asked), 'nvn_missing': missing,
            'svc_calls': sum(svc.values()), 'svc_distinct': len(svc),
            'svc_ids': sorted(svc),
        })
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n')
        print(f'\nwrote {args.json}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
