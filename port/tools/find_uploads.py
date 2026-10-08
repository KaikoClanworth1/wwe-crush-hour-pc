import glob, os
files = [f for f in glob.glob('GameFiles/BUNDLES/**/*', recursive=True) if os.path.isfile(f)]
blobs = {f: open(f, 'rb').read() for f in files if 'SHELL' in f.upper() or 'MISC' in f.upper()}
for up in sorted(glob.glob('testrun/upload_*.bin')):
    data = open(up, 'rb').read()
    if len(data) < 4096:
        continue
    probe = data[256:256 + 64]
    hits = []
    for f, b in blobs.items():
        i = b.find(probe)
        if i >= 0:
            # measure how much of the upload matches contiguously from the aligned start
            start = i - 256
            n = 0
            while n < len(data) and start + n < len(b) and b[start + n] == data[n]:
                n += 1
            hits.append((os.path.basename(f), hex(start), n))
    print(os.path.basename(up), len(data), hits[:3] if hits else 'NOT FOUND')
