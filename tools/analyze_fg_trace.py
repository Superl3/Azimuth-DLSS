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


# Actual final shader assembly identifies these as different color paths,
# not simply aspect-ratio variants. Presence at PSO creation is not execution.
HDR_FINAL_PS="577d52ef-933f3888-157f3b5e-e0935945"
SDR_FINAL_PS="a93721c4-5975a343-749f00dc-e74366c0"
PASS_ORDER=re.compile(r"^FG PASS resolve=(\d+) seq=(\d+) list=(\S+)")
COPY_ORDER=re.compile(r"^DEBUG COPY id=\d+ resolve=(\d+) list=(\S+) kind=(\S+) src=(\S+) size=(\d+x\d+) fmt=(\d+) dst=(\S+) size=(\d+x\d+) fmt=(\d+)")

def _scale_direction(source,dest):
    a=tuple(map(int,source.split("x")))
    b=tuple(map(int,dest.split("x")))
    if a==b: return "same_size"
    if a[0]*b[1]!=a[1]*b[0]: return "aspect_changed"
    return "upscale" if b[0]>a[0] else "downscale"

def analyze_hdr_scene_copy(lines,rows,chains):
    """Require an observed same-list copy of pre-overlay color to final t2.
    These are CPU recording identities, never proof of GPU pixels or lifetime.
    A CopyTextureRegion log does not describe the region or subresource.
    """
    positions={}
    duplicate=set()
    copies=defaultdict(list)
    byseq={}
    for r in rows:
        byseq[r["seq"]]=r
    for pos,line in enumerate(lines):
        m=PASS_ORDER.match(line)
        if m:
            seq=int(m[2])
            if seq in positions: duplicate.add(seq)
            positions[seq]=pos
        m=COPY_ORDER.match(line)
        if m:
            frame,cl,kind,src,ss,sf,dst,ds,df=m.groups()
            copies[int(frame)].append((pos,cl,kind,src,ss,int(sf),dst,ds,int(df)))
    observations=[]
    for c in chains:
        mode="hdr_pq_candidate" if c["final_ps"].lower()==HDR_FINAL_PS else "sdr_curve_candidate"
        entry=dict(resolve=c["resolve"],list=c["list"],final_ps=c["final_ps"],
                   path=mode,source_size=c["sizes"][0],final_size=c["sizes"][-1],
                   scale_direction=_scale_direction(c["sizes"][0],c["sizes"][-1]),
                   status="not_hdr" if mode=="sdr_curve_candidate" else "unverified")
        observations.append(entry)
        if mode!="hdr_pq_candidate":continue
        ids=[c["postfx_seq"],c["copy_seq"],c["final_seq"],c["first_blend_seq"]]
        if any(i is None or i not in positions or i in duplicate or i not in byseq for i in ids):
            entry["status"]="missing_or_ambiguous_event_order"
            continue
        start,post_copy,final,blend=(positions[i] for i in ids)
        if not start<blend<post_copy<final:
            entry["status"]="invalid_event_order"
            continue
        final_row=byseq[c["final_seq"]]
        t2={(x["resource"],x["size"],x["format"]) for x in final_row["inputs"]
            if x["register"]=="2" and x["space"]=="0"}
        if len(t2)!=1:
            entry["status"]="missing_or_ambiguous_t2"
            continue
        resource,size,fmt=next(iter(t2))
        entry.update(t2_resource=resource,t2_size=size,t2_format=fmt)
        if (resource in (c["scene_target"],c["copy_target"],c["final_target"])
            or size!=c["sizes"][0] or fmt!="24"):
            entry["status"]="t2_identity_or_format_mismatch"
            continue
        # Include potential subsequent writes from any observed list, not
        # merely the list that recorded the original scene draw.
        edges=[e for e in copies[c["resolve"]] if start<e[0]<final and e[6]==resource]
        candidates=[e for e in edges if e[0]<blend and e[1]==c["list"]
            and e[2] in ("region","resource") and e[3]==c["scene_target"]
            and e[4]==size and e[7]==size and e[5] in (23,24) and e[8]==24]
        if len(candidates)!=1:
            entry["status"]="missing_or_ambiguous_preoverlay_copy"
            continue
        e=candidates[0]
        earlier_draw=any(r["resolve"]==c["resolve"] and r["target"]==c["scene_target"]
            and start<positions.get(r["seq"],-1)<e[0] for r in rows)
        later_draw=any(r["resolve"]==c["resolve"] and r["target"]==resource
            and e[0]<positions.get(r["seq"],-1)<final for r in rows)
        if earlier_draw or later_draw or any(q[0]>e[0] for q in edges):
            entry["status"]="intervening_or_overwriting_draw_copy"
            continue
        entry.update(status="preoverlay_copy_observed",copy_kind=e[2],copy_source_format=e[5],
                     copy_destination_format=e[8],copy_source=e[3],
                     postfx_seq=c["postfx_seq"],first_blend_seq=c["first_blend_seq"],final_seq=c["final_seq"])
    return {"hdr_chains":sum(o["path"]=="hdr_pq_candidate" for o in observations),
            "sdr_chains":sum(o["path"]=="sdr_curve_candidate" for o in observations),
            "observed_preoverlay_copies":sum(o["status"]=="preoverlay_copy_observed" for o in observations),
            "scale_directions":dict((k,sum(o["scale_direction"]==k for o in observations))
                for k in sorted({o["scale_direction"] for o in observations})),
            "observations":observations,
            "warning":"Shader path and recorded copy evidence only. No proof of copy region, UI contents, GPU ordering, color space, present ownership, or valid FG input. Do not enable rendering from this report."}
