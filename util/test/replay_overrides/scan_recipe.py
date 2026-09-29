"""Read-only recipe expansion against a capture, without changing any GUI instance."""
import json
import hashlib
import os
import sys
import time

runtime, capture, recipe_path, output = sys.argv[1:]
sys.path.insert(0, os.path.join(runtime, 'pymodules'))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../qriderduck/Resources')))
import riderduck as rd
from freecamera_recipe import RecipeScanner

with open(recipe_path, encoding='utf-8-sig') as f:
    recipe = json.load(f)
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(capture, '', None) == rd.ResultCode.Succeeded
status, r = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded
try:
    started = time.time()
    fingerprint = None
    if recipe.get('reference_event') or recipe.get('passes'):
        digest = hashlib.sha256()
        with open(capture, 'rb') as f:
            for block in iter(lambda: f.read(1024*1024), b''): digest.update(block)
        fingerprint = dict(size=os.path.getsize(capture), sha256=digest.hexdigest())
    scanner = RecipeScanner(r, recipe, fingerprint)
    while True:
        result = scanner.step(16)
        print('{} {}/{}'.format(result['phase'], result['progress'], result['total']), flush=True)
        if result['done']:
            break
    result['elapsed_seconds'] = time.time() - started
    with open(output, 'w') as f:
        json.dump(result, f, indent=2)
    print(result['report'])
finally:
    r.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
