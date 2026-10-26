#!/usr/bin/env python3
"""Which nn:: *definitions* does a title reach that this host cannot answer?

Why this exists
---------------
game-intake.py deliberately ignores nn:: symbols, because a title's own SDK
module (subsdk1/std) defines them and they execute as guest code. That is still
true -- but the host *overrides* a set of nn:: entry points with HLE stubs in
native/host_services.cpp. When a title's SDK is newer than the ones already
supported, the mangled names differ (extra defaulted arguments, changed
signatures) and the override silently misses. The guest then runs its own
implementation, which expects real kernel service sessions and faults deep
inside the SDK with no useful message.

So this tool answers one question: for module M, which nn:: symbols that M
*defines* are covered by a host HLE entry, and of the uncovered ones, which
belong to subsystems where the host has already established a partial
override set (i.e. where a miss is likely a real gap rather than business as
usual)?

Reading NSO0 needs LZ4: segments are per-segment raw LZ4 blocks. See
native/loader.cpp Module::load for the authoritative header layout.

Usage
-----
    tools/hle-coverage.py --exefs <dir with main, sdk, subsdk1, ...>
    tools/hle-coverage.py --exefs <dir> --prefix _ZN2nn2fs --list
    tools/hle-coverage.py --exefs <dir> --json out.json
"""
import argparse
import json
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent / 'native'
MODULES = ('rtld', 'main', 'subsdk0', 'subsdk1', 'sdk')


def lz4_block(src, out_size):
    """Decode one raw LZ4 block, exactly `out_size` bytes."""
    out = bytearray()
    pos, n = 0, len(src)
    while pos < n and len(out) < out_size:
        token = src[pos]; pos += 1
        literals = token >> 4
        if literals == 15:
            while True:
                extra = src[pos]; pos += 1
                literals += extra
                if extra != 255:
                    break
        out += src[pos:pos + literals]
        pos += literals
        if len(out) >= out_size or pos >= n:
            break
        offset = src[pos] | (src[pos + 1] << 8); pos += 2
        if offset == 0:
            raise ValueError('lz4: zero offset')
        length = token & 15
        if length == 15:
            while True:
                extra = src[pos]; pos += 1
                length += extra
                if extra != 255:
                    break
        length += 4
        start = len(out) - offset
        if start < 0:
            raise ValueError('lz4: match before start of output')
        for i in range(length):
            out.append(out[start + i])
    if len(out) < out_size:
        raise ValueError('lz4: short output %d < %d' % (len(out), out_size))
    return bytes(out[:out_size])


def load_segments(path):
    """Return (header offsets for dynstr/dynsym, decompressed segments)."""
    data = path.read_bytes()
    if data[:4] != b'NSO0':
        raise ValueError('%s is not an NSO0 image' % path)
    flags = struct.unpack_from('<I', data, 0x0C)[0]
    segments = {}
    for index, name in enumerate(('text', 'ro', 'data')):
        file_offset, memory_offset, size = struct.unpack_from('<III', data, 0x10 + index * 16)
        stored = struct.unpack_from('<I', data, 0x60 + index * 4)[0] if flags & (1 << index) else size
        raw = data[file_offset:file_offset + stored]
        segments[name] = lz4_block(raw, size) if flags & (1 << index) else raw
    dynamic = struct.unpack_from('<IIII', data, 0x90)
    return dynamic, segments


def defined_symbols(dynamic, segments):
    """Names in the module's own .dynsym. Stripped modules keep only exports.

    The dynstr/dynsym offsets live in the *file* header at 0x90..0x9C but are
    resolved against the rodata base, matching loader.cpp's Module::load.
    """
    str_offset, str_size, sym_offset, sym_size = dynamic
    data = segments['ro']
    if sym_offset + sym_size > len(data) or str_offset + str_size > len(data):
        return set()
    names = set()
    for index in range(sym_size // 24):
        at = sym_offset + index * 24
        name_at = struct.unpack_from('<I', data, at)[0]
        if name_at >= str_size:
            continue
        end = data.find(b'\0', str_offset + name_at, str_offset + str_size)
        if end < 0:
            continue
        names.add(data[str_offset + name_at:end].decode('ascii', 'ignore'))
    return names


def host_hle_names():
    names = set()
    for path in list(NATIVE.rglob('*.cpp')) + list(NATIVE.rglob('*.c')) + list(NATIVE.rglob('*.inc')):
        try:
            text = path.read_text(encoding='utf-8', errors='ignore')
        except OSError:
            continue
        names.update(re.findall(r'"(_ZN2nn[0-9A-Za-z_]+)"', text))
    return names


def subsystem(symbol):
    match = re.match(r'_ZN2nn(\d+)([A-Za-z0-9_]+)', symbol)
    if not match:
        return '?'
    length = int(match.group(1))
    return match.group(2)[:length]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exefs', required=True, type=pathlib.Path)
    parser.add_argument('--prefix', default=None, help='only report symbols with this prefix')
    parser.add_argument('--list', action='store_true', help='print every uncovered symbol')
    parser.add_argument('--json', type=pathlib.Path)
    args = parser.parse_args()

    host = host_hle_names()
    defined, per_module = set(), {}
    for name in MODULES:
        path = args.exefs / name
        if not path.exists():
            continue
        try:
            dynamic, segments = load_segments(path)
            found = defined_symbols(dynamic, segments)
        except (ValueError, struct.error) as error:
            print('  %-8s skipped: %s' % (name, error), file=sys.stderr)
            continue
        per_module[name] = len(found)
        defined |= found

    if not defined:
        print('no dynamic symbols found -- module set does not look loadable')
        return 1
    if args.prefix:
        defined = {s for s in defined if s.startswith(args.prefix)}

    covered = sorted(s for s in defined if s in host)
    missing = sorted(s for s in defined if s not in host)
    buckets = {}
    for symbol in missing:
        buckets.setdefault(subsystem(symbol), []).append(symbol)
    host_subsystems = {subsystem(s) for s in host if s.startswith('_ZN2nn')}
    at_risk = {k: v for k, v in buckets.items() if k in host_subsystems}

    print('modules      : ' + ', '.join('%s=%d' % kv for kv in per_module.items()))
    print('nn:: defined : %d   host HLE entries: %d' % (len(defined), len(host)))
    print('covered      : %d   uncovered: %d' % (len(covered), len(missing)))
    print('uncovered by subsystem (host already partially overrides these):')
    for name, symbols in sorted(at_risk.items(), key=lambda kv: -len(kv[1])):
        print('  %-8s %4d' % (name, len(symbols)))
    other = {k: len(v) for k, v in buckets.items() if k not in host_subsystems}
    if other:
        print('uncovered by subsystem (host has no entry at all):')
        for name, count in sorted(other.items(), key=lambda kv: -kv[1])[:12]:
            print('  %-8s %4d' % (name, count))

    if args.list:
        print('\nuncovered symbols in subsystems the host already overrides:')
        for name in sorted(at_risk):
            for symbol in at_risk[name]:
                print('  [%s] %s' % (name, symbol))

    if args.json:
        args.json.write_text(json.dumps({
            'modules': per_module,
            'defined': len(defined),
            'covered': covered,
            'uncovered_by_subsystem': {k: len(v) for k, v in sorted(buckets.items())},
            'host_overridden_subsystems': sorted(host_subsystems),
        }, indent=2, ensure_ascii=False) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
