import pathlib
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/"tools"))
from analyze_nr_guides import analyze
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

if __name__=="__main__":
    unittest.main()
