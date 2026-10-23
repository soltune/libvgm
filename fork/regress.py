#!/usr/bin/env python3.14
"""Render the files in fork/regress.list with build/bin/vgm2wav and compare with reference renders.

usage: fork/regress.py            compare (reference WAVs in $LIBVGM_REGRESS, default ~/tools/libvgm-regress)
       fork/regress.py --update   make the current renders the new references

A render that is bit-identical prints "same". One that differs prints the correlation with the
reference (aligned within +-50 samples) and its level difference; below 0.99 it counts as FAIL.
Upstream changes such as resampler fixes legitimately move bits, so read "changed" lines, do not
just count them.
"""
import os, sys, subprocess, wave, shutil
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.environ.get('LIBVGM_REGRESS', os.path.expanduser('~/tools/libvgm-regress'))
PLAYER = os.path.join(ROOT, 'build', 'bin', 'vgm2wav')


def render(src, dst):
    subprocess.run([PLAYER, '--loops', '1', '--fade', '0', src, dst], check=True,
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read(p):
    w = wave.open(p)
    return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, w.getnchannels()).astype(float)


def compare(a, b):
    n = min(len(a), len(b)) - 100
    x = a[50:n].mean(1)
    best = (-2, 0)
    for lag in range(-50, 51):
        y = b[50 + lag:n + lag].mean(1)
        c = float(np.dot(x, y) / (np.sqrt(np.dot(x, x) * np.dot(y, y)) + 1e-9))
        best = max(best, (c, lag))
    c, lag = best
    y = b[50 + lag:n + lag].mean(1)
    db = 10 * np.log10((np.dot(x, x) + 1e-9) / (np.dot(y, y) + 1e-9))
    return c, lag, db, len(a) - len(b)


def main():
    update = '--update' in sys.argv
    lst = [l.strip() for l in open(os.path.join(ROOT, 'fork', 'regress.list')) if l.strip() and not l.startswith('#')]
    os.makedirs(os.path.join(REF, 'out'), exist_ok=True)
    fails = 0
    for src in lst:
        src = os.path.expanduser(src)
        name = os.path.basename(src).rsplit('.', 1)[0] + '.wav'
        out = os.path.join(REF, 'out', name)
        ref = os.path.join(REF, name)
        render(src, out)
        if update or not os.path.exists(ref):
            shutil.copy(out, ref)
            print(f'reference  {name}')
            continue
        if open(out, 'rb').read() == open(ref, 'rb').read():
            print(f'same       {name}')
            continue
        c, lag, db, dn = compare(read(out), read(ref))
        bad = c < 0.99
        fails += bad
        print(f'{"FAIL" if bad else "changed":10s} {name}: corr {c:.4f} lag {lag} level {db:+.2f} dB length {dn:+d}')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
