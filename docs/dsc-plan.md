# Display Stream Compression (DSC), plan

Status, 2026-11-25: planned; host-only by Sunneva's choice. The rig's Samsung reports DPCD 1.2 / HBR2 (17.3 Gbit/s) over
DisplayPort and has no DP 1.4 setting, though its EDID lists 3840x2160@120 (1188 MHz): it cannot take DSC, so nothing here
can run on the card until someone with a DP 1.4 DSC monitor tries it.

## Why

3840x2160 at 120 Hz needs ~28.5 Gbit/s at 8 bits per colour, at 144 Hz ~32 Gbit/s; DP 1.4's HBR3 x4 carries 25.9. DSC
compresses the stream (to 8-12 bits per pixel) so both fit. The README lists it as not implemented.

## How (the project's route: Linux's code run on the host, its register writes replayed)

1. **Sink capabilities:** with DPCD 1.4 or later, read DPCD 0x060-0x06F with the rest at training (DSC_SUPPORT, algorithm
   revision, RC buffer, slice counts, max slice width, max bits per pixel and its increment, colour formats and depths,
   throughput) into `DpTrain::Sink`; log them. Host test: a sample DPCD dump decodes as Linux's `dc_dsc_parse_dsc_dpcd` does.
2. **Configuration and PPS:** the slice count, bits per pixel (the most the link fits, at most the sink's), RC parameters and
   the 128-byte picture parameter set. Linux: `dc_dsc_compute_config` (dc/dsc/dc_dsc.c), `drm_dsc_compute_rc_parameters` and
   `drm_dsc_pps_payload_pack` (drivers/gpu/drm/display/drm_dsc_helper.c, MIT: add to lib/linux's sparse set). Port the
   arithmetic into a freestanding `src/dsc.cpp` and hold it against the Linux functions on the host for a table of modes,
   sinks and links (byte-for-byte PPS).
3. **Register sequence:** a DSC scenario in tools/gen/pipegen: DML with DSC on, `stream->timing.flags.DSC` and `dsc_cfg`, a DSC
   engine (and ODM 2:1 where DML asks for it at 144 Hz) on the pipe; record `link_set_dpms_on` (DSC engine config and enable,
   OPTC bytes per pixel, the stream encoder's DSC control and PPS SDP, the sink's DSC_ENABLE at DPCD 0x160 as an AUX write).
   Tables for 3840x2160@120 and @144 on the DP connectors, for the second pipe and for a mode switch on the boot display
   (as test/ref/dp_retime_linux.inc). Host test: the kext's plan equals Linux's table.
4. **Kext:** `ModeSet`/`Pipe2` gain the DSC steps when the chosen mode needs compression; the link bandwidth check
   (`dp-modes-above-boot`) counts the compressed rate; `modes.cpp` offers 4K 120/144 only when the sink has DSC and the link fits.
5. **On a card, later:** needs a DP 1.4 DSC monitor: the mode, a still and a moving picture, display sleep/wake, mode switches
   back to 60 Hz; underflow checks (`logUnderflow`).

## Notes

- Regenerating today's tables from lib/linux (6addb4f38557) reproduces them exactly, except the six YCbCr 4:2:0 ones (one
  DIG_FIFO_CTRL0 entry in the committed tables, from 4f59ba9): check that before replacing them.
- HDMI 2.1 FRL (the other way to 4K 120) is a separate feature, not part of this.
