"""Group observed output-sized draws. No inferred HUD-less classification."""
import re
from collections import defaultdict

HASH=r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{8}){3}"
PASS=re.compile(r"FG PASS resolve=(\d+) seq=(\d+) list=(\S+) target=(\S+) size=(\d+x\d+) fmt=(\d+) bbSnapshot=(\d+) blend=(\d+) depth=(\d+) dsv=(\d+) writeMask=(\d+) layout=(\d+) viewport=(\S+) draw=(\d+)/(\d+) indexed=(\d+) ps=("+HASH+r") vs=("+HASH+r")")
INPUT=re.compile(r"FG INPUT seq=(\d+) resource=(\S+) size=(\d+x\d+) fmt=(\d+) t(\d+) space(\d+) scene=(\d+) boundOnly=1")
CHAIN=re.compile(r"FG SWAPCHAIN slot=(\d+) chain=(\S+) queue=(\S+) size=(\d+x\d+) format=(\d+) buffers=(\d+) effect=(\d+) flags=(\d+) creationSnapshot=1")
def parse_fg(text):
    chains=[dict(zip(("slot","chain","queue","size","format","buffers","effect","flags"),m.groups())) for m in CHAIN.finditer(text)]
    byseq=defaultdict(list)
    for m in INPUT.finditer(text):
        seq,*v=m.groups()
        byseq[seq].append(dict(zip(("resource","size","format","register","space","scene"),v)))
    rows=[]
    groups={}
    for m in PASS.finditer(text):
        names=("resolve","seq","list","target","size","format","backbuffer_snapshot","blend","depth","dsv","write_mask","layout","viewport","vertices_or_indices","instances","indexed","ps","vs")
        row=dict(zip(names,m.groups()))
        for key in ("resolve","seq","format","backbuffer_snapshot","blend","depth","dsv","write_mask","layout","vertices_or_indices","instances","indexed"):
            row[key]=int(row[key])
        row["inputs"]=byseq.get(str(row["seq"]),[])
        rows.append(row)
        k=(row["ps"],row["vs"],row["size"],row["format"],row["blend"],row["depth"],row["backbuffer_snapshot"])
        g=groups.setdefault(k,dict(ps=k[0],vs=k[1],size=k[2],format=k[3],blend=k[4],depth=k[5],backbuffer_snapshot=k[6],observations=0,first_seq=row["seq"],last_seq=row["seq"],resolves=set()))
        g["observations"]+=1
        g["resolves"].add(row["resolve"])
        g["last_seq"]=row["seq"]
    for g in groups.values(): g["resolves"]=sorted(g["resolves"])
    return {"enabled":"FG TRACE ONLY" in text,
            "draw_count":len(rows),"truncated":"FG TRACE truncated" in text or "FG INPUT truncated" in text,
            "swapchain_creation_snapshots":chains,
            "groups":sorted(groups.values(),key=lambda g:g["first_seq"]),
            "passes":rows,
            "warning":"Resolve sequences and CPU recording order are not presentation or queue-execution order. Bound resources and creation-time backbuffer identities are candidates only. PIX verification required; no FG is enabled."}
