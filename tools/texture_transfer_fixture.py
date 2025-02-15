#!/usr/bin/env python3
"""Convert texture-transfer observer JSON (research evidence) into the text
fixture read by okage-texture-transfer-compare. Usage: fixture.py in.json out.txt"""
import json, sys
d = json.load(open(sys.argv[1]))
with open(sys.argv[2], 'w') as out:
    for c in d['cases']:
        lo, hi = c['descriptor']
        out.write(f"case {c['case']} {c['kind']} {c['psm']} {c['w_or_n']} {c['h']} {c['clut_alloc_rows']} "
                  f"{lo} {hi} {c['tbp']} {c['tbw']} {len(c['touched'])}\n")
        for a, v in c['touched']:
            out.write(f"{a} {v}\n")
