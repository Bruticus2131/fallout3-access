Tolk — Screen Reader Abstraction Library
========================================

Project: https://github.com/dkager/tolk
License: LGPL 3.0

Tolk is loaded *dynamically* by Fallout3Access at runtime via LoadLibrary,
so you do NOT need to link against Tolk.lib. You only need:

  1. Tolk.h (and TolkDotNet.h is irrelevant) — to compile the wrapper.
     Place it at: third_party/tolk/include/Tolk.h
  2. Tolk.dll plus its companion DLLs at runtime, in:
        <Fallout 3 GOTY>/Data/FOSE/Plugins/
     The companions you need to ship next to Tolk.dll are:
        SAAPI32.dll        (System Access)
        nvdaControllerClient32.dll (NVDA)
        jfwapi32.dll       (JAWS)
        ZoomTextAPI.dll    (ZoomText)
        dolapi32.dll       (Dolphin)
     The official Tolk release zip ships all of them — just drop the
     32-bit ones in.

If Tolk.dll is missing at runtime the plugin falls back to SAPI through
Windows directly (worst case), and logs a warning.
