#!/usr/bin/env python3
"""Summarize possible pure-HDR scale consumers; never arm experimental routing."""
import argparse
import collections
import json
import re
from pathlib import Path

HASH = r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{8}){3}'
LINE = re.compile(r'SR CONSUMER frame=(?P<frame>\d+) list=\S+ same=(?P<same>\d+) hash=(?P<ps>'+HASH+r') vs=(?P<vs>'+HASH+r') t(?P<reg>\d+) space(?P<space>\d+) root=(?P<root>\d+) matches=(?P<matches>\d+) target=(?P<target>\d+x\d+) fmt=(?P<fmt>\d+) vp=(?P<vp>\d+x\d+) pureCandidate=(?P<pure>\d+) stateRT=(?P<state>\d+) unchanged=(?P<unchanged>\d+) layout=(?P<layout>\d+) draw=(?P<vertices>\d+)/(?P<instances>\d+)')

def parse(text,output=None):
    groups={}
    for m in LINE.finditer(text):
        d=m.groupdict()
        key=(d['ps'].lower(),d['vs'].lower(),int(d['reg']),int(d['space']),d['target'])
        g=groups.setdefault(key,dict(ps=key[0],vs=key[1],register=key[2],space=key[3],target=key[4],observations=0,frames=set(),plausible=0,reasons=collections.Counter()))
        g['observations']+=1;g['frames'].add(int(d['frame']))
        checks={'different_list':d['same']=='1','multiple_scene_bindings':d['matches']=='1','not_r11_hdr':d['fmt']=='26','viewport_mismatch':d['vp']==d['target'],'not_pure_pso':d['pure']=='1','unknown_rt_state':d['state']=='1','source_changed':d['unchanged']=='1','incomplete_layout':d['layout']=='1','not_fullscreen_draw':d['vertices'] in ('3','6') and d['instances']=='1','target_mismatch':not output or d['target']==output,'unknown_shader_hash':d['ps'].replace('-','').strip('0') and d['vs'].replace('-','').strip('0')}
        bad=[k for k,v in checks.items() if not v]
        if not bad:g['plausible']+=1
        else:g['reasons'].update(bad)
    for g in groups.values():g['frames']=len(g['frames']);g['reasons']=dict(g['reasons'])
    return sorted(groups.values(),key=lambda g:(-g['plausible'],-g['frames'],-g['observations'],g['ps']))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);p.add_argument('--output',help='expected output size e.g. 3840x1600');p.add_argument('--json',action='store_true');a=p.parse_args()
    if a.output and not re.fullmatch(r'[1-9]\d*x[1-9]\d*',a.output):p.error('--output must be WxH')
    text=a.log.read_text(encoding='utf-8',errors='replace');rows=parse(text,a.output)
    if a.json:print(json.dumps(rows,indent=2));return
    print('Bound-SRV candidates (NOT proof of shader semantics or full route eligibility).')
    print('Resolve samples:',text.count('SR TRACE RESOLVE'),'consumer rows:',sum(g['observations'] for g in rows))
    for g in rows[:25]:
        print(f"\n{g['ps']}  VS {g['vs']}  t{g['register']} space{g['space']}  target {g['target']}")
        print(f"  plausibility {g['plausible']}/{g['observations']}  distinct frames={g['frames']}  rejection hints={g['reasons']}")
    if not any(g['plausible'] for g in rows):print('\nNo structurally plausible logged pure scale candidate. Capture the native post-resolve chain; do not force route.')
    else:print('\nFor any candidate, verify PIX shader semantics, full-frame UVs, absence of side effects, queue ordering and downstream HDR consumption before arming route. This tool never sets CONFIRMED=1.')

if __name__=='__main__':main()
