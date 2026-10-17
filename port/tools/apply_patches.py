# Applies the port's patches to freshly generated game code (recomp/output), in order. Every patch is idempotent,
# so this can be re-run after any partial regeneration.
#
#   python port/tools/apply_patches.py                  # the build patches
#   python port/tools/apply_patches.py --list           # what would run
#   python port/tools/apply_patches.py --diag raylog    # also a diagnostic patch from diag/ (repeatable)
#
# Paths come from ps2x_paths.py (PS2X_PORT_ROOT / PS2X_RECOMP_OUTPUT). The scripts in applied/ edited the runtime
# sources once and are already part of this checkout; they are kept for reference only.
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, TOOLS  # noqa: E402

# Order matters where patches match the same generated code:
#  - the instruction-semantics fixes run first (they rewrite generic patterns that later patches anchor next to);
#    the recompiler emits these correctly now, so on fresh output they find nothing to do;
#  - patch_fpu_sqrt rewrites FPU_SQRT_S operands, which patch_fpu_semantics then replaces: sqrt before semantics.
BUILD_PATCHES = [
    # instruction semantics (also fixed in ps2xRecomp)
    'patch_branch64.py',
    'patch_bc0.py',
    'patch_fpu_div.py',
    'patch_fpu_sqrt.py',
    'patch_fpu_semantics.py',
    'patch_vftoi_sat.py',
    # host replacements for guest library code
    'patch_softfloat.py',
    # game bug workarounds
    'patch_animloop.py',
    'patch_animqueue.py',
    'patch_spriteflush.py',
    'patch_camlock.py',
    # PC features
    'patch_60fps.py',
    'patch_widescreen.py',
    'patch_ws_enemyinfo.py',
    # native RenderWare renderer hooks
    'patch_nativeworld.py',
    'patch_nativenode.py',
    'patch_worldnode.py',
    'patch_im2d.py',
    # mod menu and restored cut content
    'patch_modmenu.py',
    'patch_cutcontent.py',
]

# Diagnostic patches that depend on another one.
DIAG_REQUIRES = {'rayprobe': ['raylog']}


def diag_script(name):
    for candidate in (f'patch_{name}.py', f'{name}.py', name):
        path = os.path.join(TOOLS, 'diag', candidate)
        if os.path.isfile(path):
            return path
    sys.exit(f'unknown diagnostic patch: {name} (see {os.path.join(TOOLS, "diag")})')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--list', action='store_true', help='print the patches that would run and exit')
    ap.add_argument('--diag', action='append', default=[], metavar='NAME', help='also apply diag/patch_NAME.py')
    args = ap.parse_args()

    scripts = [os.path.join(TOOLS, p) for p in BUILD_PATCHES]
    diags = []
    for name in args.diag:
        for dep in DIAG_REQUIRES.get(name, []) + [name]:
            if dep not in diags:
                diags.append(dep)
    scripts += [diag_script(d) for d in diags]

    if args.list:
        print('output:', OUTPUT)
        for s in scripts:
            print(' ', os.path.relpath(s, TOOLS))
        return
    if not os.path.isdir(OUTPUT):
        sys.exit(f'generated code not found: {OUTPUT} (set PS2X_PORT_ROOT or PS2X_RECOMP_OUTPUT)')

    for s in scripts:
        print(f'== {os.path.relpath(s, TOOLS)}', flush=True)
        r = subprocess.run([sys.executable, s])
        if r.returncode != 0:
            sys.exit(f'{os.path.basename(s)} failed (exit {r.returncode}); later patches were not applied')
    print('all patches applied')


if __name__ == '__main__':
    main()
