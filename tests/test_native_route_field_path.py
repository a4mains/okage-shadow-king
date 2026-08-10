"""field_path: floor-grid planning with walls, steps, automatic-trigger cost
and roof layers, on synthetic polygons."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from native_route import field_path as fp  # noqa: E402

UP = [0, -16384, 0]

def quad(x0, z0, x1, z1, y=0.0, material=0x0001):
    return {'material': material, 'normal': UP, 'flags': 1,
            'vertices': [[x0, y, z0], [x1, y, z0], [x1, y, z1], [x0, y, z1]]}

def wall(x, z0, z1, top=-10.0, bottom=1.0):
    return {'material': 0, 'normal': [-16384, 0, 0], 'flags': 1,
            'vertices': [[x, bottom, z0], [x, bottom, z1], [x, top, z1], [x, top, z0]]}

class FieldPathTests(unittest.TestCase):
    def test_straight_plan(self):
        r = fp.plan([quad(0, 0, 40, 10)], (2, 5), (38, 5))
        self.assertEqual((r['waypoints'][0], r['waypoints'][-1], r['crossed']), ((2, 5), (38, 5), []))

    def test_wall_forces_detour_and_full_wall_blocks(self):
        floor = quad(0, 0, 40, 40)
        r = fp.plan([floor, wall(20, 0, 30)], (5, 5), (35, 5))
        self.assertTrue(any(z >= 30 for _, z in r['waypoints']), r['waypoints'])
        self.assertIsNone(fp.plan([floor, wall(20, -1, 41)], (5, 5), (35, 5)))

    def test_step_limit(self):
        low, high = quad(0, 0, 20, 10, 0.0), quad(20, 0, 40, 10, -5.0)
        self.assertIsNone(fp.plan([low, high], (5, 5), (35, 5), start_y=0, goal_y=-5))
        ramp = {'material': 1, 'normal': [3000, -16000, 0], 'flags': 1,
                'vertices': [[20, 0, 0], [40, -5, 0], [40, -5, 10], [20, 0, 10]]}
        self.assertIsNotNone(fp.plan([low, ramp], (5, 5), (35, 5), start_y=0, goal_y=-4.4))

    def test_automatic_trigger_costs_and_is_reported(self):
        floor = [quad(0, 0, 40, 40), quad(18, -1, 22, 41, -0.0, material=0x2281)]
        # A full-width automatic strip (event 34) must be crossed and is reported.
        r = fp.plan(floor, (5, 20), (35, 20))
        self.assertEqual(r['crossed'], [34])
        # The goal's own event is not penalised or reported.
        self.assertEqual(fp.plan(floor, (5, 20), (35, 20), goal_event=34)['crossed'], [])
        # A partial strip is avoided when a detour is cheaper.
        part = [quad(0, 0, 40, 40), quad(18, 15, 22, 25, -0.0, material=0x2281)]
        self.assertEqual(fp.plan(part, (5, 20), (35, 20))['crossed'], [])

    def test_roof_layer_is_not_the_default_surface(self):
        ground, roof = quad(0, 0, 40, 10, 0.0), quad(10, 0, 20, 10, -30.0)
        g = fp.Grid([ground, roof])
        self.assertEqual(g.node(15, 5)[2], 0.0)
        self.assertEqual(g.node(15, 5, y=-29)[2], -30.0)
        self.assertIsNotNone(fp.plan([ground, roof], (2, 5), (38, 5)))

    def test_off_floor_start_is_an_error(self):
        with self.assertRaises(ValueError):
            fp.plan([quad(0, 0, 10, 10)], (50, 50), (5, 5))

if __name__ == '__main__':
    unittest.main()
