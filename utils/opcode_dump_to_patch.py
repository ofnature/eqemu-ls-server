#!/usr/bin/env python3
"""
Convert a protocol opcode dump (from reverse engineering the client) into
patch_Laurion.conf-style lines. Use this when you've dumped the ntoh table
from eqgame.exe at 0x140636670 (no MQ2 required).

Input: text file with one protocol opcode per line (hex, e.g. 6d2d or 0x6d2d).
Optional: path to existing patch_Laurion.conf to skip opcodes already mapped.

Usage:
  python opcode_dump_to_patch.py opcode_dump.txt
  python opcode_dump_to_patch.py opcode_dump.txt --patch utils/patches/patch_Laurion.conf
  python opcode_dump_to_patch.py opcode_dump.txt -o new_opcodes.conf
"""

import argparse
import re
import sys
from typing import Optional

def parse_hex(s: str) -> Optional[int]:
    s = s.strip()
    if not s or s.startswith('#'):
        return None
    m = re.match(r'(?:0x)?([0-9a-fA-F]{2,4})\b', s)
    if m:
        return int(m.group(1), 16)
    return None

def read_dump(path: str) -> set[int]:
    seen = set()
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            val = parse_hex(line)
            if val is not None and 0 <= val <= 0xFFFF:
                seen.add(val)
    return seen

def read_existing_patch(path: str) -> set[int]:
    """Return set of protocol opcode values already in patch (0xXXXX)."""
    seen = set()
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'=0x([0-9a-fA-F]{2,4})\b', line)
            if m:
                seen.add(int(m.group(1), 16))
    return seen

def main():
    ap = argparse.ArgumentParser(description='Opcode dump to patch_Laurion.conf lines (no MQ2)')
    ap.add_argument('dump', help='Input: one protocol opcode per line (hex)')
    ap.add_argument('--patch', '-p', help='Existing patch_Laurion.conf to skip known opcodes')
    ap.add_argument('--output', '-o', help='Write lines to file (default: stdout)')
    ap.add_argument('--include-known', action='store_true', help='Output all opcodes from dump, even if already in patch')
    args = ap.parse_args()

    dump_opcodes = read_dump(args.dump)
    if not dump_opcodes:
        print('No valid opcodes in dump.', file=sys.stderr)
        sys.exit(1)

    existing = read_existing_patch(args.patch) if args.patch else set()
    if args.patch and not args.include_known:
        to_emit = sorted(dump_opcodes - existing)
    else:
        to_emit = sorted(dump_opcodes)

    lines = [f"OP_Unknown_{v:04X}=0x{v:04x}" for v in to_emit]
    out = '\n'.join(lines) + '\n'

    if args.output:
        with open(args.output, 'w', encoding='utf-8') as f:
            f.write(out)
        print(f"Wrote {len(lines)} lines to {args.output}", file=sys.stderr)
    else:
        print(out, end='')

    if args.patch and not args.include_known and existing:
        skipped = len(dump_opcodes & existing)
        if skipped:
            print(f"Skipped {skipped} opcodes already in patch.", file=sys.stderr)

if __name__ == '__main__':
    main()
