"""Headless integration of the bundled discovery script against a real capture."""
import importlib.util
import json
import os
import sys
import time

runtime, capture, source, output, seed = sys.argv[1:]
sys.path.insert(0, os.path.join(runtime, 'pymodules'))
import riderduck as rd
sys.path.insert(0, os.path.dirname(source))
spec = importlib.util.spec_from_file_location('freecamera_hgrp', source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(capture, '', None) == rd.ResultCode.Succeeded
status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded
try:
    started = time.time()
    if int(seed) == 0:
        scanner = None
        for action in module.flatten(controller.GetRootActions()):
            if not action.flags & rd.ActionFlags.Drawcall:
                continue
            try:
                scanner = module.Scanner(controller, action.eventId)
                print('Verified seed: {}'.format(action.eventId), flush=True)
                break
            except ValueError:
                pass
        assert scanner is not None, 'No verified HGRP seed in this capture'
    else:
        scanner = module.Scanner(controller, int(seed))
    while True:
        result = scanner.step(32)
        print('{}/{}'.format(result['progress'], result['total']), flush=True)
        if result['done']:
            break
    result['elapsed_seconds'] = time.time() - started
    with open(output, 'w') as stream:
        json.dump(result, stream, indent=2)
    print(result['report'])
finally:
    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
