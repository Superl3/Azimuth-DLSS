"""Analyze paired FH2 guide RAW files; never infer semantic object labels or NR consumption."""
import argparse
import collections
import json
import math
import pathlib
import re
import struct
import statistics

NAME = re.compile(r"f(\d+)-(stencil|guidecb|depth|mv|color)-(\d+)x(\d+)-fmt(\d+)\.raw$")

def values(path, fmt, count):
    data = path.read_bytes()
    size = struct.calcsize(fmt)
    if len(data) != count * size:
        raise ValueError(f"bad length: {path}")
    return struct.iter_unpack(fmt, data)

def finite_or_none(x):
    return x if math.isfinite(x) else None

def load_raw(item, allowed_formats, shape=None):
    if item is None:
        return None
    p,w,h,f=item
    if f not in allowed_formats or (shape is not None and (w,h)!=shape):
        return None
    if w<1 or h<1 or w>3840 or h>2160:
        raise ValueError(f"invalid analysis dimensions: {p}")
    raw=p.read_bytes()
    if len(raw)!=w*h*4:
        raise ValueError(f"bad length: {p}")
    return raw

def decode_ufloat(v, mantissa):
    frac=v&((1<<mantissa)-1)
    exp=v>>mantissa
    if exp==31:
        return math.inf if frac==0 else math.nan
    return math.ldexp(frac/(1<<mantissa),-14) if exp==0 else math.ldexp(1+frac/(1<<mantissa),exp-15)

def log_luma(raw,w,x,y):
    v=struct.unpack_from("<I",raw,(y*w+x)*4)[0]
    r=decode_ufloat(v&2047,6)
    g=decode_ufloat((v>>11)&2047,6)
    b=decode_ufloat((v>>22)&1023,5)
    if not all(map(math.isfinite,(r,g,b))):
        return math.nan
    # Approximate log luminance in the scene domain, not calibrated nits.
    return math.log1p(.2126*r+.7152*g+.0722*b)

def bilinear(raw,w,h,x,y):
    if not (0<=x<=w-1 and 0<=y<=h-1):
        return math.nan
    x0=int(x);y0=int(y)
    x1=min(x0+1,w-1);y1=min(y0+1,h-1)
    tx=x-x0;ty=y-y0
    a=log_luma(raw,w,x0,y0);b=log_luma(raw,w,x1,y0)
    c=log_luma(raw,w,x0,y1);d=log_luma(raw,w,x1,y1)
    return (1-ty)*((1-tx)*a+tx*b)+ty*((1-tx)*c+tx*d)

def residual_stats(values):
    if not values:
        return {"count":0}
    shift=statistics.median(values)
    return {"count":len(values),"median_signed_log_luma":shift,
            "mae_log_luma":statistics.mean(abs(x) for x in values),
            "mae_after_median_shift":statistics.mean(abs(x-shift) for x in values)}

def temporal_pair(prev_id,prev,cur_id,cur,stride=8):
    row={"previous_resolve":prev_id,"resolve":cur_id,"validated":False,
         "note":"Diagnostic current-to-previous color consistency only; not ground-truth flow, linear depth, exposure or NR consumption."}
    if cur_id!=prev_id+1:
        return dict(row,reason="nonadjacent_resolves")
    if "color" not in cur:
        return dict(row,reason="missing_color")
    _,w,h,_=cur["color"]
    shape=(w,h)
    previous=load_raw(prev.get("color"),(26,),shape)
    current=load_raw(cur.get("color"),(26,),shape)
    motion=load_raw(cur.get("mv"),(34,),shape)
    if previous is None or current is None or motion is None:
        return dict(row,reason="missing_or_incompatible_color_mv")
    prev_depth=load_raw(prev.get("depth"),(39,40,41),shape)
    cur_depth=load_raw(cur.get("depth"),(39,40,41),shape)
    prev_stencil=load_raw(prev.get("stencil"),(41,),shape)
    cur_stencil=load_raw(cur.get("stencil"),(41,),shape)
    modes={"zero":0.0,"current_to_previous":1.0,"reverse":-1.0,"half":.5,"double":2.0}
    residuals={k:[] for k in modes}
    tags=collections.defaultdict(lambda:{k:[] for k in modes})
    counts=collections.Counter()
    depth_delta=[]
    for y in range(0,h,stride):
        for x in range(0,w,stride):
            off=(y*w+x)*4
            counts["sampled"]+=1
            dx,dy=struct.unpack_from("<ee",motion,off)
            if not all(map(math.isfinite,(dx,dy))):
                counts["nonfinite_mv"]+=1
                continue
            dx*=w;dy*=h
            tag=None
            if cur_stencil is not None:
                s=struct.unpack_from("<f",cur_stencil,off)[0]
                if not math.isfinite(s) or s!=int(s) or not 0<=s<=255:
                    counts["invalid_current_stencil"]+=1
                    continue
                tag=int(s)
            center=log_luma(current,w,x,y)
            values={k:bilinear(previous,w,h,x+dx*f,y+dy*f) for k,f in modes.items()}
            if not math.isfinite(center) or not all(map(math.isfinite,values.values())):
                counts["outside_or_nonfinite_common_warp"]+=1
                continue
            counts["common"]+=1
            if dx==0 and dy==0:
                counts["zero_mv_common"]+=1
            for k,val in values.items():
                residuals[k].append(center-val)
                if tag is not None:
                    tags[tag][k].append(center-val)
            px=int(round(x+dx));py=int(round(y+dy))
            if not (0<=px<w and 0<=py<h):
                continue
            p_off=(py*w+px)*4
            if prev_depth is not None and cur_depth is not None:
                a=struct.unpack_from("<f",cur_depth,off)[0]
                b=struct.unpack_from("<f",prev_depth,p_off)[0]
                if math.isfinite(a) and math.isfinite(b):
                    depth_delta.append(abs(a-b))
            if prev_stencil is not None and tag is not None:
                other=struct.unpack_from("<f",prev_stencil,p_off)[0]
                if math.isfinite(other) and other==int(other) and 0<=other<=255:
                    counts["paired_valid_stencil"]+=1
                    counts["stencil_id_mismatch"]+=int(other!=tag)
    return dict(row,reason="comparison_only" if counts["common"] else "no_common_valid_samples",
                size=[w,h],stride=stride,counts=dict(counts),
                metrics={k:residual_stats(v) for k,v in residuals.items()},
                stencil_metrics={str(k):{name:residual_stats(v) for name,v in group.items()} for k,group in sorted(tags.items())},
                raw_depth_delta_median=statistics.median(depth_delta) if depth_delta else None,
                raw_depth_delta_count=len(depth_delta),
                caution="Compare fresh-MV, reset, jitter and external NR mode logs. Color capture occurs after NGX evaluation and may be mutated by external pre-SR NR. Nonlinear raw depth deltas are not disocclusion masks. Zero or low-texture scenes may not discriminate modes.")

def analyze(folder,stride=8):
    groups = collections.defaultdict(dict)
    for path in sorted(folder.rglob("*.raw")):
        m = NAME.fullmatch(path.name)
        if m:
            frame, name, w, h, fmt = m.groups()
            if name in groups[int(frame)]:
                raise ValueError(f"duplicate {name} for resolve {frame}; analyze each session separately")
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
            counts = collections.defaultdict(lambda: {"pixels":0,"invalid_reprojection":0,"mv_finite":0,"mv_mag_sum_px":0.0,"depth_finite":0,"depth_min":math.inf,"depth_max":-math.inf})
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
    ordered=sorted(groups.items())
    pairs=[temporal_pair(a,g,b,h,stride) for (a,g),(b,h) in zip(ordered,ordered[1:])]
    return {"groups":report,"temporal_pairs":pairs,"nr_receiver_verified":False}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("folder",type=pathlib.Path)
    p.add_argument("--output",type=pathlib.Path)
    p.add_argument("--stride",type=int,default=8)
    a=p.parse_args()
    if not 1<=a.stride<=128:
        p.error("--stride must be 1..128")
    result=json.dumps(analyze(a.folder,a.stride),indent=2,allow_nan=False)
    if a.output:
        a.output.write_text(result+"\n",encoding="utf-8")
    else:
        print(result)

if __name__=="__main__":
    main()
