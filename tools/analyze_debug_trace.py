#!/usr/bin/env python3
"""Summarize bounded, local SR/FG debug capture windows; never enable features."""
import argparse
import json
from pathlib import Path
import re

from analyze_sr_trace import parse as parse_sr
from analyze_fg_trace import parse_fg, analyze_fh2_postfx

BEGIN=re.compile(r"^DEBUG BEGIN id=(\d+) firstResolveAfter=(\d+) limit=(\d+) timeoutMs=(\d+)")
END=re.compile(r"^DEBUG END id=(\d+) reason=(\w+) resolves=(\d+) lastResolve=(\d+)")
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
                     "complete":False,"warnings":[],"lines":[]}
            windows.append(current);by_id[i]=current;last_id=i
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
        w["truncation"]=[line for line in lines if "TRUNCATED" in line or "truncated" in line]
        if not w["complete"]:w["warnings"].append("No end marker; game closed or trace interrupted.")
        if not w["resolve_records"]:w["warnings"].append("No recognized scene resolve in window; capture may be menu/loading or unsupported profile.")
        if not w["counts"]["native_velocity"]:w["warnings"].append("No native velocity pass observed in this window.")
        if w["truncation"]:w["warnings"].append("Bounded log truncated; use GPU capture or a shorter window.")
        if not w["fg_discovery"]["draw_count"]:w["warnings"].append("No FG draw candidates; descriptor/PSO tracking may be incomplete.")
    return {"debug_ready":"DEBUG READY:" in text,"capture_count":len(windows),
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
