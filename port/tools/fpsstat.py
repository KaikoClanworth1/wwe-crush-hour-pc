# Frame-rate summary of a test run's stdout ([frames] lines every 2 s): the match part starts at the first sample
# after <from_sec>. Usage: python tools/fpsstat.py <stdout.txt> [from_sec=150]
import re, sys

path = sys.argv[1]
from_sec = float(sys.argv[2]) if len(sys.argv) > 2 else 150.0
rates = [float(m.group(1)) for m in re.finditer(r'\[frames\][^\[]*?rate=([\d.]+)', open(path, encoding='latin1').read())]
sel = rates[int(from_sec / 2):]
if not sel:
    print('no samples')
    sys.exit(1)
sel_sorted = sorted(sel)
print(f'samples={len(sel)} mean={sum(sel) / len(sel):.1f} min={sel_sorted[0]:.1f} p10={sel_sorted[len(sel) // 10]:.1f} '
      f'>=58: {100.0 * sum(1 for r in sel if r >= 58.0) / len(sel):.0f}%  <45: {sum(1 for r in sel if r < 45.0)}')
