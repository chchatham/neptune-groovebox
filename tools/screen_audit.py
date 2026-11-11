#!/usr/bin/env python3
"""Screen audit: every screen, every page, every knob, on the real device.

The app does the work (src/ui/ScreenAudit.h, switched on by NEPTUNE_AUDIT_SCREENS): it sends
itself the same messages the Launchkey sends, turns every knob one click up and back, and checks
each picture it draws (text collisions, text off the glass, dead knobs, pictures that do not
come back, bank-button paging, and that the device was sent the picture that was drawn).

This script runs it and, with --photos, also photographs the Launchkey's display on every page
with the Mac's camera and compares each photo with the picture the app drew.

    tools/screen_audit.py [--photos] [--out DIR] [--box X,Y,W,H]

    --photos   stop on every page, photograph the glass, score photo against the drawn picture
    --out      results folder (default .ralph/screen-audit; it is replaced)
    --box      where the lit display is in the 1920x1080 photo (default: found from the first photo)

Results in DIR:
    report.txt      one line per finding, then the totals
    pages/          every page as drawn (PBM)
    findings/       the picture that shows each finding
    photos/         with --photos: the display cut out of each photo, and photos.txt with a score
                    per page (1.0 = the photo matches the drawn picture; low scores first)

Needs the Launchkey on Neptune firmware, the optimised build (build-release), and for --photos
`imagesnap` and the camera pointed at the display. Nothing else may be using the display: quit
the app first. No project is loaded or saved by the audit.
"""
import argparse, os, struct, subprocess, sys, time, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "build-release/NeptuneGroovebox_artefacts/Release/Neptune Groovebox.app/Contents/MacOS/Neptune Groovebox")


def read_pbm(path):
    t = open(path).read().split()
    w, h = int(t[1]), int(t[2])
    bits = [int(c) for c in ''.join(t[3:])]
    return [bits[y * w:(y + 1) * w] for y in range(h)]


def read_bmp_gray(path):
    """24- or 32-bit uncompressed BMP (what `sips -s format bmp` writes) as rows of 0..255."""
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    bpp, = struct.unpack_from('<H', d, 28)
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(abs(h)):
        base = off + y * stride
        rows.append([(d[base + x * step] + d[base + x * step + 1] * 2 + d[base + x * step + 2]) // 4 for x in range(w)])
    return rows[::-1] if h > 0 else rows


def sips(*args):
    subprocess.run(["sips", *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)


def find_box(photo, tmp):
    """Bounding box of the lit display in a 1920x1080 photo: the bright, blue-white block in the
    upper half that is far wider than a letter and not a pad (pads are below it and larger)."""
    small = os.path.join(tmp, "find.bmp")
    sips("-s", "format", "bmp", "-z", "270", "480", photo, "--out", small)
    g = read_bmp_gray(small)
    # Columns / rows with enough bright pixels, in the top 60 % of the picture.
    limit = int(270 * 0.3)
    bright = [[1 if v > 150 else 0 for v in row] for row in g[:limit]]
    cols = [sum(bright[y][x] for y in range(limit)) for x in range(480)]
    xs = [x for x in range(480) if cols[x] >= 3]
    if not xs:
        return None
    # The widest run of bright columns (gaps of up to 3 allowed).
    best, start, prev = (0, 0), xs[0], xs[0]
    for x in xs[1:] + [10 ** 6]:
        if x - prev > 3:
            if prev - start > best[1] - best[0]:
                best = (start, prev)
            start = x
        prev = x
    x0, x1 = best
    rows = [y for y in range(limit) if sum(bright[y][x0:x1 + 1]) >= 3]
    if not rows:
        return None
    y0, y1 = rows[0], rows[-1]
    # The drawn area is 128 x 64; the lit part of a page starts a pixel or two inside it.
    px = (x1 - x0 + 1) * 4 / 124.0
    return (int(x0 * 4 - 2 * px), int(y0 * 4 - 1.5 * px), int(128 * px), int(64 * px))


def score(expected, gray):
    """Correlation between the drawn picture (128 x 64, 0/1) and the photo cut to the same size,
    best over small shifts; and the worst of its 32 blocks of 16 x 16."""
    blur = [[0.0] * 128 for _ in range(64)]
    for y in range(64):
        for x in range(128):
            s = n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < 64 and 0 <= xx < 128:
                        s += expected[yy][xx]; n += 1
            blur[y][x] = s / n

    def corr(dx, dy, x0=0, y0=0, w=128, h=64):
        a, b = [], []
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                yy, xx = y + dy, x + dx
                if 0 <= yy < 64 and 0 <= xx < 128:
                    a.append(blur[y][x]); b.append(gray[yy][xx])
        n = len(a)
        if n < 16:
            return 0.0
        ma, mb = sum(a) / n, sum(b) / n
        va = sum((v - ma) ** 2 for v in a); vb = sum((v - mb) ** 2 for v in b)
        if va < 1e-9 or vb < 1e-9:
            return 1.0 if va < 1e-9 and vb < 30.0 * n else 0.0     # an empty block photographed dark
        return sum((a[i] - ma) * (b[i] - mb) for i in range(n)) / (va * vb) ** 0.5

    best = max(((corr(dx, dy), dx, dy) for dx in range(-3, 4) for dy in range(-3, 4)))
    whole, dx, dy = best
    worst = min(corr(dx, dy, bx * 16, by * 16, 16, 16) for by in range(4) for bx in range(8))
    return whole, worst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--photos", action="store_true")
    ap.add_argument("--out", default=os.path.join(ROOT, ".ralph/screen-audit"))
    ap.add_argument("--box")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    if subprocess.run(["pgrep", "-f", "MacOS/Neptune Groovebox"], stdout=subprocess.DEVNULL).returncode == 0:
        sys.exit("The app is running: quit it first (the audit needs the display to itself).")
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, NEPTUNE_AUDIT_SCREENS=out)
    if args.photos:
        env["NEPTUNE_AUDIT_PHOTOS"] = "1"
    log = open(os.path.join(out, "app.log"), "w")
    app = subprocess.Popen([APP], env=env, stdout=log, stderr=subprocess.STDOUT)

    box = tuple(int(v) for v in args.box.split(",")) if args.box else None
    shots, photos = os.path.join(out, "shots"), os.path.join(out, "photos")
    results, started = [], time.time()
    try:
        while app.poll() is None:
            if time.time() - started > 3600:
                app.kill(); sys.exit("Gave up after an hour.")
            if not args.photos:
                time.sleep(0.5); continue
            todo = sorted(p for p in glob.glob(os.path.join(shots, "*.pbm")) if not os.path.exists(p[:-4] + ".done"))
            if not todo:
                time.sleep(0.05); continue
            pbm = todo[0]; stem = os.path.basename(pbm)[:-4]
            for _ in range(20):                       # the label is written just after the picture
                if os.path.exists(pbm[:-4] + ".txt"): break
                time.sleep(0.02)
            where = open(pbm[:-4] + ".txt").read().strip() if os.path.exists(pbm[:-4] + ".txt") else stem
            os.makedirs(photos, exist_ok=True)
            time.sleep(0.25)                          # the frame reaches the glass in about 80 ms
            full = os.path.join(photos, "full.jpg")
            subprocess.run(["imagesnap", "-q", "-d", "FaceTime HD Camera", "-w", "1.0", full],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            line = None
            try:
                if box is None:
                    box = find_box(full, photos)
                    print("display found at x,y,w,h =", box, flush=True)
                x, y, w, h = box
                crop = os.path.join(photos, stem + ".jpg")
                sips("-c", str(h), str(w), "--cropOffset", str(max(0, y)), str(max(0, x)), full, "--out", crop)
                bmp = os.path.join(photos, "cmp.bmp")
                sips("-s", "format", "bmp", "-z", "64", "128", crop, "--out", bmp)
                whole, worst = score(read_pbm(pbm), read_bmp_gray(bmp))
                results.append((whole, worst, stem, where))
                line = f"{stem}  match {whole:.2f}  worst block {worst:.2f}  {where}"
            except Exception as e:                    # a failed photo must not stop the audit
                line = f"{stem}  photo failed ({e})  {where}"
            print(line, flush=True)
            open(pbm[:-4] + ".done", "w").close()
    finally:
        if app.poll() is None:
            app.terminate()
    if results:
        results.sort()
        with open(os.path.join(photos, "photos.txt"), "w") as f:
            f.write("match  worst-block  page   (lowest first; 1.00 = the photo is the drawn picture)\n")
            for whole, worst, stem, where in results:
                f.write(f"{whole:5.2f}  {worst:5.2f}  {stem}  {where}\n")
        for tmp in ("full.jpg", "cmp.bmp", "find.bmp"):
            p = os.path.join(photos, tmp)
            if os.path.exists(p): os.remove(p)
    report = os.path.join(out, "report.txt")
    if os.path.exists(report):
        print(''.join(open(report).readlines()[-6:]))
        print("report:", report)
    else:
        print("No report was written; see", os.path.join(out, "app.log"))


if __name__ == "__main__":
    main()
