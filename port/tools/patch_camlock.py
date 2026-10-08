# Generated code: PcplSpriteBank::CacheLockCameraRaster locks the shell's 512x256x32 texture-camera
# raster into RenderWare's VRAM texture cache (RpSkyTexCacheRasterLock). An earlier revision of this
# script forced lock=FALSE (the main-menu sign texture did not fit the cache at the time); that left
# the raster unlocked, so SkyCameraBeginUpdate refused to render into it (its ext+0x17 "locked" flag
# stays 0) -> the match-type / arena panels stayed a flat 0xAA grey and the raster was re-uploaded
# (512 KB) every frame. The sign now fits, so this script only removes the old hack if present.
import re

path = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\CacheLockCameraRaster__14PcplSpriteBankFb_0x265140.cpp'
s = open(path, encoding='utf-8').read()
s2 = re.sub(r'    if \(!std::getenv\("PS2X_CAMLOCK"\)\) SET_GPR_U64\(ctx, 5, 0\); // patch_camlock[^\n]*\n', '', s)
if s2 != s:
    open(path, 'w', encoding='utf-8', newline='').write(s2)
    print('removed camlock hack')
else:
    print('no camlock hack present')
