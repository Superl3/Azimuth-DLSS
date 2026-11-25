import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from analyze_debug_trace import parse_debug
from analyze_fg_trace import analyze_fh2_postfx,analyze_hdr_scene_copy,FH2_POSTFX,FH2_COPY,FH2_FINAL,HDR_FINAL_PS,SDR_FINAL_PS
from capture_sr import trace_env,summarize

class DebugTests(unittest.TestCase):
    def test_windows(self):
        text="""DEBUG READY: F8 starts/stops bounded SR+FG trace
DEBUG QUEUE_CREATE queue=0x1 type=0
DEBUG BEGIN id=1 firstResolveAfter=12 limit=8 timeoutMs=8000
DEBUG AUTO_BEGIN id=1 requestedResolve=10
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
        self.assertEqual(a["trigger"],"automatic")
        self.assertEqual(a["requested_resolve"],10)
        self.assertEqual(a["observed_resolves"],8)
        self.assertEqual(a["counts"]["copy_edges"],1)
        self.assertEqual(a["counts"]["native_velocity"],1)
        self.assertEqual(a["resolve_records"],1)
        self.assertFalse(b["complete"])
        self.assertEqual(b["trigger"],"manual")
        self.assertTrue(b["truncation"])
        self.assertEqual(data["renderdoc_observations"][0]["nearest_capture_id"],1)
        self.assertEqual(summarize(text,"3840x2160")["debug_capture"]["capture_count"],2)
    def test_fh2_postfx_chain(self):
        def draw(seq, pair, target, src=None, blend=0, other_list=None):
            return dict(seq=seq,resolve=9,list=other_list or "list1",ps=pair[0],vs=pair[1],
                        target=target,size="1920x1080",format=24,vertices_or_indices=3,
                        instances=1,indexed=0,blend=blend,depth=0,dsv=0,layout=0,
                        inputs=[] if src is None else [dict(register="0",space="0",resource=src,scene="0")])
        for final in FH2_FINAL:
            a=draw(1,FH2_POSTFX,"scene","hdr")
            a["inputs"][0]["scene"]="1"
            overlay=draw(2,("1","2"),"scene",blend=1)
            b=draw(3,FH2_COPY,"copy","scene")
            c=draw(4,final,"final","copy")
            result=analyze_fh2_postfx([a,overlay,b,c])
            self.assertEqual(result["matched_chains"],1)
            self.assertEqual(result["blend_draw_range"],[1,1])
            self.assertEqual(result["chains"][0]["first_blend_seq"],2)
            self.assertTrue(result["chains"][0]["scene_t0_identity"])
            self.assertFalse(result["chains"][0]["layouts_complete"])
            self.assertEqual(analyze_fh2_postfx([a,overlay,b,dict(c,list="list2")])["matched_chains"],0)
            self.assertEqual(analyze_fh2_postfx([a,b,dict(c,inputs=[])])["matched_chains"],0)
            self.assertEqual(analyze_fh2_postfx([a,b,dict(b,seq=5),c])["ambiguous_chains"],1)
    def test_hdr_preoverlay_copy(self):
        c={"resolve":9,"list":"cl1","postfx_seq":1,"first_blend_seq":2,"copy_seq":3,"final_seq":4,
           "final_ps":HDR_FINAL_PS,"scene_target":"scene","copy_target":"composite","final_target":"output",
           "sizes":["2560x1440","2560x1440","3840x2160"]}
        rows=[dict(seq=1,resolve=9,target="scene"),dict(seq=2,resolve=9,target="scene"),
              dict(seq=3,resolve=9,target="composite"),dict(seq=4,resolve=9,target="output",
              inputs=[dict(register="2",space="0",resource="prehud",size="2560x1440",format="24")])]
        draw=lambda n:f"FG PASS resolve=9 seq={n} list=cl1"
        copy="DEBUG COPY id=1 resolve=9 list=cl1 kind=region src=scene size=2560x1440 fmt=23 dst=prehud size=2560x1440 fmt=24"
        lines=[draw(1),copy,draw(2),draw(3),draw(4)]
        def report(lines,rows=rows,chain=c):
            return analyze_hdr_scene_copy(lines,rows,[chain])
        result=report(lines)
        self.assertEqual(result["observed_preoverlay_copies"],1)
        self.assertEqual(result["scale_directions"],{"upscale":1})
        self.assertEqual(report([draw(1),draw(2),copy,draw(3),draw(4)])["observed_preoverlay_copies"],0)
        self.assertEqual(report([draw(1),copy.replace("list=cl1","list=cl2"),draw(2),draw(3),draw(4)])["observed_preoverlay_copies"],0)
        self.assertEqual(report([draw(1),copy,draw(2),copy,draw(3),draw(4)])["observed_preoverlay_copies"],0)
        self.assertEqual(report(lines,[*rows[:3],dict(rows[3],inputs=[])])["observed_preoverlay_copies"],0)
        self.assertEqual(report(lines,chain=dict(c,final_ps=SDR_FINAL_PS,sizes=["2560x1440","2560x1440","1920x1080"]))["scale_directions"],{"downscale":1})
        self.assertEqual(report(lines,chain=dict(c,final_ps=SDR_FINAL_PS))["hdr_chains"],0)
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
