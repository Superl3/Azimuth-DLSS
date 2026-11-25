#!/usr/bin/env python3
"""Summarize bounded, local SR/FG debug capture windows; never enable features."""
import argparse
import json
from pathlib import Path
import re

from analyze_sr_trace import parse as parse_sr
from analyze_fg_trace import parse_fg, analyze_fh2_postfx, analyze_hdr_scene_copy

BEGIN=re.compile(r"^DEBUG BEGIN id=(\d+) firstResolveAfter=(\d+) limit=(\d+) timeoutMs=(\d+)")
END=re.compile(r"^DEBUG END id=(\d+) reason=(\w+) resolves=(\d+) lastResolve=(\d+)")
AUTO_BEGIN=re.compile(r"^DEBUG AUTO_BEGIN id=(\d+) requestedResolve=(\d+)$")
SECTIONS={
    "geometry_draws":"DEBUG GEOMETRY_DRAW ",
    "resolve_geometry":"SR GEOMETRY ",
    "native_velocity":"SR NATIVE VELOCITY ",
    "role_draws":"DEBUG ROLE ",
    "role_hashes":"DEBUG ROLE_HASH ",
    "role_bound_srvs":"DEBUG ROLE_SRV ",
    "copy_edges":"DEBUG COPY ",
    "compute_dispatches":"DEBUG DISPATCH ",
    "queue_submissions":"DEBUG SUBMIT ",
    "submitted_lists":"DEBUG SUBMIT_LIST ",
    "queue_signals":"DEBUG SIGNAL ",
    "queue_waits":"DEBUG WAIT ",
}
def analyze_visible(text):
    """Identity and submission observations, not proof of actual Present."""
    attempts={}; frames={}; counts={}; skip_reasons={}
    last_success_by_hwnd={}
    for line in text.splitlines():
        if not line.startswith("VISIBLE "): continue
        parts=line.split()
        if len(parts)<2: continue
        tag=parts[1]; counts[tag]=counts.get(tag,0)+1
        row=dict(re.findall(r"(\w+)=([^\s]+)",line))
        if tag in ("SWAPCHAIN_REQUEST","SWAPCHAIN_RESULT","CHAIN") and row.get("attempt","").isdigit():
            i=int(row["attempt"]); entry=attempts.setdefault(i,{"attempt":i})
            entry[tag.lower()]=row
            hwnd=row.get("hwnd")
            if tag=="SWAPCHAIN_REQUEST":
                entry["prior_success_same_hwnd"]=last_success_by_hwnd.get(hwnd)
            elif tag=="SWAPCHAIN_RESULT":
                try: success=int(row.get("hr","ffffffff"),16)<0x80000000
                except ValueError: success=False
                entry["success"]=success
                if success and hwnd:last_success_by_hwnd[hwnd]=i
        resolve=row.get("resolve",row.get("resolveCandidate",""))
        if not resolve.isdigit() or int(resolve)==0: continue
        n=int(resolve)
        if tag in ("SRPOSTFX_READY","HUD_INPUT_READY","HUD_COMPOSITE_RECORDED","FINAL_RECORDED",
                   "FINAL_SUBMITTED","FINAL_GPU_COMPLETE","FINAL_CANDIDATE","TARGET_SUBMITTED",
                   "PRESENT_BARRIER","PRESENT_BARRIER_SUBMITTED","WORK_WATCH","WORK","WORK_BIND","WORK_BARRIER","WORK_SUBMIT","EDGE","PROBE_BEGIN","PROBE_HUD_RECORDED","PROBE_FINAL_RECORDED","SKIP"):
            frame=frames.setdefault(n,{"resolve":n,"stages":{},"skips":[]})
            frame["stages"][tag]=frame["stages"].get(tag,0)+1
            if tag=="SKIP":
                reason=row.get("reason","unknown")
                key=row.get("stage","unknown")+":"+reason
                skip_reasons[key]=skip_reasons.get(key,0)+1
                frame["skips"].append(row)
            elif tag=="FINAL_CANDIDATE":
                frame.setdefault("final_candidates",[]).append(row)
            elif tag in ("WORK_WATCH","WORK","WORK_BIND","WORK_BARRIER","WORK_SUBMIT","EDGE"):
                frame.setdefault("work_and_edges",[]).append({"tag":tag,**row})
            elif tag=="PRESENT_BARRIER_SUBMITTED":
                frame.setdefault("submitted_present_barriers",[]).append(row)
    results=list(frames.values())
    return {"swapchain_attempts":list(attempts.values()),
            "failed_creations":sum(x.get("success") is False for x in attempts.values()),
            "counts":counts,"skips_by_stage_reason":skip_reasons,
            "resolve_count_with_observations":len(results),
            "recorded_resolves":sum("FINAL_RECORDED" in f["stages"] for f in results),
            "submitted_resolves":sum("FINAL_SUBMITTED" in f["stages"] for f in results),
            "gpu_complete_resolves":sum("FINAL_GPU_COMPLETE" in f["stages"] for f in results),
            "frames":results,"actual_present_verified":False,
            "warning":"Creation snapshots, PRESENT/COMMON transitions and submissions are candidates; external ETW/video required. No actual Present inferred."}

def parse_debug(text, output=None):
    windows=[]; current=None; by_id={}
    startup={"queues":[],"swapchains":[],"backbuffers":[],"warnings":[]}
    rdc=[]
    last_id=None
    for line in text.splitlines():
        m=BEGIN.match(line)
        if m:
            i,first,limit,timeout=map(int,m.groups())
            if current is not None:
                current["warnings"].append("Another begin marker before end; previous capture incomplete.")
            current={"id":i,"first_resolve_after":first,"limit":limit,"timeout_ms":timeout,
                     "complete":False,"trigger":"manual","warnings":[],"lines":[]}
            windows.append(current);by_id[i]=current;last_id=i
            continue
        m=AUTO_BEGIN.match(line)
        if m:
            i,requested=map(int,m.groups())
            if i in by_id:
                by_id[i].update(trigger="automatic",requested_resolve=requested)
            continue
        m=END.match(line)
        if m:
            i,reason,n,last=m.groups();w=by_id.get(int(i))
            if w is not None:
                w.update(complete=True,end_reason=reason,observed_resolves=int(n),last_resolve=int(last))
                if current is w: current=None
            continue
        if line.startswith("DEBUG QUEUE_CREATE "): startup["queues"].append(line)
        elif line.startswith("FG SWAPCHAIN "): startup["swapchains"].append(line)
        elif line.startswith("FG BACKBUFFER "): startup["backbuffers"].append(line)
        elif line.startswith("DEBUG RENDERDOC "):
            rdc.append({"nearest_capture_id":last_id,"message":line})
        if "SR TRACE incomplete" in line or line.startswith("DEBUG QUEUE wrapper differs"):
            startup["warnings"].append(line)
        if current is not None: current["lines"].append(line)
    for w in windows:
        lines=w.pop("lines"); segment="\n".join(lines)
        w["sections"]={name:[line for line in lines if line.startswith(prefix)] for name,prefix in SECTIONS.items()}
        w["counts"]={name:len(rows) for name,rows in w["sections"].items()}
        w["resolve_records"]=sum(line.startswith("SR TRACE RESOLVE ") for line in lines)
        w["sr_candidates"]=parse_sr(segment,output)
        w["fg_discovery"]=parse_fg("FG TRACE ONLY\n"+"\n".join(startup["swapchains"])+"\n"+segment)
        w["fh2_postfx"]=analyze_fh2_postfx(w["fg_discovery"]["passes"])
        w["hdr_scene_copy"]=analyze_hdr_scene_copy(lines,w["fg_discovery"]["passes"],w["fh2_postfx"]["chains"])
        w["truncation"]=[line for line in lines if "TRUNCATED" in line or "truncated" in line]
        if not w["complete"]:w["warnings"].append("No end marker; game closed or trace interrupted.")
        if not w["resolve_records"]:w["warnings"].append("No recognized scene resolve in window; capture may be menu/loading or unsupported profile.")
        if not w["counts"]["native_velocity"]:w["warnings"].append("No native velocity pass observed in this window.")
        if w["truncation"]:w["warnings"].append("Bounded log truncated; use GPU capture or a shorter window.")
        if not w["fg_discovery"]["draw_count"]:w["warnings"].append("No FG draw candidates; descriptor/PSO tracking may be incomplete.")
    preview=[line for line in text.splitlines() if line.startswith(("SR PREVIEW:","PREVIEW ","SR PROBE RESULT ","POSTFX REPLAY ","POSTFX SR_","POSTFX HDR_","VISIBLE "))]
    return {"preview_observations":preview,"visible_trial":analyze_visible(text),"debug_ready":"DEBUG READY:" in text,"capture_count":len(windows),
            "startup":startup,"captures":windows,"renderdoc_observations":rdc,
            "warning":"CPU metadata is not a GPU capture or proof of resource contents. RenderDoc completion count is observational; inspect actual .rdc files. No guessed HUD/camera values and no routing/FG enablement."}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("log",type=Path)
    p.add_argument("--output",help="Expected display WxH, not a measured value.")
    a=p.parse_args()
    if a.output and not re.fullmatch(r"[1-9]\d*x[1-9]\d*",a.output):p.error("--output must be WxH")
    print(json.dumps(parse_debug(a.log.read_text(encoding="utf-8",errors="replace"),a.output),indent=2))
if __name__=="__main__":main()
