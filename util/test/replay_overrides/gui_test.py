# Run via qriderduck --python gui_test.py fixture_capture.rdc.
import ctypes
import os

out = os.environ['REPLAY_OVERRIDE_TEST_OUTPUT']
# Do not persist this test instance's layout, recent files or other UI preferences.
pyriderduck.Config().Close()
dll = ctypes.CDLL(os.path.join(out, 'gui_test.dll'))
dll.ScheduleReplayOverrideTest.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
dll.ScheduleReplayOverrideTest.restype = None
dll.ScheduleReplayOverrideTest(int(pyriderduck.this), out.encode('utf-8'))
