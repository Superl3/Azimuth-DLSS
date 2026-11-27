"""Verify one FH2 capture's exact FG input pair and run the real NGX replay.
No game or DLL modifications. An uncalibrated camera is opt-in, never guessed.
"""
import argparse
import hashlib
import json
import math
import pathlib
import re
import subprocess
import sys

MASK = 6 | 1024 | 131072
CORE = re.compile(r"^FG INPUT_CORE resolve=(\d+) reset=([01]) fresh=([01]) jitter=([01]) jitterX=([^ ]+) jitterY=([^ ]+) recording=(\d+)", re.M)
FINAL = re.compile(r"^FG INPUT_FINAL resolve=(\d+) final=1 hudless=1 hdr=([01]) actualPresent=0\s*$", re.M)
GROUP = re.compile(r"^PREVIEW GROUP_RESULT resolve=(\d+) expected=0x[0-9a-f]+ attempted=0x[0-9a-f]+ saved=0x([0-9a-f]+) postfxSeen=0x[0-9a-f]+ coreComplete=1\s*$", re.M)
COMPLETE = re.compile(r"^VISIBLE FINAL_GPU_COMPLETE resolve=(\d+) ", re.M)
RAW = re.compile(r"^PREVIEW FILE directory=azimuth_preview_\d+_\d+ file=(f(\d+)-(depth|mv|visiblefinal|fghudless)-(\d+)x(\d+)-fmt(\d+)\.raw) bytes=(\d+)\s*$", re.M)

def one(items, label):
    if len(items) != 1:
        raise ValueError("expected one " + label)
    return items[0]

def build_contract(root):
    meta = json.loads((root / "capture.json").read_text(encoding="utf-8-sig"))
    if not all(meta.get(k) is True for k in ("fg_input_probe_requested", "preview_complete", "visible_sr_requested", "sr_no_jitter_requested")):
        raise ValueError("not a complete explicit no-jitter FG input capture")
    if meta.get("visible_marker_requested") or meta.get("sr_mv_jittered_requested"):
        raise ValueError("marker or jittered vectors unsupported in alpha")
    if meta.get("visible_hdr_requested") and not meta.get("hdr_live_requested"):
        raise ValueError("HDR requires live routed output")
    log = (root / "azimuth_dlaa.log").read_text(encoding="utf-8", errors="replace")
    cores = list(CORE.finditer(log))
    first, second = (int(m[1]) for m in cores) if len(cores) == 2 else (None, None)
    if first is None or second != first + 1:
        raise ValueError("require exactly two adjacent selected resolve candidates")
    for i, m in enumerate(cores):
        if m[3] != "1" or m[4] != "0" or float(m[5]) != 0 or float(m[6]) != 0 or not int(m[7]) or (i and m[2] != "0"):
            raise ValueError("stale MV, jitter, invalid recording or reset between frames")
    raw = list(RAW.finditer(log))
    groups = list(GROUP.finditer(log))
    finals = list(FINAL.finditer(log))
    complete = set(int(i) for i in COMPLETE.findall(log))
    result = {"first_resolve": first, "frames": [], "present_lineage_verified": False}
    shape = None
    hdr = int(bool(meta.get("visible_hdr_requested")))
    for i in (first, second):
        g = one([m for m in groups if int(m[1]) == i], f"group result {i}")
        f = one([m for m in finals if int(m[1]) == i], f"final result {i}")
        if int(g[2], 16) & MASK != MASK or i not in complete or int(f[2]) != hdr:
            raise ValueError("missing fence-completed FG input")
        roles = {}
        for role, formats in (("depth", (39, 40, 41)), ("mv", (34,)), ("visiblefinal", (24,)), ("fghudless", (24,))):
            m = one([m for m in raw if int(m[2]) == i and m[3] == role], f"{role} {i}")
            name, w, h, fmt, size = m[1], int(m[4]), int(m[5]), int(m[6]), int(m[7])
            if fmt not in formats or not 320 <= w <= 3840 or not 200 <= h <= 2160 or size != w*h*4:
                raise ValueError("bad RAW format/shape")
            file = root / "gpu-raw" / name
            if name not in meta.get("gpu_raw", []) or file.is_symlink() or not file.is_file() or file.stat().st_size != size:
                raise ValueError("missing or incorrect packaged RAW")
            digest = hashlib.sha256()
            with file.open("rb") as inp:
                for chunk in iter(lambda: inp.read(1 << 20), b""):
                    digest.update(chunk)
            roles[role] = {"file": name, "width": w, "height": h, "format": fmt, "sha256": digest.hexdigest()}
        dims = lambda name: (roles[name]["width"], roles[name]["height"])
        r, o = dims("depth"), dims("visiblefinal")
        if r != dims("mv") or o != dims("fghudless") or r[0] > o[0] or r[1] > o[1] or (shape and shape != (r, o)):
            raise ValueError("mismatched FG input sizes")
        shape = (r, o)
        result["frames"].append({"resolve": i, "roles": roles})
    result["render"], result["output"] = shape
    result["hdr"] = hdr
    result["depth_inverted_requested"] = int(meta.get("sr_depth_inverted_requested", 0))
    # SR caller's normalized MV multipliers, not its pixel-space scales.
    scales = meta.get("sr_mv_scale_requested", [1.0, 1.0])
    if result["depth_inverted_requested"] not in (0, 1) or len(scales) != 2 or not all(math.isfinite(float(v)) and 0 < abs(float(v)) <= 4 for v in scales):
        raise ValueError("invalid guide metadata")
    result["mv_normalization_scale_requested"] = [float(v) for v in scales]
    result["camera_semantics_verified"] = False
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--runtime", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--exe", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1]/"azimuth_fg_replay.exe")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--camera", type=pathlib.Path)
    group.add_argument("--probe-no-camera", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    a = parser.parse_args()
    try:
        contract = build_contract(a.capture)
        if a.validate_only:
            print(json.dumps(contract, indent=2))
            return 0
        if not (a.runtime and a.output and (a.camera or a.probe_no_camera)):
            parser.error("replay requires --runtime, --output, and --camera or explicit --probe-no-camera")
        if a.camera and not a.camera.is_file():
            raise ValueError("camera file missing")
        cam = str(a.camera.resolve()) if a.camera else "probe-no-camera"
        contract["camera_mode"] = "operator_supplied" if a.camera else "unverified_probe"
        if a.output.exists() and (not a.output.is_dir() or any(a.output.iterdir())):
            raise ValueError("output directory must be absent or empty")
        a.output.mkdir(parents=True, exist_ok=True)
        cmd = [str(a.exe.resolve()), "replay", str(a.runtime.resolve()), str((a.capture/"gpu-raw").resolve()),
               str(a.output.resolve()), str(contract["first_resolve"]), *map(str, contract["render"]),
               *map(str, contract["output"]), str(contract["hdr"]), str(contract["depth_inverted_requested"]),
               *map(str, contract["mv_normalization_scale_requested"]), cam]
        # The evaluator requires an empty output dir; store this evidence after exit.
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=300)
        contract["exit_code"] = p.returncode
        (a.output/"fg-input-contract.json").write_text(json.dumps(contract,indent=2)+"\n",encoding="utf-8")
        (a.output/"fg-replay.log").write_text(p.stdout, encoding="utf-8")
        print(p.stdout, end="")
        return p.returncode
    except (ValueError, OSError, subprocess.TimeoutExpired) as e:
        print(f"FG replay refused/failed: {e}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
