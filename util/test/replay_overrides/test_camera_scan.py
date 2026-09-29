"""Discovery regressions without a GPU; fake bindings change at real action boundaries."""
import importlib.util
import pathlib
import struct
import sys
import types
import unittest

rd = types.ModuleType('riderduck')
rd.ShaderStage = types.SimpleNamespace(Vertex=0, Hull=1, Domain=2, Geometry=3, Pixel=4, Compute=5)
rd.ActionFlags = types.SimpleNamespace(Drawcall=1, Dispatch=2, Present=4)
rd.ResourceId = types.SimpleNamespace(Null=lambda: 0)
sys.modules['riderduck'] = rd
source = pathlib.Path(__file__).parents[3] / 'qriderduck/Resources/freecamera_hgrp.py'
sys.path.insert(0, str(source.parent))
spec = importlib.util.spec_from_file_location('camera', str(source))
camera = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera)


def buffer(x=2, jitter=0.001):
    view = [1., 0, 0, -x, 0, 1., 0, -3., 0, 0, 1., -4., 0, 0, 0, 1.]
    p = [1.5, 0, jitter, 0, 0, 2., 0, 0, 0, 0, -1.01, -.101, 0, 0, -1., 0]
    stable = list(p)
    stable[2] = 0
    relative = list(view)
    relative[3] = relative[7] = relative[11] = 0
    mats = {0: view, 64: camera.inverse(view), 128: p, 192: camera.inverse(p),
            256: camera.multiply(p, view), 320: camera.multiply(p, relative),
            384: camera.inverse(camera.multiply(p, view)),
            448: camera.multiply(stable, view), 512: camera.multiply(stable, relative),
            576: camera.inverse(camera.multiply(stable, view))}
    data = bytearray(720)
    for offset, matrix in mats.items():
        struct.pack_into('<16f', data, offset, *camera.transpose(matrix))
    struct.pack_into('<3f', data, 704, x, 3, 4)
    return bytes(data)


def action(eid, shader='scene', data=None, flags=1, slot=0, registers='cb0[32], cb0[44]'):
    return types.SimpleNamespace(eventId=eid, flags=flags, children=[], copyDestination=0,
                                 shader=shader, data=buffer() if data is None else data,
                                 slot=slot, code='dcl_constantbuffer cb0[100], immediateIndexed\n'
                                 '0: add r0, {}, r1\n'.format(registers))


class Controller:
    def __init__(self, actions):
        self.actions = actions
    def GetRootActions(self): return self.actions
    def GetTextures(self): return []
    def SetFrameEvent(self, eid, force): self.current = next(a for a in self.actions if a.eventId == eid)
    def GetPipelineState(self): return self
    def GetShaderReflection(self, stage):
        if stage != (5 if self.current.flags & 2 else 0): return None
        return types.SimpleNamespace(constantBlocks=[types.SimpleNamespace(fixedBindNumber=self.current.slot)])
    def GetShader(self, stage): return self.current.shader if self.GetShaderReflection(stage) else 0
    def GetConstantBlock(self, stage, block, index):
        return types.SimpleNamespace(descriptor=types.SimpleNamespace(resource=1, byteOffset=69632,
                                                                       byteSize=len(self.current.data)))
    def GetBufferData(self, resource, offset, size):
        assert offset == 69632  # Bound-range offsets must be preserved.
        return self.current.data[:size]
    def DisassembleShader(self, pipe, reflection, target): return self.current.code


def scan(actions):
    scanner = camera.Scanner(Controller(actions), actions[0].eventId)
    while True:
        result = scanner.step(1)
        if result['done']: return result


class DiscoveryTests(unittest.TestCase):
    def test_distinct_projection_variants(self):
        result = scan([action(7, registers='cb0[16], cb0[32], cb0[44]')])
        fields = result['config']['fields']
        variants = [f['projection'] for f in fields if 'projection' in f]
        self.assertEqual(len(variants), 2)
        self.assertFalse(camera.close(variants[0], variants[1], 1e-6, 0))
        self.assertFalse(result['partial'])

    def test_camera_switch_splits_interval(self):
        result = scan([action(10), action(30, data=buffer(99)), action(80)])
        self.assertEqual([(f['first_event'], f['last_event']) for f in result['config']['fields']
                          if f['semantic'] == 'CameraPosition'], [(10, 10), (80, 80)])

    def test_missing_binding_splits_interval(self):
        result = scan([action(1), action(2, data=b''), action(5)])
        self.assertEqual(len(result['config']['fields']), 4)

    def test_reordered_added_pass_and_compute(self):
        result = scan([action(73), action(74, shader='new', slot=3, registers='cb3[44]'),
                       action(101, shader='taa', flags=2, registers='cb0[32]'), action(150)])
        self.assertEqual({f['stage'] for f in result['config']['fields']}, {'VS', 'CS'})
        self.assertTrue(any(f['slot'] == 3 for f in result['config']['fields']))
        self.assertTrue(any(f['first_event'] == 73 and f['last_event'] == 150
                            for f in result['config']['fields']))

    def test_projection_changes_split_interval(self):
        result = scan([action(1, registers='cb0[16]'),
                       action(3, data=buffer(jitter=.1), registers='cb0[16]')])
        self.assertEqual(len(result['config']['fields']), 2)

    def test_unknown_layout_and_dynamic_index_are_partial(self):
        bad = bytearray(buffer())
        struct.pack_into('<f', bad, 256, 999)
        result = scan([action(1), action(2, data=bytes(bad)),
                       action(3, shader='dynamic', registers='cb0[r0.x + 32]')])
        self.assertTrue(result['partial'])
        self.assertIn('Dynamically', result['report'])
        self.assertEqual({f['last_event'] for f in result['config']['fields']}, {1})

    def test_history_reported_without_overwriting(self):
        result = scan([action(9, registers='cb0[32], cb0[44], cb0[81]')])
        self.assertTrue(result['partial'])
        self.assertTrue(all(f['offset'] < 720 for f in result['config']['fields']))

    def test_bad_seed(self):
        with self.assertRaises(ValueError): scan([action(9, data=b'\0' * 720)])
        with self.assertRaises(ValueError): camera.Scanner(Controller([action(1)]), 2)

    def test_declarations_are_not_reads(self):
        self.assertEqual(camera.registers('dcl_constantbuffer cb0[99]\n0: mov r0, cb0[32]', 0),
                         ({32}, False))


if __name__ == '__main__': unittest.main()
