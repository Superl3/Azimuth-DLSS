import json
import pathlib
import sys
import tempfile
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from run_fg_replay import build_contract, MASK

class Contract(unittest.TestCase):
    def fixture(self, root, gap=0, reset=0, missing=0):
        (root/"gpu-raw").mkdir()
        lines=[]; files=[]
        for idx, n in enumerate((10, 11+gap)):
            lines.extend([f"FG INPUT_CORE resolve={n} reset={reset if idx else 1} fresh=1 jitter=0 jitterX=0.0000000 jitterY=0.0000000 recording=50",
                          f"FG INPUT_FINAL resolve={n} final=1 hudless=1 hdr=1 actualPresent=0",
                          f"VISIBLE FINAL_GPU_COMPLETE resolve={n} target=0x1 batch=1",
                          f"PREVIEW GROUP_RESULT resolve={n} expected=0x{MASK:x} attempted=0x{MASK:x} saved=0x{MASK:x} postfxSeen=0x7 coreComplete=1"])
            for role,fmt in (("depth",39),("mv",34),("visiblefinal",24),("fghudless",24)):
                if missing and idx and role=="fghudless":
                    continue
                name=f"f{n}-{role}-320x200-fmt{fmt}.raw"; size=320*200*4
                (root/"gpu-raw"/name).write_bytes(bytes(size)); files.append(name)
                lines.append(f"PREVIEW FILE directory=azimuth_preview_1_2 file={name} bytes={size}")
        (root/"azimuth_dlaa.log").write_text("\n".join(lines)+"\n")
        (root/"capture.json").write_text(json.dumps(dict(fg_input_probe_requested=True,preview_complete=True,
            visible_sr_requested=True,sr_no_jitter_requested=True,visible_hdr_requested=True,hdr_live_requested=True,gpu_raw=files)))
    def test_success(self):
        with tempfile.TemporaryDirectory() as d:
            root=pathlib.Path(d);self.fixture(root)
            a=build_contract(root)
            self.assertEqual(a["first_resolve"],10)
            self.assertEqual(a["output"],(320,200))
            self.assertFalse(a["present_lineage_verified"])
    def test_refuses(self):
        for kwargs in ({"gap":1},{"reset":1},{"missing":1}):
            with self.subTest(kwargs=kwargs),tempfile.TemporaryDirectory() as d:
                root=pathlib.Path(d);self.fixture(root,**kwargs)
                with self.assertRaises(ValueError):build_contract(root)
if __name__=="__main__":
    unittest.main()
