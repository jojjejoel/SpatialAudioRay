"""Stages the culled voice bank for Unreal import.

Reads out/manifest.json, picks the lowest surviving take per (line, bucket)
(culling = deleting rejected WAVs from out/), copies the winners to
export/Wavs/ and writes export/VoiceBank_<NPC>.csv as UE DataTable rows.

Import into UE:
  1. Drag export/Wavs/*.wav into the editor at /SpatialAudioRay/Voice/<NPC>/
  2. After the FNPCVoiceLineRow struct exists, import the CSV as a DataTable
     with that row type (same folder).

The CSV's soft object paths assume the /SpatialAudioRay/Voice/<NPC>/ location;
importing the WAVs elsewhere breaks the Sound column.
"""

import argparse
import csv
import json
import shutil
import sys
from pathlib import Path

CONTENT_ROOT = "/SpatialAudioRay/Voice"

# CSV category -> exact ENPCVoiceCategory entry name. DataTable enum import matches the
# entry name verbatim, and str.capitalize() would mangle the compound ones ("behindwall"
# becomes "Behindwall", not "BehindWall"), so the spelling is listed explicitly.
CATEGORIES = {
    "clear": "Clear",
    "occluded": "Occluded",
    "transition": "Transition",
    "aroundcorner": "AroundCorner",
    "behindwall": "BehindWall",
    "lostsight": "LostSight",
    "sightregained": "SightRegained",
    "partiallyoccluded": "PartiallyOccluded",
}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", default="out", help="generation output directory (manifest.json + WAVs)")
    p.add_argument("--export", default="export", help="staging directory for UE import")
    args = p.parse_args()

    base = Path(__file__).resolve().parent
    out_dir = base / args.out
    manifest_path = out_dir / "manifest.json"
    if not manifest_path.exists():
        sys.exit(f"No manifest at {manifest_path} — run generate_bank.py first.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    npc = manifest["npc"]

    chosen = {}
    for fname, e in manifest["entries"].items():
        if not (out_dir / fname).exists():
            continue
        key = (e["line_id"], e["bucket"])
        if key not in chosen or e["take"] < chosen[key][1]["take"]:
            chosen[key] = (fname, e)

    # Expected pairs come from the manifest itself — lines can whitelist buckets, so the
    # full lines × buckets product would flag deliberate absences.
    expected = {(e["line_id"], e["bucket"]) for e in manifest["entries"].values()}
    missing = sorted(expected - set(chosen))
    if missing:
        print("WARNING: no surviving take for:")
        for l, b in missing:
            print(f"  {l} / {b}")
    line_ids = sorted({k[0] for k in chosen})

    wav_dir = base / args.export / "Wavs"
    wav_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    for (line_id, bucket), (fname, e) in sorted(chosen.items()):
        category = (e["category"] or "").strip().lower()
        if category not in CATEGORIES:
            sys.exit(f"{fname}: unknown category '{category}' (expected one of {sorted(CATEGORIES)})")
        shutil.copy2(out_dir / fname, wav_dir / fname)
        stem = Path(fname).stem
        rows.append([
            f"{line_id}_{bucket.capitalize()}",
            line_id,
            bucket.capitalize(),
            CATEGORIES[category],
            (e.get("direction") or "none").capitalize(),
            e["cooldown_group"] or "",
            e["duration_sec"],
            f"{CONTENT_ROOT}/{npc}/{stem}.{stem}",
            e["text"],
        ])

    csv_path = base / args.export / f"VoiceBank_{npc}.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Name", "LineId", "Bucket", "Category", "Direction", "CooldownGroup", "Duration", "Sound", "Text"])
        w.writerows(rows)

    print(f"Exported {len(rows)} takes ({len(line_ids)} lines) for {npc}")
    print(f"  WAVs: {wav_dir}")
    print(f"  CSV:  {csv_path}")
    print(f"Import WAVs to {CONTENT_ROOT}/{npc}/ in the editor.")


if __name__ == "__main__":
    main()
