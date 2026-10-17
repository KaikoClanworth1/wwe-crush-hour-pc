# Compares a scripted test run's screenshots and log with a saved baseline (used by regress.ps1).
#
#   python compare_shots.py <baseline dir> <run dir> [--threshold 12] [--window 2]
#
# Each dir holds shots/*.bmp and stdout.txt from bgtest.ps1. Screenshots are taken on wall-clock time, so a run
# drifts by a frame or two against its baseline: every shot is matched with the closest baseline shot within
# +-window positions, on downscaled grey images (mean absolute difference, 0..255). Shots whose best match is
# above the threshold are reported. The log is checked for crashes, missing functions and the frame rate.
# Exit code: 0 = pass, 1 = regression, 2 = bad arguments / no data.
import argparse
import glob
import os
import re
import sys

from PIL import Image, ImageChops, ImageFilter, ImageStat

THUMB = (160, 90)
FAILURE_PATTERNS = [
    (re.compile(r'\[terminate\]'), 'terminate handler ran'),
    (re.compile(r'missing function|Function not found', re.I), 'missing recompiled function'),
    (re.compile(r'Unhandled exception|access violation', re.I), 'host exception'),
]
# PS2X_STATUS=1 prints "[frames] vif1kicks=N rate=R/s" every 2 s: VIF1 kicks per second, a frame-rate proxy.
FRAMES = re.compile(r'\[frames\][^\n]*?rate=(\d+(?:\.\d+)?)/s')


def thumbs(run_dir):
    out = []
    for f in sorted(glob.glob(os.path.join(run_dir, 'shots', '*.bmp'))):
        try:
            im = Image.open(f).convert('L').resize(THUMB, Image.BILINEAR).filter(ImageFilter.GaussianBlur(1))
        except OSError:
            continue  # a shot being written when the run was killed
        out.append((os.path.basename(f), im))
    return out


def diff(a, b):
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def log_report(run_dir):
    path = os.path.join(run_dir, 'stdout.txt')
    if not os.path.isfile(path):
        return [], None
    text = open(path, encoding='utf-8', errors='replace').read()
    problems = [msg for rx, msg in FAILURE_PATTERNS if rx.search(text)]
    fps = [float(m) for m in FRAMES.findall(text)]
    return problems, (sum(fps) / len(fps) if fps else None)


def compare(baseline, run, threshold, window):
    base, cur = thumbs(baseline), thumbs(run)
    if not base or not cur:
        print(f'no screenshots (baseline {len(base)}, run {len(cur)})')
        return 2
    failed = False
    if len(cur) < len(base) * 0.9:
        print(f'FAIL run has {len(cur)} shots, baseline {len(base)} (stopped early / crashed?)')
        failed = True

    bad = []
    worst = 0.0
    for i, (name, im) in enumerate(cur):
        lo, hi = max(0, i - window), min(len(base), i + window + 1)
        if lo >= hi:
            continue
        best = min(diff(im, base[j][1]) for j in range(lo, hi))
        worst = max(worst, best)
        if best > threshold:
            bad.append((name, best))
    print(f'shots {len(cur)} (baseline {len(base)}), worst match {worst:.1f}, over threshold {len(bad)}')
    for name, d in bad[:20]:
        print(f'  {name}: {d:.1f}')
    if bad:
        failed = True

    base_problems, base_fps = log_report(baseline)
    problems, fps = log_report(run)
    for p in problems:
        if p not in base_problems:
            print(f'FAIL log: {p}')
            failed = True
    if fps is not None and base_fps is not None:
        print(f'vif1 kick rate {fps:.1f}/s (baseline {base_fps:.1f}/s)')
        if fps < base_fps * 0.9:
            print('FAIL frame rate (VIF1 kick rate) dropped by more than 10%')
            failed = True
    print('FAIL' if failed else 'PASS')
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('baseline')
    ap.add_argument('run')
    ap.add_argument('--threshold', type=float, default=12.0, help='max mean grey difference per shot (0..255)')
    ap.add_argument('--window', type=int, default=2, help='baseline shots either side to match against')
    args = ap.parse_args()
    sys.exit(compare(args.baseline, args.run, args.threshold, args.window))


if __name__ == '__main__':
    main()
