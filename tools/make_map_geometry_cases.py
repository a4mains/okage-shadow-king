#!/usr/bin/env python3
"""Reproduce the five bounded geometry fixtures from the pinned research RAM.

This is an oracle input generator, never an app asset importer. It preserves
the original eight rendered element channels and uses synthetic job/camera
settings to exercise rotation order, billboard transforms and distance fade.
"""
import argparse
import hashlib
from pathlib import Path
import struct
from verify_event import write_exact

RAM_SHA = '678908347fbaf42471f000f256164407ab6ead08309efc69303f2d3b58acb6c1'
CASES_SHA = '25981c43029a59e3a425944887b4fffd7ad8075ec81fb6edfe6e3dfbedb260b5'

def make_cases(ram):
    if len(ram) != 32*1024*1024 or hashlib.sha256(ram).hexdigest() != RAM_SHA:
        raise ValueError('Different exterior research RAM')
    u = lambda at: struct.unpack_from('<I', ram, at)[0]
    v = lambda at: struct.unpack_from('<4f', ram, at)
    job = u(u(0x1f849c)+8)
    if (u(job), u(job+8)) != (25, 8):
        raise ValueError('Expected template25 in handle2')
    texture_map = {u(u(0x24e780)+i*4): i for i in range(31)}
    elements = bytearray()
    for i in range(8):
        e = u(job+12)+i*128
        rendered = u(e+0x78)
        elements.extend(ram[rendered:rendered+64])
        elements.extend(struct.pack('<4I', 24 if i < 3 else 29,
                                    u(e+0x54), u(e+0x58), texture_map[u(e+0x5c)&65535]))
    identity = (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
    cases = [
        ((0,0,10,1), (0,0,0,0), (1,1,1,1), (0,0,0,0), identity),
        ((3,-5,425,1), (.13,.27,-.11,0), (2,3,.5,1), (.1,.2,.3,0), identity),
        ((3,-5,460,1), (.125,.25,.5,0), (.5,2,3,1), (.125,-.25,0,0), identity),
        (v(job+16), v(job+32), v(job+48), (.07,.63,.1,0), identity),
        ((3,5,8,1), (.33,.21,.17,0), (1.5,.25,4,1), (.24,.47,.13,0),
         (0,0,-1,0, 0,1,0,0, 1,0,0,0, -2,-3,440,1)),
    ]
    result = bytearray()
    for i, (position, rotation, scale, camera, view) in enumerate(cases):
        result.extend(struct.pack('<I37f', i, *position, *rotation, *scale,
                                  .75,.25,.25,.6, *camera, *view, 450))
        result.extend(elements)
    if hashlib.sha256(result).hexdigest() != CASES_SHA:
        raise ValueError('Generated fixture differs from completed original probe')
    return bytes(result)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('ram', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    write_exact(args.output, make_cases(args.ram.read_bytes()))
    print('Reproduced five geometry cases:', CASES_SHA)

if __name__ == '__main__':
    main()
