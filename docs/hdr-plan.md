# HDR output (HDR10 to the monitor), plan

Status, 2026-11-26: with `rdna4-hdr=1` (and `rdna4-vrr=1`, which serves the timing range) macOS offers HDR on the rig's
Samsung, and switching it on sends the HDR static metadata (PQ): the Samsung's "HDR Tone Mapping" goes active with HDR
on and greyed out with it off, and the picture looked normal. BT.2020 is signalled in a VSC SDP (link stable; not yet
judged by eye). The link stays 8 bits a component. Off by default.

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

## Step 2 (done, 2026-11-26)

After every switch the translator tells the backend whether the mode is an HDR twin (`Backend::setHdr`). The boot
display's backend (`RDNA4Device::setBootHdr`, DP only) writes the HDR static metadata SDP (type 0x87, version 0x13,
CTA-861-G Dynamic Range and Mastering InfoFrame with EOTF = PQ) into VPG generic packet 3 and sets DP_SEC_GSP3_ENABLE
and the secondary stream's master enable, as Linux (vpg3_update_generic_info_packet,
enc3_stream_encoder_update_dp_info_packets); off clears GSP3. The mastering primaries, luminance, MaxCLL and MaxFALL are
0 ("unknown"). Card: DP0_DP_SEC_CNTL 0x00800001 with HDR on, the Samsung's tone mapping follows.

## Step 3 (done, 2026-11-26): BT.2020

If the sink takes colorimetry from a VSC SDP (DPCD 0x2210 bit 3; the Samsung reads 0x08), HDR on also sends a VSC SDP
revision 5 (mod_build_vsc_infopacket: DB16 = RGB | BT.2020 RGB, DB17 = 8 bpc full range) in VPG packet 0 with
DP_SEC_GSP0_ENABLE, and sets MSA MISC1 bit 6 so the sink takes colorimetry from it (dcn401 set_stream_attribute).
HDR off clears GSP0 and the bit, which leaves the MSA's own colorimetry as before. Card: DP_SEC_CNTL 0x00900001,
DP_MSA_MISC 0x40, no HPD drop or retrain afterwards; macOS kept HDR on across a reboot and the packets went out at its
first switch. Not judged by eye yet (colours should look the same as with PQ alone on a monitor that assumed BT.2020).

## Next

- Display sleep and wake with HDR on passed on the card (2026-11-26): macOS switches the mode again after the wake, so the
  packet is sent again and the Samsung's tone mapping came back active. Not run: system sleep and a resolution change
  while in HDR.
- Fill the mastering data from macOS's IOFBHDRMetaData (kIOFBHDRMetaDataAttribute) or the EDID.
- 10 bits a component on the link (FMT bit depth, DIG pixel depth, MSA) for the twins; check the DP bandwidth at
  2560x1440@144 and 4K (DSC).
- What WindowServer renders in HDR mode at 32 bpp (PQ-encoded 8 bit? a 10-bit or FP16 surface would need another depth
  mode, kDepthMode2 with 30-bit pixels) and whether the plane's gamma (cscSetGamma) has to be linear in HDR.
- HDMI and the second pipe: the same InfoFrame through the HDMI generic packets, and setHdr on head 2's backend
  (the Lenovo is SDR only, so no rig test yet).
