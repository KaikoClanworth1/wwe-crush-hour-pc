# Paths shared by the port's tools. Override with environment variables instead of editing scripts:
#   PS2X_PORT_ROOT      the port's working tree: GameFiles/, recomp/, testrun/ (default: the original dev machine)
#   PS2X_RECOMP_OUTPUT  generated game code (default: <PS2X_PORT_ROOT>/recomp/output)
# The runtime sources are always the checkout this file lives in.
import os

# realpath: the dev tree reaches port/tools through a <root>/tools directory junction.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
RUNTIME = os.path.join(REPO, 'ps2xRuntime')
TOOLS = os.path.dirname(os.path.realpath(__file__))

ROOT = os.environ.get('PS2X_PORT_ROOT', r'D:\Xbox Games Ports\WWE Crush Hour')
OUTPUT = os.environ.get('PS2X_RECOMP_OUTPUT', os.path.join(ROOT, 'recomp', 'output'))


def out(*parts):
    """A path inside the generated game code."""
    return os.path.join(OUTPUT, *parts)


def runtime(*parts):
    """A path inside this checkout's ps2xRuntime."""
    return os.path.join(RUNTIME, *parts)


def root(*parts):
    """A path inside the port's working tree."""
    return os.path.join(ROOT, *parts)
