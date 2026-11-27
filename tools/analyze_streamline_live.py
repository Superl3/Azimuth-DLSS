#!/usr/bin/env python3
"""Check submitted FH2 Streamline packet lineage; never infer displayed FG from API count."""
import argparse
import collections
import csv
import json
import re
from pathlib import Path

FIELD = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")

def fields(line):
    return dict(FIELD.findall(line))

def analyze(lines):
    finals = {}
    stages = collections.defaultdict(dict)
    copies = collections.defaultdict(list)
    tags = {}
    results = {}
    skips = collections.Counter()
    invalidations = 0
    valid = []
    errors = []
    for line in lines:
        row = fields(line)
        if line.startswith("FG FINAL_RECORD "):
            finals[row.get("packet")] = row
        elif line.startswith("FG STAGE_SUBMIT "):
            stages[row.get("packet")][row.get("stage")] = row
        elif line.startswith("FG COPY "):
            copies[(row.get("resolve"), row.get("generation"))].append(row)
        elif line.startswith("FG INVALIDATE "):
            invalidations += 1
        elif line.startswith("FG PRESENT_SKIP "):
            skips[row.get("reason", "unknown")] += 1
        elif line.startswith("FG PRESENT_TAG "):
            tags[row.get("call")] = row
        elif line.startswith("FG PRESENT_RESULT "):
            results[row.get("call")] = row
    for call, tag in tags.items():
        packet = tag.get("packet")
        final = finals.get(packet)
        producer = stages.get(packet, {})
        result = results.get(call)
        ok = bool(final and result and all(x in producer for x in ("2", "3", "4")))
        if ok:
            ok = all(x.get("resolve") == tag.get("resolve") and x.get("generation") == tag.get("generation")
                     for x in (final, *producer.values()))
            ok = ok and final.get("nativeRecording") == producer["4"].get("nativeRecording")
            ok = ok and final.get("hudless") == "1"
        if ok:
            # Follow only full copies marked proven by native Execute.
            # The last edge must be the exact buffer/record/submit/queue tagged.
            source = final.get("cookie")
            last = producer["4"]
            for edge in copies[(tag.get("resolve"), tag.get("generation"))]:
                if edge.get("proven") != "1" or edge.get("srcCookie") != source:
                    continue
                if int(edge.get("submit", "0")) < int(last.get("submit", "0")):
                    continue
                source = edge.get("dstCookie")
                last = edge
                if (source == tag.get("bufferCookie") and last.get("nativeRecording") == tag.get("transferRecording")
                        and last.get("submit") == tag.get("submit")):
                    break
            ok = (source == tag.get("bufferCookie") and last.get("nativeRecording") == tag.get("transferRecording")
                  and last.get("submit") == tag.get("submit") and last.get("queue") == tag.get("queue"))
        if ok:
            ok = (result.get("token") == tag.get("token") and result.get("resolve") == tag.get("resolve")
                  and tag.get("result") == "0" and result.get("sdkResult") == "0"
                  and result.get("sdkStatus") == "0" and result.get("hr") == "00000000")
        if not ok:
            errors.append(call)
            continue
        valid.append(dict(call=int(call), resolve=int(tag["resolve"]), token=int(tag["token"]),
                          generation=int(tag["generation"]), reset=tag.get("reset") == "1",
                          mode=tag.get("mode") == "1", frames_since_query=int(result.get("framesSinceQuery","0")),
                          consumption_fence=result.get("consumptionValue", "0") != "0"))
    consecutive = []
    for prev, cur in zip(valid, valid[1:]):
        if (cur["call"] > prev["call"] and cur["generation"] == prev["generation"]
                and cur["resolve"] == prev["resolve"] + 1 and cur["token"] == prev["token"] + 1
                and not cur["reset"] and cur["mode"] and prev["mode"]
                and cur["consumption_fence"] and prev["consumption_fence"]):
            consecutive.append(cur)
    return dict(tags=len(tags), valid_lineage_and_sdk=len(valid), invalid_tags=errors,
                skipped_by_reason=dict(skips), virtual_buffer_invalidations=invalidations,
                consecutive_fg_candidates=len(consecutive),
                sdk_multiple_frame_candidates=sum(x["frames_since_query"] > 1 for x in consecutive),
                actual_display_proof=False,
                note="SDK counts since last query are not displayed-frame attribution.",
                accepted_frames=valid)

def display_summary(path):
    # Accept a physical-chain filtered PresentMon CSV as external evidence.
    # Merely having FrameType/DisplayedTime is NOT a token-level correlation.
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        columns = reader.fieldnames or []
        rows = list(reader)
    displayed_column = next((x for x in ("DisplayedTime", "MsUntilDisplayed", "ScreenTime") if x in columns), None)
    display_rows = []
    if displayed_column:
        for r in rows:
            try:
                if float(r.get(displayed_column, "")) > 0:
                    display_rows.append(r)
            except (TypeError, ValueError):
                pass
    types = collections.Counter(r.get("FrameType", "unknown") for r in display_rows)
    return dict(rows=len(rows), displayed_column=displayed_column, displayed_rows=len(display_rows),
                frame_type_counts=dict(types),
                physical_chain_and_token_correlation_required=True,
                retry_required=not bool(display_rows))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--presentmon", type=Path)
    args = parser.parse_args()
    result = analyze(args.log.read_text(encoding="utf-8", errors="replace").splitlines())
    if args.presentmon:
        result["external_display_trace"] = display_summary(args.presentmon)
    print(json.dumps(result, indent=2))
if __name__ == "__main__":
    main()
