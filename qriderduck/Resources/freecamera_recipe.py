"""Data-only camera recipes. The agent supplies layout/meaning, not expanded replay patches."""
import json
import math
import re
import struct

import riderduck as rd
from freecamera_common import STAGES, IDENTITY, close, transpose, multiply, inverse, registers, flatten

SEMANTICS = {'View', 'InverseView', 'Projection', 'InverseProjection', 'ViewProjection',
             'InverseViewProjection', 'RelativeViewProjection', 'InverseRelativeViewProjection',
             'CameraPosition', 'ModelView'}


def uint(value, name, limit=65535):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= limit:
        raise ValueError('{} must be an integer in [0, {}]'.format(name, limit))
    return value


def validate(recipe):
    if not isinstance(recipe, dict) or type(recipe.get('version')) is not int or recipe['version'] not in (1, 2):
        raise ValueError('Recipe version must be 1 or 2')
    passes = recipe.get('passes', [])
    if recipe['version'] == 1 and 'passes' in recipe:
        raise ValueError('Pass rules require recipe version 2')
    if recipe['version'] == 2 and ('passes' not in recipe or not isinstance(passes, list)):
        raise ValueError('Version 2 requires an explicit passes list (empty means no bypass)')
    if len(passes) > 1024:
        raise ValueError('At most 1024 pass rules are supported')
    if recipe.get('reference_event') or passes:
        capture = recipe.get('capture', {})
        if (not isinstance(capture, dict) or type(capture.get('size')) is not int or
                capture['size'] <= 0 or not isinstance(capture.get('sha256'), str) or
                not re.fullmatch('[0-9a-fA-F]{64}', capture['sha256'])):
            raise ValueError('Capture-specific recipes require capture.size and complete capture.sha256')
    for index, rule in enumerate(passes):
        if not isinstance(rule, dict) or rule.get('mode') not in ('skip', 'colour_bypass'):
            raise ValueError('Pass mode must be skip or colour_bypass')
        first = uint(rule.get('first_event'), 'pass.first_event', 0xffffffff)
        last = uint(rule.get('last_event'), 'pass.last_event', 0xffffffff)
        if first == 0 or last < first or not isinstance(rule.get('reason'), str) or not rule['reason'].strip():
            raise ValueError('Pass needs a valid inclusive event range and an analysis reason')
        for previous in passes[:index]:
            if (rule['mode'] == 'colour_bypass' or previous['mode'] == 'colour_bypass') and first <= previous['last_event'] and previous['first_event'] <= last:
                raise ValueError('Colour bypass cannot overlap another pass rule')
        colours = rule.get('colours', [])
        if not isinstance(colours, list) or (rule['mode'] == 'colour_bypass' and not colours) or (rule['mode'] == 'skip' and colours):
            raise ValueError('Only colour_bypass accepts nonempty colours mappings')
        for colour in colours:
            if not isinstance(colour, dict): raise ValueError('Colour mapping must be an object')
            for direction in ('source', 'destination'):
                if not isinstance(colour.get(direction), str) or not re.fullmatch(r'ResourceId::[1-9][0-9]*', colour[direction]):
                    raise ValueError('Colour mapping needs exact texture ResourceId strings')
                for part in ('mip', 'slice', 'format'):
                    uint(colour.get(direction + '_' + part, 0), direction + '_' + part, 0xffffffff)
    for key in ('row_major', 'row_vector'):
        if key not in recipe or not isinstance(recipe[key], bool):
            raise ValueError('Recipe must explicitly specify ' + key)
    if recipe.get('forward') not in ('-Z', '+Z'):
        raise ValueError('Recipe forward must be -Z or +Z')
    if recipe.get('world_up', '+Y') not in ('+X', '-X', '+Y', '-Y', '+Z', '-Z'):
        raise ValueError('Recipe world_up must be +X, -X, +Y, -Y, +Z or -Z')
    uint(recipe.get('reference_event', 0), 'reference_event', 0xffffffff)
    camera = recipe.get('camera', {})
    if not isinstance(camera, dict):
        raise ValueError('camera must contain matrix locators')
    for name in ('view', 'projection', 'inverse_view'):
        locator = camera.get(name)
        if name == 'inverse_view' and locator is None:
            continue
        if not isinstance(locator, dict):
            raise ValueError('camera.{} needs slot and offset'.format(name))
        uint(locator.get('slot'), name + '.slot', 13)
        uint(locator.get('offset'), name + '.offset', 65536 - 64)
        if 'stage' in locator and locator['stage'] not in {label for _, label in STAGES}:
            raise ValueError('Invalid camera locator stage')
        for convention in ('row_major', 'row_vector'):
            if convention in locator and not isinstance(locator[convention], bool):
                raise ValueError('Matrix conventions must be booleans')
    layouts = recipe.get('layouts')
    if not isinstance(layouts, list) or not 1 <= len(layouts) <= 64:
        raise ValueError('Recipe needs 1 to 64 layouts')
    stages = {label for _, label in STAGES}
    for layout in layouts:
        if not isinstance(layout, dict):
            raise ValueError('Each layout must be an object')
        uint(layout.get('slot'), 'layout.slot', 13)
        if (not isinstance(layout.get('stages'), list) or not layout['stages'] or
                not all(isinstance(s, str) and s in stages for s in layout['stages'])):
            raise ValueError('Each layout needs explicit stages: VS/HS/DS/GS/PS/CS')
        fields = layout.get('fields')
        if not isinstance(fields, list) or not 1 <= len(fields) <= 64:
            raise ValueError('Each layout needs 1 to 64 fields')
        for field in fields:
            if not isinstance(field, dict):
                raise ValueError('Each field must be an object')
            semantic = field.get('semantic')
            if not isinstance(semantic, str) or semantic not in SEMANTICS:
                raise ValueError('Unsupported field semantic: ' + str(semantic))
            for convention in ('row_major', 'row_vector', 'model_row_major', 'model_row_vector'):
                if convention in field and not isinstance(field[convention], bool):
                    raise ValueError('Field conventions must be booleans')
            uint(field.get('offset'), 'field.offset', 65536 - (12 if semantic == 'CameraPosition' else 64))
            if 'projection_offset' in field:
                uint(field['projection_offset'], 'projection_offset', 65536 - 64)
            if semantic == 'ModelView':
                uint(field.get('model_offset'), 'ModelView.model_offset', 65536 - 64)
        # Projection alone cannot identify which camera a buffer belongs to.
        if not any(f['semantic'] not in ('Projection', 'InverseProjection') for f in fields):
            raise ValueError('A layout needs a view-dependent field to identify the camera')
    unresolved = recipe.get('unresolved', [])
    if not isinstance(unresolved, list) or not all(isinstance(s, str) for s in unresolved):
        raise ValueError('unresolved must be a list of explanatory strings')
    return recipe


class RecipeScanner:
    def __init__(self, controller, recipe, fingerprint=None):
        self.r = controller
        self.recipe = validate(recipe)
        if recipe.get('reference_event') or recipe.get('passes'):
            expected = dict(recipe['capture'])
            expected['sha256'] = expected['sha256'].lower()
            if expected != fingerprint:
                raise ValueError('Recipe fingerprint does not match the current RDC; reanalyse this capture')
        self.all_actions = list(flatten(controller.GetRootActions()))
        self.passes = recipe.get('passes', [])
        events = {a.eventId for a in self.all_actions}
        textures = {str(t.resourceId): t for t in controller.GetTextures()}
        for rule in self.passes:
            if rule['first_event'] not in events or rule['last_event'] not in events:
                raise ValueError('Pass endpoints must identify existing actions in this capture')
            for colour in rule.get('colours', []):
                for direction in ('source', 'destination'):
                    texture = textures.get(colour[direction])
                    if texture is None or texture.msSamp != 1 or texture.dimension != 2:
                        raise ValueError('Colour bypass requires an existing single-sample 2D texture')
                    if colour.get(direction + '_mip', 0) >= texture.mips or colour.get(direction + '_slice', 0) >= texture.arraysize:
                        raise ValueError('Colour bypass mip/slice exceeds the texture')
        self.actions = [a for a in flatten(controller.GetRootActions()) if a.flags &
                        (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)]
        self.actions_by_id = {a.eventId: a for a in self.actions}
        self.index = 0
        self.seed = None
        self.candidates = {}
        self.shader_cache = {}
        self.fields = []
        self.active = {}
        self.matches = 0
        self.rejected = 0
        self.issues = set()
        self.reads = {}
        self.buffer_reads = {}
        reference = self.recipe.get('reference_event', 0)
        if reference:
            if reference not in self.actions_by_id:
                raise ValueError('reference_event must identify a draw or dispatch in this capture')
            self.move(self.actions_by_id[reference])
            candidates = self.find_cameras()
            if not candidates or any(not close(c['view'], candidates[0]['view'], 1e-5, 1e-6) for c in candidates[1:]):
                raise ValueError('Reference event has no unique camera matching the recipe')
            self.seed = candidates[0]
        textures = {str(t.resourceId) for t in controller.GetTextures()}
        presents = [a for a in flatten(controller.GetRootActions()) if a.flags & rd.ActionFlags.Present
                    and str(a.copyDestination) in textures]
        self.output = presents[-1] if presents and len({str(a.copyDestination) for a in presents}) == 1 else None

    def move(self, action):
        self.r.SetFrameEvent(action.eventId, True)
        self.pipe = self.r.GetPipelineState()
        compute = bool(action.flags & rd.ActionFlags.Dispatch)
        self.stages = [(s, label) for s, label in STAGES if (label == 'CS') == compute]
        self.reads = {}
        self.buffer_reads = {}

    def binding(self, stage, slot):
        key = int(stage), slot
        if key not in self.reads:
            reflection = self.pipe.GetShaderReflection(stage)
            self.reads[key] = None
            if reflection:
                for i, block in enumerate(reflection.constantBlocks):
                    if block.fixedBindNumber != slot:
                        continue
                    d = self.pipe.GetConstantBlock(stage, i, 0).descriptor
                    if d.resource != rd.ResourceId.Null() and d.byteSize:
                        # Only this event's binding is cached; later uploads may change the contents.
                        source = str(d.resource), d.byteOffset, min(d.byteSize, 65536)
                        if source not in self.buffer_reads:
                            self.buffer_reads[source] = bytes(self.r.GetBufferData(d.resource, d.byteOffset, source[2]))
                        raw = self.buffer_reads[source]
                        self.reads[key] = raw, reflection
                    break
        return self.reads[key]

    def matrix(self, data, offset, settings=None):
        settings = settings or {}
        if offset + 64 > len(data):
            raise ValueError('Matrix exceeds the actual bound Buffer data')
        values = list(struct.unpack_from('<16f', data, offset))
        if not all(math.isfinite(x) for x in values):
            raise ValueError('Matrix contains non-finite values')
        if not settings.get('row_major', self.recipe['row_major']):
            values = transpose(values)
        if settings.get('row_vector', self.recipe['row_vector']):
            values = transpose(values)
        return values

    def find_cameras(self):
        found = []
        locators = self.recipe['camera']
        for stage, _ in self.stages:
            try:
                values = {}
                for name, locator in locators.items():
                    if name not in ('view', 'projection', 'inverse_view'):
                        continue
                    source_stage = next((s for s, label in self.stages if label == locator.get('stage')), stage)
                    if 'stage' in locator and not any(label == locator['stage'] for _, label in self.stages):
                        raise ValueError('Camera locator stage is inactive at this event')
                    binding = self.binding(source_stage, locator['slot'])
                    if binding is None:
                        raise ValueError('Camera binding missing')
                    values[name] = self.matrix(binding[0], locator['offset'], locator)
                view, proj = values['view'], values['projection']
                inv = inverse(view)
                if not close(view[12:16], [0, 0, 0, 1], 1e-4):
                    continue
                rotation = [view[r * 4 + c] if r < 3 and c < 3 else float(r == c)
                            for r in range(4) for c in range(4)]
                if not close(multiply(rotation, transpose(rotation)), IDENTITY, 0.002):
                    continue
                if 'inverse_view' in values and not close(multiply(view, values['inverse_view']), IDENTITY):
                    continue
                inverse(proj)
                found.append(dict(view=view, projection=proj, inverse_view=inv))
            except (ValueError, KeyError):
                continue
        return found

    def expected(self, semantic, projection):
        view, pose = self.seed['view'], self.seed['inverse_view']
        relative = list(view)
        relative[3] = relative[7] = relative[11] = 0
        return {'View': lambda: view, 'InverseView': lambda: pose,
                'Projection': lambda: projection, 'InverseProjection': lambda: inverse(projection),
                'ViewProjection': lambda: multiply(projection, view),
                'InverseViewProjection': lambda: inverse(multiply(projection, view)),
                'RelativeViewProjection': lambda: multiply(projection, relative),
                'InverseRelativeViewProjection': lambda: inverse(multiply(projection, relative))}[semantic]()

    def scan_event(self, action):
        for layout_index, layout in enumerate(self.recipe['layouts']):
            for stage, label in self.stages:
                if label not in layout['stages']:
                    continue
                shader = str(self.pipe.GetShader(stage))
                key = layout_index, label, shader, layout['slot']
                bound = self.binding(stage, layout['slot'])
                fields = []
                try:
                    if bound is None:
                        raise ValueError('unbound')
                    raw, reflection = bound
                    cachekey = label, shader, layout['slot']
                    if cachekey not in self.shader_cache:
                        code = self.r.DisassembleShader(rd.ResourceId.Null(), reflection, 'DXBC')
                        self.shader_cache[cachekey] = registers(code, layout['slot'])
                    used, dynamic = self.shader_cache[cachekey]
                    if dynamic:
                        self.issues.add('{} {} b{} has dynamic indexing'.format(label, shader, layout['slot']))
                        raise ValueError('dynamic indexing')
                    # Validate the declared layout even if this particular shader reads only one field.
                    for definition in layout['fields']:
                        semantic, offset = definition['semantic'], definition['offset']
                        proj = self.seed['projection']
                        if 'projection_offset' in definition:
                            proj = self.matrix(raw, definition['projection_offset'], definition)
                            inverse(proj)
                        model = None
                        if semantic == 'CameraPosition':
                            if offset + 12 > len(raw): raise ValueError('position out of range')
                            actual = struct.unpack_from('<3f', raw, offset)
                            expected = [self.seed['inverse_view'][i] for i in (3, 7, 11)]
                        elif semantic == 'ModelView':
                            actual = self.matrix(raw, offset, definition)
                            model_settings = dict(definition)
                            for convention in ('row_major', 'row_vector'):
                                if 'model_' + convention in definition:
                                    model_settings[convention] = definition['model_' + convention]
                            reference_model = self.matrix(raw, definition['model_offset'], model_settings)
                            expected = multiply(self.seed['view'], reference_model)
                            # Recover from the observed ModelView to retain capture-specific rounding.
                            model = multiply(self.seed['inverse_view'], actual)
                            if actual == self.seed['view']: model = list(IDENTITY)
                            try:
                                inverse(model)
                            except ValueError:
                                self.issues.add('EID {} {}: singular ModelView cannot be expanded'.format(action.eventId, label))
                                raise
                        else:
                            actual = self.matrix(raw, offset, definition)
                            expected = self.expected(semantic, proj)
                        if not close(actual, expected):
                            raise ValueError('layout/camera mismatch')
                        size = 12 if semantic == 'CameraPosition' else 64
                        field_regs = set(range(offset // 16, (offset + size - 1) // 16 + 1))
                        if not used.intersection(field_regs):
                            continue
                        field = dict(semantic=semantic, shader=shader, stage=label,
                                     slot=layout['slot'], offset=offset,
                                     row_major=definition.get('row_major', self.recipe['row_major']),
                                     row_vector=definition.get('row_vector', self.recipe['row_vector']))
                        if model is not None: field['model'] = model
                        if semantic in ('Projection','InverseProjection','ViewProjection','InverseViewProjection',
                                        'RelativeViewProjection','InverseRelativeViewProjection'):
                            field['projection'] = proj
                        fields.append(field)
                except (ValueError, struct.error):
                    if bound: self.rejected += 1
                    fields = []
                signature = json.dumps(fields, sort_keys=True) if fields else None
                previous = self.active.get(key)
                if signature:
                    self.matches += 1
                    if previous and previous[0] == signature:
                        for field in previous[1]: field['last_event'] = action.eventId
                    else:
                        for field in fields: field.update(first_event=action.eventId, last_event=action.eventId)
                        self.fields.extend(fields)
                        self.active[key] = signature, fields
                else:
                    self.active.pop(key, None)

    def step(self, count=16):
        discovery = self.seed is None
        for action in self.actions[self.index:self.index + count]:
            self.move(action)
            if discovery:
                for candidate in self.find_cameras():
                    key = tuple(candidate['view'])
                    if key not in self.candidates:
                        self.candidates[key] = candidate, action.eventId
            else:
                self.scan_event(action)
            self.index += 1
        done = self.index == len(self.actions)
        if discovery and done:
            if len(self.candidates) != 1:
                examples = ', '.join(str(v[1]) for v in list(self.candidates.values())[:12])
                raise ValueError('Found {} candidate views. Agent must set reference_event; candidates: {}'.format(
                    len(self.candidates), examples or 'none'))
            self.seed = next(iter(self.candidates.values()))[0]
            self.index = 0
            done = False
        result = dict(done=done, progress=self.index, total=len(self.actions),
                      phase='Finding reference camera' if discovery else 'Expanding recipe fields')
        if done:
            if not self.fields:
                raise ValueError('No recipe fields were verified; configuration unchanged')
            unresolved = self.recipe.get('unresolved', [])
            config = dict(view=self.seed['view'], projection=self.seed['projection'], fields=self.fields,
                          forward=self.recipe['forward'], world_up=self.recipe.get('world_up', '+Y'),
                          enabled=True, show=False, recipe=self.recipe)
            if self.recipe['version'] == 2:
                config['passes'] = self.passes
            if self.output:
                config.update(output_rid=str(self.output.copyDestination), flip_y=False)
                result['event_id'] = self.output.eventId
            result.update(config=config, partial=bool(self.issues or unresolved),
                          report='{}: {} verified fields, {} matched bindings; {} mismatching bindings excluded.\n'
                                 'Coverage is limited to the fields declared in this recipe.\n{}'.format(
                                     self.recipe.get('name', 'Camera recipe'), len(self.fields), self.matches,
                                     self.rejected, '\n'.join(unresolved + sorted(self.issues))))
            if not self.output: result['report'] += '\nNo unique Present output; current display retained.'
            result['pass_count'] = len(self.passes)
            for rule in self.passes:
                result['report'] += '\n{} EID {}-{}: {}'.format(rule['mode'], rule['first_event'], rule['last_event'], rule['reason'])
                for mapping in rule.get('colours', []):
                    result['report'] += '\n  {} -> {}'.format(mapping['source'], mapping['destination'])
        return result
