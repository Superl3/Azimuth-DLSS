"""Analyze paired FH2 guide RAW files; never infer semantic object labels or NR consumption."""
import argparse
import collections
import json
import math
import pathlib
import re
import struct

NAME = re.compile(r"f(\d+)-(stencil|guidecb|depth|mv)-(\d+)x(\d+)-fmt(\d+)\.raw$")

def values(path, fmt, count):
    data = path.read_bytes()
    size = struct.calcsize(fmt)
    if len(data) != count * size:
        raise ValueError(f"bad length: {path}")
    return struct.iter_unpack(fmt, data)

def finite_or_none(x):
    return x if math.isfinite(x) else None

def analyze(folder):
    groups = collections.defaultdict(dict)
    for path in sorted(folder.rglob("*.raw")):
        m = NAME.fullmatch(path.name)
        if m:
            frame, name, w, h, fmt = m.groups()
            groups[int(frame)][name] = (path, int(w), int(h), int(fmt))
    report = []
    for frame, g in sorted(groups.items()):
        row = {"resolve": frame, "available": sorted(g), "engine_semantics_verified": False}
        if "guidecb" in g:
            p,w,h,f = g["guidecb"]
            if (w,h,f) != (64,1,41):
                raise ValueError("unexpected guidecb shape")
            v = [finite_or_none(x[0]) for x in values(p,"<f",64)]
            row["candidate_cb_rows"] = {
                "selector": v[0],
                "luminance_keys": v[1:5],
                "adaptive_exposure": v[5:9],
                "adaptive_params": v[9:13],
                "view_matrix": v[13:29],
                "projection_matrix": v[29:45],
                "stencil_zero_reprojection": v[45:61],
                "note": "Raw selected b1 rows at velocity pass. Field names from shader reflection; unused fields may be zero, stale or unrelated to SR color pre-exposure."
            }
        if "stencil" in g:
            p,w,h,f = g["stencil"]
            if f != 41:
                raise ValueError("unexpected stencil format")
            mv = g.get("mv")
            dep = g.get("depth")
            motion = values(mv[0],"<ee",w*h) if mv and mv[1:]==(w,h,34) else None
            depth = values(dep[0],"<f",w*h) if dep and dep[1:3]==(w,h) and dep[3] in (39,40,41) else None
            counts = collections.defaultdict(lambda: {"pixels":0,"invalid_reprojection":0,"mv_finite":0,"mv_mag_sum_px":0.0,"depth_finite":0,"depth_min":1.0,"depth_max":0.0})
            bad = 0
            for (s,) in values(p,"<f",w*h):
                m = next(motion) if motion is not None else None
                d = next(depth)[0] if depth is not None else None
                if not math.isfinite(s) or s!=int(s) or not -256<=s<=255:
                    bad+=1
                    continue
                tag=int(s) if s>=0 else -int(s)-1
                a=counts[tag];a["pixels"]+=1;a["invalid_reprojection"]+=s<0
                if m is not None and all(map(math.isfinite,m)):
                    a["mv_finite"]+=1;a["mv_mag_sum_px"]+=math.hypot(m[0]*w,m[1]*h)
                if d is not None and math.isfinite(d):
                    a["depth_finite"]+=1;a["depth_min"]=min(a["depth_min"],d);a["depth_max"]=max(a["depth_max"],d)
            for a in counts.values():
                n=a.pop("mv_mag_sum_px")
                a["mean_mv_magnitude_px_default_scale"]=n/a["mv_finite"] if a["mv_finite"] else None
                if not a["depth_finite"]:
                    a["depth_min"]=a["depth_max"]=None
            row["stencil_groups"] = dict(sorted(counts.items()))
            row["unexpected_values"] = bad
            row["size"] = [w,h]
            row["paired_mv"] = motion is not None
            row["paired_depth"] = depth is not None
        report.append(row)
    return {"groups":report,"nr_receiver_verified":False}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("folder",type=pathlib.Path)
    p.add_argument("--output",type=pathlib.Path)
    a=p.parse_args()
    result=json.dumps(analyze(a.folder),indent=2,allow_nan=False)
    if a.output:
        a.output.write_text(result+"\n",encoding="utf-8")
    else:
        print(result)

if __name__=="__main__":
    main()
