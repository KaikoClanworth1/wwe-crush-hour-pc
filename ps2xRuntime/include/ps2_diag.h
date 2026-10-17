#pragma once

// PS2X_DIAG: developer diagnostics compiled into the hot paths (branch ring, call trace, hitch sampler,
// autoinput anchors, scheduler event ring, sp check). On for native-host dev builds so the hidden test
// scripts (port/tools/*test.ps1) keep working; a shipping build configures with -DPS2X_ENABLE_DIAG=OFF.
#if !defined(PS2X_DIAG)
#if defined(PS2X_NATIVE_HOST) && !defined(PS2X_DISABLE_DIAG)
#define PS2X_DIAG 1
#else
#define PS2X_DIAG 0
#endif
#endif
