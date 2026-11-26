import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from analyze_sr_trace import parse
PS='12345678-11111111-22222222-33333333';VS='87654321-11111111-22222222-33333333'
ROW=f'SR CONSUMER frame=1 list=0x1 same=1 hash={PS} vs={VS} t7 space2 root=0 matches=1 target=3840x1600 fmt=26 vp=3840x1600 pureCandidate=1 stateRT=1 unchanged=1 layout=1 draw=3/1'
class TestTrace(unittest.TestCase):
    def test_group_and_reject(self):
        rows=parse(ROW+'\n'+ROW.replace('frame=1','frame=2')+'\n'+ROW.replace('same=1','same=0'), '3840x1600')
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['plausible'],2);self.assertEqual(rows[0]['observations'],3);self.assertEqual(rows[0]['frames'],2);self.assertEqual(rows[0]['reasons'],{'different_list':1})
    def test_output(self):self.assertEqual(parse(ROW,'2560x1067')[0]['plausible'],0)
    def test_empty(self):self.assertEqual(parse('irrelevant log'),[])
if __name__=='__main__':unittest.main()
