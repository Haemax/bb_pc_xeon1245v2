#!/usr/bin/env python3
"""Find the instructions an older x86-64 CPU cannot run in the game's code.

Bloodborne's code was compiled for the PS4's CPU (AMD Jaguar) and uses BMI1 (andn, bextr, blsr,
blsmsk, blsi), MOVBE and LZCNT/TZCNT directly. Intel CPUs before Haswell (Sandy Bridge, Ivy
Bridge: Core i*-2000/3000, Xeon E3 v1/v2) lack them. src/cpu_compat.c emulates them; this
script lists where they are: a linear disassembly (GNU objdump, part of binutils) of every
executable segment of out/boot-linked.bin, written as out/cpucompat.bin.

    cpu_compat.py out/boot-linked.bin --out out/cpucompat.bin [--force]

Table format (BBCPUC3), little endian: 8 bytes magic, u64 sites, u64 caves, u64 targets, sites x u64
image offsets, caves x (u64 offset, u64 length), targets x u64 image offsets. A target is the destination
of a direct jmp/jcc/call/loop that lies 1..19 bytes after a site: the runtime moves the instruction after
a short lzcnt/tzcnt into its stub only when nothing branches into the bytes it overwrites (indirect
branches and jump tables are not visible to a linear scan; such code is not found in the middle of a
straight run of arithmetic). A cave is padding (int3 / nop after a ret, jmp, ud2 or
hlt) of 5+ bytes close to a site. The 4-byte sites (lzcnt/tzcnt/movbe on registers, too short for a
5-byte jmp) hold a 2-byte jmp to a cave, which holds the jmp to the stub, instead of trapping
with SIGILL. BBCPUC1 (no caves) and BBCPUC2 (no targets, so no relocation) are still accepted by the loader.
"""
import argparse
import bisect
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

MNEMONICS = {b'lzcnt', b'tzcnt', b'movbe', b'andn', b'bextr', b'blsr', b'blsmsk', b'blsi'}
LINE = re.compile(rb'^\s*([0-9a-f]+):\t(\S+)(.*)$')
BRANCH_TARGET = re.compile(rb'^\s*(?:0x)?([0-9a-f]+)\b')
VERSION = b'v3'
CAVE_REACH = 160      # keep caves this close to a site (a rel8 reaches -126..+129 bytes)
TERMINATORS = (b'ret', b'jmp', b'ud2', b'hlt')


def read_boot(path):
    """Returns (image offset in the file, image size, segments) of a BBPROBE5 boot file."""
    with open(path, 'rb') as f:
        def q(n=1):
            data = f.read(8 * n)
            if len(data) != 8 * n:
                raise ValueError('truncated boot file')
            return struct.unpack(f'<{n}Q', data)
        magic = f.read(8)
        if magic != b'BBPROBE5':
            raise ValueError(f'{path}: expected a BBPROBE5 boot file (link_modules.py output), found {magic!r}')
        size, entry, ns, nr, imports, capabilities = q(6)
        q(1 + 4)                      # procparam, eboot TLS
        modules, = q()
        q(7 * modules)
        nb, = q()
        q(3 * nb)
        segments = [q(3) for _ in range(ns)]  # address, size, flags
        f.seek(128 * imports + 32 * nr, os.SEEK_CUR)
        return f.tell(), size, segments


def classify(mnemonic, rest):
    """'term' (never falls through), 'int3', 'nop' (padding) or 'other'."""
    if mnemonic in (b'notrack', b'bnd', b'repz', b'rep', b'repnz'):
        parts = rest.split(None, 1)
        if not parts:
            return 'other'
        mnemonic, rest = parts[0], (parts[1] if len(parts) > 1 else b'')
    if mnemonic == b'int3':
        return 'int3'
    if mnemonic.startswith(b'ret') or mnemonic.startswith(b'jmp') or mnemonic in (b'ud2', b'hlt'):
        return 'term'
    if mnemonic.startswith(b'nop'):
        return 'nop'
    if mnemonic in (b'data16', b'cs', b'ds', b'es', b'ss') and b'nop' in rest:
        return 'nop'
    if mnemonic == b'xchg' and rest.strip() == b'%ax,%ax':
        return 'nop'
    return 'other'


def scan(path, objdump):
    """Returns (sites, caves). Caves are runs of padding: int3 or nop instructions that directly
    follow an instruction that never falls through (ret, jmp, ud2, hlt), so nothing executes or
    jumps into them, and that the linear disassembly cannot have mistaken for real code."""
    offset, size, segments = read_boot(path)
    sites = []
    caves = []
    targets = set()
    with open(path, 'rb') as f, tempfile.TemporaryDirectory() as tmp:
        for address, length, flags in segments:
            if not flags & 1 or not length:
                continue
            f.seek(offset + address)
            data = f.read(length)
            part = os.path.join(tmp, 'segment.bin')
            with open(part, 'wb') as out:
                out.write(data)
            cmd = [objdump, '-D', '-b', 'binary', '-m', 'i386:x86-64', '--no-show-raw-insn', part]
            run_start = run_end = None   # the padding run being collected
            prev_addr = prev_valid = None  # previous instruction, and whether it was valid padding
            ok = False                   # padding may follow: the last real instruction never falls through

            def close_prev(end):
                nonlocal run_start, run_end
                if prev_valid:
                    if run_end == prev_addr:
                        run_end = end
                    else:
                        flush()
                        run_start, run_end = prev_addr, end
                elif run_start is not None:
                    flush()

            def flush():
                nonlocal run_start, run_end
                if run_start is not None and run_end - run_start >= 5:
                    caves.append((address + run_start, run_end - run_start))
                run_start = run_end = None

            with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as proc:
                for line in proc.stdout:
                    if b'\t' not in line:
                        continue
                    m = LINE.match(line)
                    if not m:
                        continue
                    here = int(m.group(1), 16)
                    mnemonic = m.group(2)
                    if prev_addr is not None:
                        close_prev(here)
                    kind = classify(mnemonic, m.group(3))
                    valid = False
                    if kind == 'term':
                        ok = True
                    elif kind == 'int3':
                        valid = ok
                    elif kind == 'nop':
                        valid = ok
                        ok = valid
                    else:
                        ok = False
                    prev_addr, prev_valid = here, valid
                    if mnemonic in MNEMONICS:
                        sites.append(address + here)
                    elif mnemonic.startswith((b'j', b'call', b'loop')):
                        t = BRANCH_TARGET.match(m.group(3))
                        if t and b'*' not in m.group(3) and b'%' not in m.group(3):
                            targets.add(address + int(t.group(1), 16))
            if prev_addr is not None:
                close_prev(length)
            flush()
            os.unlink(part)
    sites = sorted(set(sites))
    # Only caves that a 2-byte jump from some site can reach.
    kept = []
    for start, run in sorted(caves):
        i = bisect.bisect_left(sites, start - CAVE_REACH)
        if i < len(sites) and sites[i] <= start + run + CAVE_REACH:
            kept.append((start, run))
    # Only branch targets that fall just after a site (1..19 bytes) matter for relocation.
    near = []
    for t in sorted(targets):
        i = bisect.bisect_left(sites, t) - 1   # the last site below t
        if i >= 0 and t - sites[i] < 20:
            near.append(t)
    return sites, kept, near


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('boot')
    parser.add_argument('--out', required=True)
    parser.add_argument('--force', action='store_true', help='scan even when the table is for this very boot file')
    args = parser.parse_args()
    # run.sh links the boot file again at every launch: the table is keyed by its content.
    digest = hashlib.sha1()
    with open(args.boot, 'rb') as f:
        while chunk := f.read(1 << 22):
            digest.update(chunk)
    digest.update(VERSION)       # a new table format must not reuse the old table
    stamp = args.out + '.sha1'
    try:
        with open(stamp) as f:
            known = f.read().strip()
    except OSError:
        known = ''
    if not args.force and os.path.exists(args.out) and known == digest.hexdigest():
        print(f'CPU compatibility table up to date: {args.out}')
        return 0
    objdump = shutil.which('objdump')
    if not objdump:
        print('ERROR: objdump (binutils) is needed to prepare the game for this CPU: install binutils.', file=sys.stderr)
        return 1
    print('CPU compatibility: scanning the game code for BMI1/MOVBE/LZCNT instructions (once, a minute or two)...', flush=True)
    sites, caves, targets = scan(args.boot, objdump)
    tmp = args.out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'BBCPUC3\0' + struct.pack('<QQQ', len(sites), len(caves), len(targets)) + struct.pack(f'<{len(sites)}Q', *sites))
        for start, run in caves:
            f.write(struct.pack('<QQ', start, run))
        f.write(struct.pack(f'<{len(targets)}Q', *targets))
    os.replace(tmp, args.out)
    with open(stamp, 'w') as f:
        f.write(digest.hexdigest() + '\n')
    print(f'CPU compatibility: {len(sites)} instructions, {len(caves)} padding caves and {len(targets)} nearby branch targets listed in {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
