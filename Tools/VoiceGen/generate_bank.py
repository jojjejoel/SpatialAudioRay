"""Voice-bank generator for the SpatialAudioRay NPC voice system (pilot).

Renders every line in a CSV at every effort bucket via Chatterbox, using one
recorded reference clip per bucket, loudness-normalizes each take, and
maintains manifest.json alongside the WAVs.

Setup (Python 3.10-3.12):
    pip install -r requirements.txt      # read its torch note first — the build you
                                         # install decides whether this runs on GPU

Reference clips go in refs/, named after the buckets:
    refs/whisper.wav  refs/conversational.wav  refs/raised.wav  refs/shout.wav

Usage (from anywhere):
    python generate_bank.py                     # pilot lines, one take each, Turbo model
    python generate_bank.py --model standard    # A/B against the standard model
    python generate_bank.py --takes 3           # audition pool to pick from instead

Reroll workflow: delete the takes you don't like from out/, re-run — existing files
are skipped, deleted ones are re-rendered with fresh randomness, and the manifest is
pruned to match what's on disk. Generation is stochastic, so a re-render of the same
line is a genuinely different performance.
"""

import argparse
import csv
import inspect
import json
import math
import random
import sys
import time
from pathlib import Path

import numpy as np
import pyloudnorm as pyln
import torch
import torchaudio

# Starting points, tuned by ear during the pilot. Lower cfg_weight = slower,
# more deliberate pacing; expressive extremes want low cfg + high exaggeration.
# Keys not accepted by the loaded model's generate() (e.g. Turbo) are dropped,
# so on Turbo the effort difference comes entirely from the reference clips.
BUCKETS = {
    "whisper":        {"exaggeration": 0.40, "cfg_weight": 0.50},
    "conversational": {"exaggeration": 0.50, "cfg_weight": 0.50},
    "raised":         {"exaggeration": 0.65, "cfg_weight": 0.40},
    "shout":          {"exaggeration": 0.90, "cfg_weight": 0.30},
}

PEAK_CEILING = 10 ** (-1.0 / 20.0)  # -1 dBFS headroom after normalization

# Must match ENPCVoiceCategory; export_to_unreal.py maps these to the exact entry names.
CATEGORIES = ("clear", "occluded", "transition", "aroundcorner",
              "behindwall", "lostsight", "sightregained", "partiallyoccluded")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--lines", default="lines_pilot.csv",
                   help="CSV with id,category,cooldown_group,text and optional buckets "
                        "(space-separated whitelist; empty = all) and direction "
                        "(closer/farther, required on transition rows)")
    p.add_argument("--refs", default="refs", help="directory with <bucket>.wav reference clips")
    p.add_argument("--out", default="out", help="output directory for WAVs + manifest.json")
    p.add_argument("--npc", default="NPC01", help="filename prefix / NPC id")
    p.add_argument("--model", choices=["turbo", "standard"], default="turbo")
    p.add_argument("--takes", type=int, default=1,
                   help="renders per line and bucket; reroll a bad one by deleting it and "
                        "re-running, or raise this to render an audition pool up front")
    p.add_argument("--lufs", type=float, default=-18.0, help="integrated loudness target per take")
    p.add_argument("--out-sr", type=int, default=0, help="output sample rate; 0 = keep model native")
    p.add_argument("--device", default=None, help="cuda / cpu; default auto-detect")
    p.add_argument("--seed", type=int, default=None, help="base seed for reproducible takes; default random")
    p.add_argument("--overwrite", action="store_true", help="re-render takes that already exist")
    return p.parse_args()


def resolve(base: Path, p: str) -> Path:
    path = Path(p)
    return path if path.is_absolute() else base / path


def load_lines(path: Path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        missing = [k for k in ("id", "category", "text") if not (row.get(k) or "").strip()]
        if missing:
            sys.exit(f"{path.name}: row {row} is missing {missing}")
        bad = [b for b in line_buckets(row) if b not in BUCKETS]
        if bad:
            sys.exit(f"{path.name}: line {row['id']} lists unknown buckets {bad}")
        category = row["category"].strip().lower()
        if category not in CATEGORIES:
            sys.exit(f"{path.name}: line {row['id']} has unknown category '{category}' "
                     f"(expected one of {sorted(CATEGORIES)})")
        direction = (row.get("direction") or "").strip().lower()
        if direction not in ("", "closer", "farther"):
            sys.exit(f"{path.name}: line {row['id']} has invalid direction '{direction}'")
        if (row["category"].strip().lower() == "transition") != bool(direction):
            sys.exit(f"{path.name}: line {row['id']}: direction is required on transition "
                     f"rows and forbidden elsewhere")
    return rows


def line_buckets(row) -> list:
    """Bucket whitelist for one line; empty column = all. Lets direction-specific content
    (a 'you're running away' transition) render only at the efforts it can land in."""
    listed = (row.get("buckets") or "").split()
    return listed or list(BUCKETS)


def scheduling_metadata(row) -> dict:
    """CSV fields describing how a line is SCHEDULED, not what was said. Safe to refresh on
    an already-rendered take, because none of it is baked into the audio.

    `text` is deliberately NOT here: it is the words the take actually speaks, so refreshing
    it on a skipped render would make the bank claim wording the audio never said."""
    return {
        "category": row["category"],
        "direction": (row.get("direction") or "").strip().lower() or None,
        "cooldown_group": (row.get("cooldown_group") or "").strip() or None,
    }


def load_model(name: str, device: str):
    if name == "turbo":
        from chatterbox.tts_turbo import ChatterboxTurboTTS
        return ChatterboxTurboTTS.from_pretrained(device=device)
    from chatterbox.tts import ChatterboxTTS
    return ChatterboxTTS.from_pretrained(device=device)


def prepare_ref(src: Path, dst: Path, target_lufs: float = -27.0) -> Path:
    """Loudness-matched float32 mono copy of a reference clip.

    Chatterbox's own norm_loudness promotes the waveform to float64 (pyloudnorm
    returns a float64 scalar) and the float32 mel filters then crash on it — so
    we replicate its -27 LUFS ref normalization here in float32 and disable
    theirs via norm_loudness=False.
    """
    if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
        return dst
    wav, sr = torchaudio.load(str(src))
    x = wav.mean(dim=0).numpy().astype(np.float32)
    try:
        loudness = pyln.Meter(sr).integrated_loudness(x)
        if math.isfinite(loudness):
            x = x * np.float32(10.0 ** ((target_lufs - loudness) / 20.0))
    except ValueError:
        pass
    dst.parent.mkdir(parents=True, exist_ok=True)
    # Float WAV keeps >1.0 peaks from the boost intact, same as their in-memory norm.
    torchaudio.save(str(dst), torch.from_numpy(x).unsqueeze(0), sr, encoding="PCM_F", bits_per_sample=32)
    return dst


def normalize_loudness(x: np.ndarray, sr: int, target_lufs: float):
    """Returns (samples, measured_lufs_or_None, peak_limited)."""
    # Sub-400ms clips can't be measured; near-silent takes measure -inf.
    # Both pass through unnormalized and are visible in the manifest as
    # source_lufs = null.
    try:
        loudness = pyln.Meter(sr).integrated_loudness(x)
    except ValueError:
        return x, None, False
    if not math.isfinite(loudness):
        return x, None, False
    y = pyln.normalize.loudness(x, loudness, target_lufs)
    peak = float(np.max(np.abs(y))) if y.size else 0.0
    if peak > PEAK_CEILING:
        # Clamping keeps the file below actual target loudness — flagged so a
        # too-hot LUFS target shows up in the summary instead of silently
        # breaking the effort=timbre / loudness-parity premise.
        return y * (PEAK_CEILING / peak), loudness, True
    return y, loudness, False


def main():
    args = parse_args()
    base = Path(__file__).resolve().parent
    refs_dir = resolve(base, args.refs)
    out_dir = resolve(base, args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    lines = load_lines(resolve(base, args.lines))
    missing = [f"{b}.wav" for b in BUCKETS if not (refs_dir / f"{b}.wav").exists()]
    if missing:
        sys.exit(f"Missing reference clips in {refs_dir}: {', '.join(missing)}")
    prepared_refs = {b: prepare_ref(refs_dir / f"{b}.wav", out_dir / "_refs" / f"{b}.wav") for b in BUCKETS}

    device = args.device or ("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Loading Chatterbox ({args.model}) on {device} ...")
    model = load_model(args.model, device)
    sr = model.sr
    out_sr = args.out_sr or sr
    accepted = set(inspect.signature(model.generate).parameters)
    if args.model == "turbo":
        # Turbo accepts these in its signature but ignores them (no CFG /
        # exaggeration conditioning) — passing them would record tuning values
        # in the manifest that had no effect on the audio.
        accepted -= {"exaggeration", "cfg_weight", "min_p"}

    manifest_path = out_dir / "manifest.json"
    entries = {}
    if manifest_path.exists():
        entries = json.loads(manifest_path.read_text(encoding="utf-8")).get("entries", {})

    made, skipped, limited_files, restaled = 0, 0, [], []
    for line in lines:
        for bucket in line_buckets(line):
            settings = BUCKETS[bucket]
            kwargs = {k: v for k, v in settings.items() if k in accepted}
            call_kwargs = dict(kwargs)
            if "norm_loudness" in accepted:
                call_kwargs["norm_loudness"] = False  # refs are pre-normalized by prepare_ref
            ref = prepared_refs[bucket]
            for take in range(1, args.takes + 1):
                fname = f"{args.npc}_{line['id']}_{bucket}_t{take}.wav"
                fpath = out_dir / fname
                existing = entries.get(fname)
                if fpath.exists() and not args.overwrite and existing is not None \
                        and existing.get("text") == line["text"]:
                    skipped += 1
                    # Scheduling metadata isn't in the audio, so CSV edits to it reach the
                    # export without re-rendering.
                    existing.update(scheduling_metadata(line))
                    continue
                if fpath.exists() and not args.overwrite:
                    # The wording changed (or nothing records what this file says), so the
                    # audio on disk is no longer this line — re-render instead of shipping a
                    # take whose text the bank would misreport.
                    restaled.append(fname)

                seed = (args.seed + take) if args.seed is not None else random.randrange(1 << 31)
                torch.manual_seed(seed)
                t0 = time.time()
                wav = model.generate(line["text"], audio_prompt_path=str(ref), **call_kwargs)
                x = wav.detach().cpu().squeeze().numpy().astype(np.float32)
                duration = len(x) / sr

                y, src_lufs, limited = normalize_loudness(x, sr, args.lufs)
                if limited:
                    limited_files.append(fname)

                tensor = torch.from_numpy(y).unsqueeze(0)
                if out_sr != sr:
                    tensor = torchaudio.functional.resample(tensor, sr, out_sr)
                torchaudio.save(str(fpath), tensor, out_sr, encoding="PCM_S", bits_per_sample=16)

                entries[fname] = {
                    "line_id": line["id"],
                    "bucket": bucket,
                    "take": take,
                    **scheduling_metadata(line),
                    "text": line["text"],
                    "duration_sec": round(duration, 3),
                    "seed": seed,
                    **kwargs,
                    "source_lufs": None if src_lufs is None else round(src_lufs, 2),
                    "peak_limited": limited,
                }
                made += 1
                print(f"  {fname}  {duration:5.2f}s  (gen {time.time() - t0:4.1f}s)")

    # Culled takes are deleted on disk; their manifest rows must not linger.
    entries = {f: e for f, e in entries.items() if (out_dir / f).exists()}

    # export_to_unreal.py builds the bank from the manifest, not from the CSV, so anything
    # the CSV no longer backs would keep shipping: lines that were removed, and takes
    # rendered from wording that has since changed (their siblings at higher take numbers
    # are not re-rendered, and would be promoted the moment take 1 is culled). Drop them
    # from the manifest; the WAVs stay on disk, they simply stop being part of the bank.
    csv_text = {row["id"]: row["text"] for row in lines}
    dropped_lines = sorted(f for f, e in entries.items() if e["line_id"] not in csv_text)
    dropped_text = sorted(f for f, e in entries.items()
                          if e["line_id"] in csv_text and e.get("text") != csv_text[e["line_id"]])
    for fname in dropped_lines + dropped_text:
        entries.pop(fname, None)

    manifest_path.write_text(json.dumps({
        "npc": args.npc,
        "model": args.model,
        "sample_rate": out_sr,
        "lufs_target": args.lufs,
        "bucket_settings": BUCKETS,
        "entries": dict(sorted(entries.items())),
    }, indent=2), encoding="utf-8")

    print(f"\nDone: {made} rendered, {skipped} skipped (existing), {len(entries)} takes in manifest.")
    print(f"Output: {out_dir}")
    if restaled:
        print(f"\nRe-rendered {len(restaled)} take(s) whose wording had changed since the "
              f"audio was made:")
        for f in restaled:
            print(f"  {f}")
    if dropped_lines or dropped_text:
        print(f"\nDropped from the bank ({len(dropped_lines)} line(s) no longer in "
              f"{args.lines}, {len(dropped_text)} take(s) of superseded wording). The WAVs "
              f"are still in {out_dir.name} if you want them back:")
        for f in dropped_lines + dropped_text:
            print(f"  {f}")
    if limited_files:
        print(f"\nPeak-limited (LUFS target {args.lufs} may be too hot for these):")
        for f in limited_files:
            print(f"  {f}")


if __name__ == "__main__":
    main()
