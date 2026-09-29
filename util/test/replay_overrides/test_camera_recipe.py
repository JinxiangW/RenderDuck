import copy
import importlib.util
import pathlib
import struct
import sys
import types
import unittest

import test_camera_scan as fixture

source = pathlib.Path(__file__).parents[3] / 'qriderduck/Resources/freecamera_recipe.py'
spec = importlib.util.spec_from_file_location('recipe', str(source))
recipe_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe_module)
rd, maths = fixture.rd, fixture.camera


def recipe(reference=1):
    return dict(version=1, reference_event=reference, capture=dict(size=123,sha256='a'*64), row_major=True, row_vector=True, forward='-Z',
                camera=dict(view=dict(slot=0, offset=0), inverse_view=dict(slot=0, offset=64),
                            projection=dict(slot=0, offset=128)),
                layouts=[dict(slot=0, stages=['VS'], fields=[dict(semantic='View', offset=0),
                                                            dict(semantic='InverseView', offset=64)]),
                         dict(slot=7, stages=['VS'], fields=[dict(semantic='ModelView', offset=0,
                                                                 model_offset=192)])])


def action(eid, position=2, model_x=0):
    camera = fixture.buffer(position)
    view = maths.transpose(struct.unpack_from('<16f', camera, 0))
    model = list(maths.IDENTITY)
    model[3] = model_x
    raw = bytearray(256)
    struct.pack_into('<16f', raw, 0, *maths.transpose(maths.multiply(view, model)))
    struct.pack_into('<16f', raw, 192, *maths.transpose(model))
    return types.SimpleNamespace(eventId=eid, flags=rd.ActionFlags.Drawcall, children=[],
                                 copyDestination=0, bindings={0:camera, 7:bytes(raw)})


class Controller(fixture.Controller):
    def GetShaderReflection(self, stage):
        if stage != rd.ShaderStage.Vertex: return None
        return types.SimpleNamespace(constantBlocks=[types.SimpleNamespace(fixedBindNumber=s)
                                                      for s in self.current.bindings])
    def GetShader(self, stage): return 'shader' if stage == rd.ShaderStage.Vertex else 0
    def GetConstantBlock(self, stage, block, index):
        slot = list(self.current.bindings)[block]
        return types.SimpleNamespace(descriptor=types.SimpleNamespace(resource=slot+1, byteOffset=4096,
                                                                       byteSize=len(self.current.bindings[slot])))
    def GetBufferData(self, resource, offset, size):
        assert offset == 4096
        return self.current.bindings[resource-1][:size]
    def DisassembleShader(self, *args): return '0: add r0, cb0[0], cb0[4]\n1: mov r1, cb7[0]'


def run(actions, r=None):
    r = r or recipe(actions[0].eventId)
    scanner = recipe_module.RecipeScanner(Controller(actions), r, r.get('capture'))
    for _ in range(100):
        result = scanner.step(1)
        if result['done']: return result
    raise AssertionError('scan never completed')


class RecipeTests(unittest.TestCase):
    def test_world_up_defaults_and_explicit_axes(self):
        self.assertEqual(run([action(1)])['config']['world_up'], '+Y')
        for axis in ('+X','-X','+Y','-Y','+Z','-Z'):
            r=recipe();r['world_up']=axis
            self.assertEqual(run([action(1)],r)['config']['world_up'],axis)
        r=recipe();r['world_up']='Y'
        with self.assertRaisesRegex(ValueError,'world_up'): recipe_module.validate(r)
    def test_v2_passes_fingerprint_and_report(self):
        r=recipe();r.update(version=2,passes=[dict(first_event=1,last_event=1,mode='skip',reason='Fixture-only unused draw')],
                            capture=dict(size=123,sha256='a'*64))
        scanner=recipe_module.RecipeScanner(Controller([action(1)]),r,r['capture'])
        result=scanner.step(1)
        self.assertEqual(result['config']['passes'],r['passes'])
        self.assertEqual(result['pass_count'],1)
        self.assertIn('Fixture-only unused draw',result['report'])
        with self.assertRaisesRegex(ValueError,'fingerprint'):
            recipe_module.RecipeScanner(Controller([action(1)]),r,dict(size=124,sha256='a'*64))
        del r['capture']
        with self.assertRaises(ValueError): recipe_module.validate(r)

    def test_v2_requires_explicit_pass_list_and_valid_ranges(self):
        r=recipe(0);r['version']=2
        with self.assertRaises(ValueError): recipe_module.validate(r)
        r['passes']=[]
        self.assertTrue(run([action(1)],r)['done'])
        self.assertEqual(run([action(1)],r)['config']['passes'],[])
        r.update(reference_event=1,capture=dict(size=12,sha256='b'*64),
                 passes=[dict(first_event=1,last_event=2,mode='skip',reason='test')])
        with self.assertRaisesRegex(ValueError,'endpoints'):
            recipe_module.RecipeScanner(Controller([action(1)]),r,r['capture'])

    def test_legacy_event_hint_needs_fingerprint_but_portable_v1_still_runs(self):
        r=recipe();del r['capture']
        with self.assertRaisesRegex(ValueError,'capture.size'): recipe_module.validate(r)
        r.pop('reference_event')
        self.assertTrue(run([action(1)],r)['done'])

    def test_v2_bypass_overlap_rejected_and_legacy_passes_rejected(self):
        r=recipe();r['passes']=[]
        with self.assertRaises(ValueError): recipe_module.validate(r)
        r.update(version=2,capture=dict(size=12,sha256='b'*64),passes=[
            dict(first_event=1,last_event=3,mode='colour_bypass',reason='verified current colour',
                 colours=[dict(source='ResourceId::1',destination='ResourceId::2')]),
            dict(first_event=2,last_event=4,mode='skip',reason='overlap')])
        with self.assertRaisesRegex(ValueError,'overlap'): recipe_module.validate(r)

    def test_expands_actual_shaders_and_modelview(self):
        result = run([action(1, model_x=3), action(7, model_x=4)])
        fields = result['config']['fields']
        models = [f for f in fields if f['semantic']=='ModelView']
        self.assertEqual([(f['first_event'], f['last_event']) for f in models], [(1,1),(7,7)])
        self.assertAlmostEqual(models[0]['model'][3],3)
        self.assertAlmostEqual(models[1]['model'][3],4)
        self.assertEqual(len(fields),4)
        self.assertFalse(result['partial'])

    def test_other_camera_breaks_intervals(self):
        fields = run([action(1),action(4,position=99),action(8)])['config']['fields']
        self.assertEqual({(f['first_event'],f['last_event']) for f in fields}, {(1,1),(8,8)})

    def test_unbound_slot_breaks_intervals(self):
        middle=action(2);del middle.bindings[7]
        models=[f for f in run([action(1),middle,action(3)])['config']['fields'] if f['semantic']=='ModelView']
        self.assertEqual(len(models),2)

    def test_unique_camera_without_reference_event(self):
        self.assertTrue(run([action(4),action(8)],recipe(0))['done'])

    def test_ambiguous_reference_reports_candidates(self):
        with self.assertRaisesRegex(ValueError,'reference_event'):
            run([action(4),action(8,position=12)],recipe(0))

    def test_invalid_seed_and_layout_fail(self):
        with self.assertRaises(ValueError): run([action(1)],recipe(9))
        invalid=recipe();invalid['layouts'][1]['fields'][0].pop('model_offset')
        with self.assertRaises(ValueError): run([action(1)],invalid)
        invalid=recipe();invalid['camera']['view']['offset']=-1
        with self.assertRaises(ValueError): run([action(1)],invalid)

    def test_partial_is_explicit(self):
        r=recipe();r['unresolved']=['History matrices were not classified']
        result=run([action(1)],r)
        self.assertTrue(result['partial'])
        self.assertIn('History', result['report'])

    def test_recipe_is_data_only(self):
        r=recipe();r['name']="'); raise RuntimeError('not executed'); #"
        self.assertTrue(run([action(1)],r)['done'])

    def test_per_field_storage_and_locator_conventions(self):
        a=action(1,model_x=3)
        raw=bytearray(a.bindings[7])
        # Store ModelView transposed relative to the recipe defaults, retaining Model storage.
        values=struct.unpack_from('<16f',raw,0)
        struct.pack_into('<16f',raw,0,*maths.transpose(values))
        a.bindings[7]=bytes(raw)
        r=recipe()
        r['layouts'][1]['fields'][0].update(row_vector=False,model_row_vector=True)
        r['camera']['view']['stage']='VS'
        fields=run([a],r)['config']['fields']
        model=next(f for f in fields if f['semantic']=='ModelView')
        self.assertFalse(model['row_vector'])
        self.assertAlmostEqual(model['model'][3],3)

    def test_independent_projection_is_preserved(self):
        a=action(1)
        raw=bytearray(a.bindings[7])
        p=list(maths.IDENTITY);p[0]=2;p[5]=3
        struct.pack_into('<16f',raw,64,*maths.transpose(p))
        r=recipe()
        r['layouts'][1]['fields'].append(dict(semantic='Projection',offset=64,projection_offset=64))
        a.bindings[7]=bytes(raw)
        controller=Controller([a])
        controller.DisassembleShader=lambda *args:'0: add r0, cb7[0], cb7[4]'
        scanner=recipe_module.RecipeScanner(controller,r,r.get('capture'))
        result=scanner.step(1)
        projection=next(f for f in result['config']['fields'] if f['semantic']=='Projection')
        self.assertEqual(projection['projection'],p)

    def test_nonfinite_model_and_dynamic_indexing_are_not_patched(self):
        a=action(1)
        raw=bytearray(a.bindings[7]);struct.pack_into('<f',raw,0,float('nan'))
        a.bindings[7]=bytes(raw)
        self.assertFalse(any(f['semantic']=='ModelView' for f in run([a])['config']['fields']))
        controller=Controller([action(1)])
        controller.DisassembleShader=lambda *args:'0: add r0, cb0[0], cb7[r0.x + 1]'
        scanner=recipe_module.RecipeScanner(controller,recipe(),recipe()['capture'])
        result=scanner.step(1)
        self.assertTrue(result['partial'])
        self.assertIn('dynamic indexing',result['report'])


if __name__ == '__main__': unittest.main()
