"""Source save envelope check (mirrors src/save_store.cpp decode_source_save).

File = '!crc' + u32le block size 0xd78 + u32le checksum + 0xd78 payload bytes.
"""
import struct

PAYLOAD_SIZE = 0xd78  # 3448
FILE_SIZE = PAYLOAD_SIZE + 12  # 3460
MAGIC = b'!crc'

_TABLE = [0] * 256
for _i in range(0xff):  # 0x16d838 iterates while index < 0xff; entry 255 stays zero
    _v = _i << 8
    for _ in range(8):
        _v = ((_v << 1) ^ 0x1021) if (_v & 0x8000) else (_v << 1)
    _TABLE[_i] = _v & 0xffff

def checksum(payload):
    state = 0xffff
    for byte in payload:
        state = ((state << 8) ^ _TABLE[(byte ^ (state >> 8)) & 0xff]) & 0xffffffff
    return (~state) & 0xffffffff

def problems(data):
    """Return a list of format problems; empty means a well-formed save file."""
    out = []
    if len(data) != FILE_SIZE:
        return [f'size {len(data)} != {FILE_SIZE}']
    if data[:4] != MAGIC:
        out.append('magic is not !crc')
    if struct.unpack('<I', data[4:8])[0] != PAYLOAD_SIZE:
        out.append('block size is not 0xd78')
    if struct.unpack('<I', data[8:12])[0] != checksum(data[12:]):
        out.append('checksum mismatch')
    return out

def payload(data):
    p = problems(data)
    if p:
        raise ValueError('malformed save: ' + '; '.join(p))
    return data[12:]

def encode(payload_bytes):
    """Build a well-formed file (used by synthetic tests)."""
    if len(payload_bytes) != PAYLOAD_SIZE:
        raise ValueError('payload size')
    return MAGIC + struct.pack('<II', PAYLOAD_SIZE, checksum(payload_bytes)) + bytes(payload_bytes)
