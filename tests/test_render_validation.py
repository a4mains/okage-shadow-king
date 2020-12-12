import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("verify_render", Path(__file__).resolve().parents[1] / "tools/verify_render.py")
render = importlib.util.module_from_spec(spec)
spec.loader.exec_module(render)

class RenderValidation(unittest.TestCase):
    def test_culled_writes(self):
        data = struct.pack("<I", 0) + b"\xcd" * 160
        render.check_primitive(data, 3, True, True, False)
        with self.assertRaises(ValueError):
            render.check_primitive(data[:-1], 3, True, True, False)
        damaged = bytearray(data)
        damaged[16] = 0
        with self.assertRaises(ValueError):
            render.check_primitive(damaged, 3, True, True, False)

    def test_branch_delay_write(self):
        data = bytearray(struct.pack("<I", 0) + b"\xcd" * 160)
        data[52:68] = struct.pack("<4i", 256,512,4,16)
        render.check_primitive(data, 3, False, True, False)
        with self.assertRaises(ValueError):
            render.check_primitive(data, 3, False, True, True)

    def test_uv_bit_comparison(self):
        data = struct.pack("<2f", .5, .25)
        render.compare_uv(data, data, 1)
        for bad in (data[:-1], struct.pack("<2f", .5, .25000003)):
            with self.assertRaises(ValueError):
                render.compare_uv(data, bad, 1)

if __name__ == "__main__":
    unittest.main()
