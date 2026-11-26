#!/usr/bin/env python3
"""Local-only Azimuth SR trace/probe runner and evidence packager."""
import argparse
import csv
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

from analyze_sr_trace import parse
from analyze_fg_trace import parse_fg
from analyze_debug_trace import parse_debug

DIM = re.compile(r"[1-9]\d*x[1-9]\d*")
LABEL = re.compile(r"[A-Za-z0-9_-]{1,48}")
GEOMETRY = re.compile(r"SR GEOMETRY color=(\d+x\d+) depth=(\d+x\d+) cfmt=(\d+) dfmt=(\d+) samples=(\d+)/(\d+) vpCount=(\d+) viewport=([^\s]+) scCount=(\d+) scissor=([^\s]+)")
VELOCITY = re.compile(r"SR NATIVE VELOCITY target=(\d+x\d+) format=(\d+) viewport=([^\s]+)")
SWAP = re.compile(r"SR observed swapchain: (\d+x\d+)")

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def trace_env(mode, size, quality, budget, fg=False, fg_start=1, fg_resolves=8, renderdoc=False):
    env = {k:v for k,v in os.environ.items() if not k.upper().startswith(("AZIMUTH_SR_", "AZIMUTH_FG_", "AZIMUTH_DEBUG_"))}
    env["AZIMUTH_SR_MODE"] = mode
    env["AZIMUTH_SR_TRACE_BUDGET"] = str(budget)
    if renderdoc and mode != "debug": raise ValueError("RenderDoc trigger requires debug mode")
    if mode == "debug": env["AZIMUTH_DEBUG_RENDERDOC"] = "1" if renderdoc else "0"
    if fg:
        if mode != "trace": raise ValueError("FG discovery requires trace mode")
        env["AZIMUTH_FG_TRACE"] = "1"
        env["AZIMUTH_FG_TRACE_START"] = str(fg_start)
        env["AZIMUTH_FG_TRACE_RESOLVES"] = str(fg_resolves)
    if mode == "probe":
        env["AZIMUTH_SR_OUTPUT"] = size
        env["AZIMUTH_SR_QUALITY"] = quality
    return env

def summarize(text, output):
    candidates = parse(text, output)
    def unique(pattern, names):
        return [dict(zip(names, v)) for v in sorted(set(pattern.findall(text)))]
    return {
        "trace_mode_seen": "SR TRACE ONLY:" in text,
        "resolves_logged": text.count("SR TRACE RESOLVE"),
        "swapchains_observed": sorted(set(SWAP.findall(text))),
        "geometry": unique(GEOMETRY, ("color","depth","color_format","depth_format","color_samples","depth_samples","viewport_count","viewport","scissor_count","scissor")),
        "native_velocity": unique(VELOCITY, ("target","format","viewport")),
        "incomplete_tracking": "SR TRACE incomplete" in text,
        "ngx_probe_evaluated": "SR PROBE EVALUATED" in text,
        "route_copied": "SR ROUTE COPIED" in text,
        "candidates": candidates,
        "fg_discovery": parse_fg(text),
        "debug_capture": parse_debug(text, output),
        "warning": "Bound SRVs are not proof of shader sampling; a resolve viewport is not proof of geometry resolution. Do not arm route from this report."
    }

def running_azimuth():
    result = subprocess.run(["tasklist.exe","/FI","IMAGENAME eq azimuth.exe","/FO","CSV","/NH"],
                            capture_output=True, text=True, errors="replace", check=True)
    return any(row and row[0].lower() == "azimuth.exe" for row in csv.reader(result.stdout.splitlines()))

def package(log, game, dest, label, mode, output, quality, render, exit_code):
    raw = log.read_bytes()
    text = raw.decode("utf-8", errors="replace")
    summary = summarize(text, output)
    if mode in ("trace","debug") and not summary["trace_mode_seen"]:
        raise RuntimeError("Log does not identify trace mode. Check that the experimental dxgi.dll was installed.")
    if mode=="debug" and not summary["debug_capture"]["debug_ready"]:
        raise RuntimeError("Log does not identify debug mode. Check that the latest debug proxy was installed.")
    metadata = {"label": label, "mode": mode, "requested_output": output, "requested_render_setting": render,
                "requested_quality": quality if mode == "probe" else None,
                "exit_code": exit_code, "utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "files_sha256": {name: sha256(game / name) for name in ("azimuth.exe","dxgi.dll","azimuth_ngx.dll","nvngx_dlss.dll") if (game / name).is_file()}}
    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=label+"-", suffix=".zip", dir=dest, delete=False) as f:
        target = Path(f.name)
    try:
        with zipfile.ZipFile(target,"w",compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("azimuth_dlaa.log",raw)
            z.writestr("capture.json",json.dumps(metadata,indent=2))
            z.writestr("summary.json",json.dumps(summary,indent=2))
    except Exception:
        target.unlink(missing_ok=True)
        raise
    return target, summary

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game-dir", required=True, type=Path)
    p.add_argument("--label", required=True)
    p.add_argument("--render-setting", required=True, help="Actual setting selected in Azimuth (not applied by this script).")
    p.add_argument("--output", required=True, help="Expected display resolution, e.g. 3840x1600")
    p.add_argument("--mode", choices=("trace","probe","debug"), default="trace")
    p.add_argument("--quality", choices=("quality","balanced","performance"),default="quality")
    p.add_argument("--budget",type=int,default=4000)
    p.add_argument("--fg-trace",action="store_true",help="Observe final-size pass candidates; does not enable FG.")
    p.add_argument("--renderdoc",action="store_true",help="Debug only: F8 triggers the API if RenderDoc is already loaded; does not inject it.")
    p.add_argument("--fg-start",type=int,default=1,help="First recognized resolve sequence to observe.")
    p.add_argument("--fg-resolves",type=int,default=8,help="Number of resolve sequences to observe (1..64).")
    p.add_argument("--dest",type=Path,default=Path("sr-captures"))
    p.add_argument("--collect-only",action="store_true",help="Package an existing log, e.g. after launching through PIX. Verify its freshness yourself.")
    a=p.parse_args()
    if not LABEL.fullmatch(a.label) or not DIM.fullmatch(a.output) or not 160<=a.budget<=20000:
        p.error("Invalid label, output WxH or budget (160..20000).")
    if a.fg_trace and (a.mode!='trace' or not 1<=a.fg_start<=1000000 or not 1<=a.fg_resolves<=64):
        p.error('FG discovery requires trace mode, start 1..1000000 and resolves 1..64.')
    if a.renderdoc and a.mode!="debug": p.error("--renderdoc requires --mode debug")
    game=a.game_dir.resolve()
    log=game/"azimuth_dlaa.log"
    for name in ("azimuth.exe","dxgi.dll"):
        if not (game/name).is_file(): p.error("Missing "+name+" in game directory")
    if a.mode=="probe" and not (game/"azimuth_ngx.dll").is_file():
        p.error("Probe requires the matching azimuth_ngx.dll")
    exit_code=None
    try:
        if not a.collect_only:
            if sys.platform!="win32": p.error("Game launch is Windows-only; use --collect-only for saved logs.")
            if running_azimuth(): p.error("Close existing Azimuth instances first.")
            # Preserve prior logs. Do not alter game settings, saves or DLLs.
            old_mtime=log.stat().st_mtime_ns if log.is_file() else None
            if log.is_file():
                a.dest.mkdir(parents=True,exist_ok=True)
                with tempfile.NamedTemporaryFile(prefix="previous-",suffix=".log",dir=a.dest,delete=False) as f:
                    backup=Path(f.name)
                shutil.copy2(log,backup)
                print("Previous log copied to",backup)
            print("Drive into gameplay, then press F8 to capture (debug mode). Exit normally. No files are uploaded.",flush=True)
            proc=subprocess.Popen([str(game/"azimuth.exe")],cwd=game,
                                  env=trace_env(a.mode,a.output,a.quality,a.budget,a.fg_trace,a.fg_start,a.fg_resolves,a.renderdoc))
            exit_code=proc.wait()
            if running_azimuth():
                raise RuntimeError("A launcher child is still running. Close it and use --collect-only.")
            if not log.is_file() or log.stat().st_mtime_ns==old_mtime:
                raise RuntimeError("No fresh log was produced.")
        if not log.is_file(): raise RuntimeError("No azimuth_dlaa.log found.")
        target, summary=package(log,game,a.dest,a.label,a.mode,a.output,a.quality,a.render_setting,exit_code)
        print("Evidence package:",target.resolve())
        print("Resolve samples:",summary["resolves_logged"],"candidate groups:",len(summary["candidates"]))
        if a.mode=="debug" and not summary["debug_capture"]["capture_count"]:
            print("WARNING: no F8 capture window found; enter actual gameplay and press F8.")
        if a.fg_trace and not summary["fg_discovery"]["enabled"]:
            print("WARNING: FG trace not detected in log; verify build and capture settings.")
        if not summary["resolves_logged"]: print("WARNING: no recognized scene resolve. Include the log for diagnosis.")
        print("Review the ZIP locally before sharing. It contains logs, hashes and settings labels only.")
    except (OSError,RuntimeError,subprocess.SubprocessError) as e:
        print("Capture failed:",e,file=sys.stderr)
        return 1
    return 0

if __name__=="__main__":
    sys.exit(main())
