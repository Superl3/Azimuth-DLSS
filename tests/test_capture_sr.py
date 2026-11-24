import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from capture_sr import package, summarize, trace_env

TRACE="""SR TRACE ONLY: no DLAA/NGX dispatch or injected motion replay
SR observed swapchain: 3840x1600
SR TRACE RESOLVE frame=1 list=0x1 scene=2560x1067 format=26 evaluated=0
SR GEOMETRY color=2560x1067 depth=2560x1067 cfmt=26 dfmt=20 samples=1/1 vpCount=1 viewport=0.00,0.00,2560.00,1067.00 scCount=1 scissor=0,0,2560,1067
SR NATIVE VELOCITY target=1280x534 format=34 viewport=0.00,0.00,1280.00,534.00
"""

class CaptureTests(unittest.TestCase):
    def test_summary(self):
        d=summarize(TRACE+TRACE,"3840x1600")
        self.assertTrue(d["trace_mode_seen"])
        self.assertEqual(d["resolves_logged"],2)
        self.assertEqual(len(d["geometry"]),1)
        self.assertEqual(d["geometry"][0]["depth"],"2560x1067")
        self.assertEqual(d["native_velocity"][0]["target"],"1280x534")
        self.assertFalse(d["ngx_probe_evaluated"])
    def test_environment_isolation(self):
        with patch.dict(os.environ,{"AZIMUTH_SR_ROUTE_CONFIRMED":"1","azimuth_sr_mode":"route","OTHER":"yes"}):
            env=trace_env("trace","3840x1600","quality",4000)
            self.assertNotIn("AZIMUTH_SR_ROUTE_CONFIRMED",env)
            self.assertNotIn("azimuth_sr_mode",env)
            self.assertNotIn("AZIMUTH_SR_OUTPUT",env)
            self.assertEqual(env["AZIMUTH_SR_MODE"],"trace")
            self.assertEqual(env["OTHER"],"yes")
        self.assertEqual(trace_env("probe","3840x1600","quality",4000)["AZIMUTH_SR_OUTPUT"],"3840x1600")
    def test_package_and_wrong_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            game=Path(tmp);log=game/"azimuth_dlaa.log";log.write_text(TRACE)
            (game/"azimuth.exe").write_bytes(b"fake")
            dest=game/"out"
            target,_=package(log,game,dest,"native","trace","3840x1600","quality","native",0)
            with zipfile.ZipFile(target) as z:
                self.assertEqual(set(z.namelist()),{"capture.json","summary.json","azimuth_dlaa.log"})
                self.assertEqual(json.loads(z.read("capture.json"))["requested_render_setting"],"native")
                self.assertEqual(len(json.loads(z.read("capture.json"))["files_sha256"]["azimuth.exe"]),64)
            log.write_text("unrelated old log")
            with self.assertRaises(RuntimeError): package(log,game,dest,"low","trace","3840x1600","quality","low",0)

if __name__=="__main__": unittest.main()
