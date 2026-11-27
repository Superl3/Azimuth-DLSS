import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_streamline_live import analyze

def frame(resolve, packet, token, reset, count, generation=3):
    f = f"resolve={resolve} packet={packet} generation={generation}"
    return [
        f"FG FINAL_RECORD {f} nativeRecording=7 cookie=100 hudless=1",
        *[f"FG STAGE_SUBMIT {f} stage={s} nativeRecording=7 submit=8 queue=0x1" for s in (2,3,4)],
        f"FG COPY resolve={resolve} generation={generation} srcCookie=100 dstCookie=200 nativeRecording=9 submit=10 queue=0x1 proven=1",
        f"FG PRESENT_TAG call={resolve} {f} token={token} reset={reset} mode=1 bufferCookie=200 transferRecording=9 submit=10 queue=0x1 result=0",
        f"FG PRESENT_RESULT call={resolve} resolve={resolve} token={token} reset={reset} mode=1 hr=00000000 sdkResult=0 sdkStatus=0 framesSinceQuery={count} consumptionValue=1",
    ]

class Evidence(unittest.TestCase):
    def test_reset_and_next(self):
        r=analyze(frame(1,1,1,1,1)+frame(2,2,2,0,2))
        self.assertEqual(r["valid_lineage_and_sdk"],2)
        self.assertEqual(r["sdk_multiple_frame_candidates"],1)
        self.assertFalse(r["actual_display_proof"])
    def test_wrong_copy(self):
        f=frame(1,1,1,1,1)
        f[4]=f[4].replace("proven=1","proven=0")
        self.assertEqual(analyze(f)["valid_lineage_and_sdk"],0)
    def test_wrong_queue(self):
        f=frame(1,1,1,1,1)
        f[4]=f[4].replace("queue=0x1","queue=0x2")
        self.assertEqual(analyze(f)["valid_lineage_and_sdk"],0)
    def test_reset_gap(self):
        r=analyze(frame(1,1,1,1,1)+frame(3,3,3,1,2))
        self.assertEqual(r["sdk_multiple_frame_candidates"],0)
    def test_sdk_count_not_proof(self):
        r=analyze(frame(1,1,1,1,2))
        self.assertEqual(r["sdk_multiple_frame_candidates"],0)
        self.assertFalse(r["actual_display_proof"])
if __name__=="__main__":
    unittest.main()
