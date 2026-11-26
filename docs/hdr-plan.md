# HDR output (HDR10 to the monitor), plan

Status, 2026-11-26: with `rdna4-hdr=1` (and `rdna4-vrr=1`, which serves the timing range) macOS offers HDR on the rig's
Samsung and switches it on and off. Nothing is sent to the monitor for it yet: the link stays 8 bits a component with no
HDR metadata, so the picture with HDR on is not right yet. Off by default.

## What the monitors and macOS have

- The Samsung (Odyssey G70D, boot display, DP) has an HDR Static Metadata block: EOTFs traditional SDR + SMPTE ST 2084
  (PQ), maximum luminance code 96 (about 400 nits), and a colorimetry block with BT.2020 RGB and YCC. The Lenovo has
  neither, so it gets no HDR modes (`Edid::takesPq`).
- CoreDisplay offers HDR on a display (`CoreDisplay::Display::SupportsHDRMode`, Tahoe 26.6.2) only if, for the current
  mode, `FindIOModeIDForIODynamicRange` finds another mode whose timing is equal (`DetailedTimingsEqual`) and whose
  `IODetailedTimingInformationV2::dynamicRange` is HDR10. The colour bits in the timing range (IOFBTimingRange) alone are
  not enough: they reach the registry but CoreDisplay still said no.
- Through IONDRV the colour fields travel in the records IOMacOSVideo.h still calls reserved: VDDetailedTimingRec
  csReserved3/4 = pixelEncoding | bitsPerColorComponent, colorimetry | dynamicRange; VDDisplayTimingRangeRec
  csReserved6/7 = the supported masks. The translator names them after IOGraphicsTypes.h (static_asserts in plugin.cpp).

## Step 1 (done, 2026-11-26)

Every table mode is listed a second time after the modes (and the scaled ones), ID | 0x20000000, with 10 bpc, BT.2020 and
HDR10 in its timing; the modes themselves say 8 bpc, sRGB, SDR. A twin switches like its mode (no hardware change) and is
never the default. On the rig: `hdrprobe` (CoreDisplay_Display_SupportsHDRMode) says 1 for the Samsung, 0 for the
Lenovo; `hdrprobe <id> on` makes CoreDisplay set mode 0x20000066 at depth 0 (32 bpp, unchanged), NSScreen's potential
EDR goes 1.0 -> 2.0; `off` sets 0x66 again.

## Next

2. Tell the backend when the mode is an HDR twin (`Translator::isHdrId(current)`), and on DP send the HDR static
   metadata (SDP type 0x87, CTA-861-G Dynamic Range and Mastering InfoFrame: EOTF = PQ, the display's primaries and
   luminance from the EDID or macOS's IOFBHDRMetaData via kIOFBHDRMetaDataAttribute) through a DP_SEC GSP slot, as Linux
   (dcn401 stream encoder, `dc_stream.hdr_static_metadata`); signal BT.2020 in the MSA MISC bits or a VSC SDP. Card
   check: the Samsung's OSD reports HDR.
3. 10 bits a component on the link (FMT bit depth, DIG pixel depth, MSA) for the twins; check the DP bandwidth at
   2560x1440@144 and 4K (DSC).
4. What WindowServer renders in HDR mode at 32 bpp (PQ-encoded 8 bit? a 10-bit or FP16 surface would need another depth
   mode, kDepthMode2 with 30-bit pixels) and whether the plane's gamma (cscSetGamma) has to be linear in HDR.
5. HDMI: the same InfoFrame through the HDMI generic packets (the Lenovo is SDR only, so no rig test yet).
