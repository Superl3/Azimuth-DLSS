import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from analyze_debug_trace import parse_debug
from capture_sr import trace_env,summarize

class DebugTests(unittest.TestCase):
    def test_windows(self):
        text="""DEBUG READY: F8 starts/stops bounded SR+FG trace
DEBUG QUEUE_CREATE queue=0x1 type=0
DEBUG BEGIN id=1 firstResolveAfter=12 limit=8 timeoutMs=8000
DEBUG ROLE id=1 resolve=12 role=2 list=0x2
SR NATIVE VELOCITY target=1280x720 format=34 viewport=0.00,0.00,1280.00,720.00
SR TRACE RESOLVE frame=13 list=0x2 scene=2560x1440 format=26 evaluated=0
DEBUG COPY id=1 resolve=13 list=0x2 kind=resource src=0x3 size=2560x1440 fmt=26 dst=0x4 size=2560x1440 fmt=26
DEBUG END id=1 reason=resolve_limit resolves=8 lastResolve=21
DEBUG RENDERDOC completed newCaptures=2
DEBUG BEGIN id=2 firstResolveAfter=50 limit=8 timeoutMs=8000
DEBUG TRUNCATED id=2 section=aux
"""
        data=parse_debug(text,"3840x2160")
        self.assertTrue(data["debug_ready"])
        self.assertEqual(data["capture_count"],2)
        a,b=data["captures"]
        self.assertTrue(a["complete"])
        self.assertEqual(a["observed_resolves"],8)
        self.assertEqual(a["counts"]["copy_edges"],1)
        self.assertEqual(a["counts"]["native_velocity"],1)
        self.assertEqual(a["resolve_records"],1)
        self.assertFalse(b["complete"])
        self.assertTrue(b["truncation"])
        self.assertEqual(data["renderdoc_observations"][0]["nearest_capture_id"],1)
        self.assertEqual(summarize(text,"3840x2160")["debug_capture"]["capture_count"],2)
    def test_env(self):
        with patch.dict(os.environ,{"AZIMUTH_DEBUG_RENDERDOC":"1","AZIMUTH_SR_ROUTE_CONFIRMED":"1","AZIMUTH_FG_TRACE":"1"}):
            env=trace_env("debug","3840x1600","quality",4000)
            self.assertEqual(env["AZIMUTH_SR_MODE"],"debug")
            self.assertEqual(env["AZIMUTH_DEBUG_RENDERDOC"],"0")
            self.assertNotIn("AZIMUTH_SR_ROUTE_CONFIRMED",env)
            self.assertNotIn("AZIMUTH_FG_TRACE",env)
            self.assertEqual(trace_env("debug","3840x1600","quality",4000,renderdoc=True)["AZIMUTH_DEBUG_RENDERDOC"],"1")
            with self.assertRaises(ValueError): trace_env("trace","3840x1600","quality",4000,renderdoc=True)

if __name__=="__main__":unittest.main()
