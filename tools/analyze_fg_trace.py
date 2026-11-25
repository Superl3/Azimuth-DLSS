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

# Exact observed pairs. This is a diagnostic chain, not an executable route.
FH2_POSTFX=("479be24a-12d3a1cc-5ae78b8d-e91827a4","2098e1f9-8e0d8b8e-7bdf6434-4ec0d391")
FH2_COPY=("a7840ad0-c7ff485a-baaba36a-f2b83b22","2b57aba8-08891572-ded78132-0c122cb6")
FH2_FINAL={
    ("577d52ef-933f3888-157f3b5e-e0935945","79d0ca68-15da8827-24fc6096-35dd7508"),
    ("a93721c4-5975a343-749f00dc-e74366c0","79d0ca68-15da8827-24fc6096-35dd7508"),
}

def analyze_fh2_postfx(rows):
    """Find same-list/same-resolve resource identities, never infer HUD semantics."""
    def pair(r): return r["ps"].lower(),r["vs"].lower()
    def fullscreen(r):
        return (r["vertices_or_indices"]==3 and r["instances"]==1 and
                not r["indexed"] and r["format"]==24 and not r["blend"] and
                not r["depth"] and not r["dsv"])
    def bound_t0(r,target):
        return any(x["register"]=="0" and x["space"]=="0" and x["resource"]==target
                   for x in r["inputs"])
    by_frame=defaultdict(list)
    for r in rows:
        by_frame[(r["resolve"],r["list"])].append(r)
    chains=[];starts=0;ambiguous=0
    for group in by_frame.values():
        group=sorted(group,key=lambda r:r["seq"])
        for i,a in enumerate(group):
            if pair(a)!=FH2_POSTFX or not fullscreen(a): continue
            starts+=1
            copies=[j for j in range(i+1,len(group))
                    if pair(group[j])==FH2_COPY and fullscreen(group[j])
                    and bound_t0(group[j],a["target"])]
            if len(copies)!=1:
                ambiguous+=len(copies)>1
                continue
            j=copies[0];b=group[j]
            finals=[k for k in range(j+1,len(group))
                    if pair(group[k]) in FH2_FINAL and fullscreen(group[k])
                    and bound_t0(group[k],b["target"])]
            if len(finals)!=1:
                ambiguous+=len(finals)>1
                continue
            c=group[finals[0]]
            intervening=[r for r in group[i+1:j] if r["target"]==a["target"]]
            blends=[r for r in intervening if r["blend"]]
            chains.append({"resolve":a["resolve"],"list":a["list"],
                "postfx_seq":a["seq"],"copy_seq":b["seq"],"final_seq":c["seq"],
                "final_ps":c["ps"],"scene_target":a["target"],"copy_target":b["target"],
                "final_target":c["target"],"sizes":[a["size"],b["size"],c["size"]],
                "scene_t0_identity":any(x["register"]=="0" and x["space"]=="0" and x["scene"]=="1" for x in a["inputs"]),
                "intervening_same_target_draws":len(intervening),
                "intervening_blend_draws":len(blends),
                "first_blend_seq":blends[0]["seq"] if blends else None,
                "blend_pairs":sorted({"/".join(pair(r)) for r in blends}),
                "layouts_complete":bool(a["layout"] and b["layout"] and c["layout"])})
    counts=[c["intervening_blend_draws"] for c in chains]
    return {"postfx_starts":starts,"matched_chains":len(chains),"ambiguous_chains":ambiguous,
            "blend_draw_range":[min(counts),max(counts)] if counts else None,
            "chains":chains,
            "warning":"Bound descriptor identities and CPU recording order only. Blended draws are candidate overlays, not proven HUD; final presentation and pre-HUD color require GPU capture. Never enable rendering from this analysis."}
