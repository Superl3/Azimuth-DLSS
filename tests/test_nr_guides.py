import pathlib
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/"tools"))
from analyze_nr_guides import analyze, decode_ufloat
class Guides(unittest.TestCase):
    def test_groups(self):
        with tempfile.TemporaryDirectory() as root:
            p=pathlib.Path(root)
            (p/"f5-stencil-2x2-fmt41.raw").write_bytes(struct.pack("<4f",0,1,-2,255))
            (p/"f5-guidecb-64x1-fmt41.raw").write_bytes(struct.pack("<64f",*range(64)))
            (p/"f5-mv-2x2-fmt34.raw").write_bytes(struct.pack("<8e",0,0,1,0,0,1,0,0))
            (p/"f5-depth-2x2-fmt39.raw").write_bytes(struct.pack("<4f",1,.5,.25,0))
            g=analyze(p)["groups"][0]
            self.assertEqual(g["stencil_groups"][1]["invalid_reprojection"],1)
            self.assertEqual(g["stencil_groups"][1]["mean_mv_magnitude_px_default_scale"],2)
            self.assertEqual(g["candidate_cb_rows"]["adaptive_exposure"],[5,6,7,8])
            self.assertFalse(g["engine_semantics_verified"])
    def test_reject_cross_session_collision(self):
        with tempfile.TemporaryDirectory() as root:
            p=pathlib.Path(root)
            for name in ("a","b"):
                (p/name).mkdir()
                (p/name/"f1-depth-1x1-fmt39.raw").write_bytes(struct.pack("<f",1))
            with self.assertRaisesRegex(ValueError,"duplicate"):
                analyze(p)

    def test_temporal_direction(self):
        # R11G11B10 packed grayscale-ish ramp, identical y rows. Current
        # image samples previous one pixel to the right; +MV must win.
        w,h=16,4
        def pixel(x):
            a=(15<<6)+2*x
            b=(15<<5)+x
            return a|(a<<11)|(b<<22)
        self.assertEqual(decode_ufloat(15<<6,6),1)
        with tempfile.TemporaryDirectory() as root:
            p=pathlib.Path(root)
            for f,shift in ((100,0),(101,1)):
                (p/f"f{f}-color-{w}x{h}-fmt26.raw").write_bytes(struct.pack(f"<{w*h}I",*(pixel(min(x+shift,w-1)) for y in range(h) for x in range(w))))
                (p/f"f{f}-mv-{w}x{h}-fmt34.raw").write_bytes(struct.pack("<ee",1/w,0)*(w*h))
                (p/f"f{f}-depth-{w}x{h}-fmt39.raw").write_bytes(struct.pack("<f",.5)*(w*h))
                (p/f"f{f}-stencil-{w}x{h}-fmt41.raw").write_bytes(struct.pack("<f",2)*(w*h))
            pair=analyze(p,stride=1)["temporal_pairs"][0]
            self.assertGreater(pair["counts"]["common"],0)
            self.assertEqual(pair["metrics"]["current_to_previous"]["mae_log_luma"],0)
            self.assertGreater(pair["metrics"]["reverse"]["mae_log_luma"],0)
            self.assertGreater(pair["metrics"]["zero"]["mae_log_luma"],0)
            self.assertFalse(pair["validated"])
            (p/f"f101-color-{w}x{h}-fmt26.raw").rename(p/f"f103-color-{w}x{h}-fmt26.raw")
            self.assertIn("nonadjacent_resolves",[x["reason"] for x in analyze(p)["temporal_pairs"]])

if __name__=="__main__":
    unittest.main()
