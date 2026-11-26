# Display Stream Compression (DSC), plan

Status, 2026-11-26: steps 1-3 done on the host; on the card DSC WORKS at 3840x2160@60 (forced on), 3840x2160@144 shows noise (below). The rig's Samsung reports DPCD 1.2 / HBR2 (17.3 Gbit/s) over
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
   **Done:** `src/dsc.cpp` (`Dsc::choose`, `Dsc::pps`; integer only, the QP tables from `tools/gen/pipegen/mkqp.py`).
   `tools/gen/pipegen/dscgen.sh` runs Linux's own code (dc_dsc.c, dcn20/dcn401_dsc.c, rc_calc*.c, drm_dsc_helper.c) over
   3 sinks x 12 timings (4K 60-240 Hz, 1440p240, 5K, 8K, 1080p) x 8/10/12 bpc x 4 links x 2 clock sets into
   test/ref/dsc_linux.inc; `make test` (test/dsc-test.cpp) requires the same decision, configuration, 128-byte PPS, OPTC
   bytes per pixel, RC buffer size and chunk size: 864 cases, 308 with DSC, all equal. Learned on the way: amdgpu counts
   the DSC stream overhead only when the sink's branch caps (DPCD 0x0A0) are passed, as the SST path does.
3. **Register sequence:** a DSC scenario in tools/gen/pipegen: DML with DSC on, `stream->timing.flags.DSC` and `dsc_cfg`, a DSC
   engine (and ODM 2:1 where DML asks for it at 144 Hz) on the pipe; record `link_set_dpms_on` (DSC engine config and enable,
   OPTC bytes per pixel, the stream encoder's DSC control and PPS SDP, the sink's DSC_ENABLE at DPCD 0x160 as an AUX write).
   Tables for 3840x2160@120 and @144 on the DP connectors, for the second pipe and for a mode switch on the boot display
   (as test/ref/dp_retime_linux.inc). Host test: the kext's plan equals Linux's table.
   **Done so far:** pipegen's `dplink d S P dsc` scenario: a sink with a DSC 1.2 decoder and FEC (kind d), the configuration
   from Linux's own `dc_dsc_compute_config` (on the link of `PIPEGEN_DP_LINK`, HBR3 x4 by default), DML with DSC, DSC
   engines in the pool (their constructor's clock-gating writes left out of the trace). The recorded `link_set_dpms_on`
   has FEC ready (DPCD 0x120) before training, the DSCCLK DTO (phase = modulo = 1), the engine's registers
   (`dsc_write_to_registers`) and `dsc401_enable` (forward to the pipe's OPP), the OPTC's DSC mode, bytes per pixel and
   slice width, the PPS as four VPG secondary-data packets after DSC_ENABLE (DPCD 0x160) = 1, and the same again on
   wake; the engine set up a second time in the plane programming. `run.sh` writes test/ref/dsc_regs_linux.inc for 11
   streams (4K 60-144 Hz, 1440p at 645 MHz; HBR3/HBR2, x1 to x4: 9.125 to 16 bpp, 2 or 4 slices) and `src/dsc.cpp`'s
   `engineRegs` and `ppsPackets` give the same values (`make test`). Linux's choice needs DISPCLK 1.29 GHz at 4K120 and
   1.38 GHz at 4K144 on one pipe (no ODM in DML at 2 GHz), well above the 650 MHz the kext holds: step 4 raises it.
   Left for step 3: the kext's plan (the static part of the trace as a table, the computed values put in) and its host
   test against the whole trace; ODM 2:1 (DML wants it at 5K) is not modelled.
4. **Kext:** `ModeSet`/`Pipe2` gain the DSC steps when the chosen mode needs compression; the link bandwidth check
   (`dp-modes-above-boot`) counts the compressed rate; `modes.cpp` offers 4K 120/144 only when the sink has DSC and the link fits.
5. **On a card, later:** needs a DP 1.4 DSC monitor: the mode, a still and a moving picture, display sleep/wake, mode switches
   back to 60 Hz; underflow checks (`logUnderflow`).

## Card tests (2026-11-26, rig: the Samsung on DP0)

The Samsung is DSC-capable after all: its DPCD 0x000 says 1.2 / HBR2 for old sources, the extended block at 0x2200 says 1.4 /
HBR3 (the kext reads it since fb3348c, as Linux does), and DPCD 0x060 has a DSC 1.2 decoder (`012100762b010100000f0e0108000000`:
slices 1/2/4/8 up to 2560 wide, 10-bit line buffer, block prediction, 340 MP/s a slice). `rdna4-dscboot=<n>` (with
`rdna4-dispclk=1400`, which also raises DPPCLK; the card's DISPCLK/DPPCLK go to 2 GHz) lets the cold start light pipe 0 again from
one of three Linux tables (tools/gen/pipegen/run.sh, the sink's caps from `PIPEGEN_DSC_CAPS`) on four HBR3 lanes:

| test | stream | the Samsung (Sunneva's eyes) | sink DSC_STATUS after clearing |
|---|---|---|---|
| 1 | 3840x2160@144 (1332.75 MHz), DSC 4 slices, 16 bpp | **noise** (5 boots, with each fix below) | 0x07 (chunk-length error stays) |
| 2 | 3840x2160@60 (533.25 MHz), DSC forced, 2 slices | **normal picture** | 0x03 |
| 2 | the same with 4 slices (the 2-slice caps bit hidden) | **normal picture** | 0x03 |
| 3 | 3840x2160 at 729 MHz (82 Hz), no DSC | black (the monitor may just refuse 82 Hz) | - |

So the DSC set-up is right: engine, PPS packets, FEC, DSC_ENABLE, our HBR3 training, 2 and 4 slices. Bits 0-1 of DSC_STATUS stay
set on this sink with a good picture; bit 2 (chunk-length error) is the one that tells a broken stream. What fails is the
high-pixel-rate path: above DPREFCLK (720 MHz) Linux runs the DIG at two pixels a cycle (`dcn401_build_pipe_pix_clk_params`), the
DP DTO gets an integer part, DP_VID_TIMING its N interval. Test 3 meant to tell that path from DSC; its black screen does not.

Fixes found on the way (all in the generator, kept): `dcn401_init_hw` power-gates the DSC engines and only the full commit
(`dcn32_update_dsc_pg`) ungates them: the "dsc" scenario now does; FEC ready is set inside training at Linux's point (after the
sink is in D0), by `DpTrain` for a `Link::fec`; pipegen took one pixel a cycle for every DP stream, Linux's rule is now used
(no existing table changes: they are all below 720 MHz). Register dump of a 144 Hz boot: DSC powered, clocked, forwarding to OPP0,
no overflow; OPTC DSC mode, 2.0 bytes per pixel, 960-pixel slices; encoder told "compressed"; FEC active, the sink decodes FEC
with 0 errors.

Next for 144 Hz: a reference from a working driver. The rig has no Linux any more (Windows and macOS only), and reading DCN
registers under Windows needs a kernel tool (RW-Everything's driver is on Microsoft's blocklist). The plan: one boot of a Linux
live USB (kernel 6.13 or later) with the Samsung at 3840x2160@144, `sudo bash tools/linux/linux-capture.sh --snap-only`, and the
dump diffed against pipe2_linux_p0_dp1_dsc144.inc.

Done 2026-11-26, on a Fedora install on the rig (kernel 7.3.4): Linux shows a clean picture at 3840x2160@144, the same
raster and link as ours (1332.75 MHz, four HBR3 lanes, 960x108 slices, 16 bpp, one pipe, no ODM). The full capture script
froze the machine twice; tools/linux/dsc-snap.py read the table's 248 registers (test/ref/dsc_linux_4k144_snapshot.txt) and
tools/linux/dsc-diff.py compared them: 193 the same, 52 differ. Not causes: DSC_VERSION_MAJOR (Linux never writes it, the
hardware reads 1), audio and DRR (on under Linux), the tiled 10-bit desktop surface. Candidates:
- the plane's memory side: Linux has 18 DET segments to our 14 and other TTU/prefetch/VSTARTUP values; at 4K144 the plane
  reads about 4.8 GB/s, from a linear surface on macOS. An underflow corrupts whole compressed slices (noise). The DSC test
  now logs the planes' and OPTC0's underflow flags.
- 10 bpc under Linux (FMT, DP_PIXEL_FORMAT, the encoder and its RC parameters), 8 in our table; 8 works at 4K60.
- DSCL line buffer partitions (Linux 5, ours 0) and the DPP clock DTO (Linux 247/255).

Card, the same day (rdna4-dscboot=1, kext b7d58c36): at 4K144 the plane underflows for good (HUBP0_DCHUBP_CNTL underflow
3, again 0.5 s after clearing; ODM0_OPTC_INPUT_GLOBAL_CONTROL 0x2400: underflow occurred and current), while FEC runs with
0 errors and the sink reports RC buffer under-run and chunk errors (DSC_STATUS 0x07). The noise is the plane not keeping
up: the next step is the memory side, Linux's DET segments and watermarks and the clocks it runs at 4K144 (DCFCLK, FCLK,
UCLK, DISPCLK).

Later the same day:
- DML asks for DISPCLK 1384.6 MHz and DPPCLK 1341 MHz for this stream (one pipe, one pixel a DPP clock). The kext held
  650 MHz. With `rdna4-dispclk=1400` the plane stops underflowing.
- The DIG FIFO then ran one pixel a cycle with its error bit set: pipe2.cpp forced one for every non-4:2:0 table. Fixed
  in d70d692, so it holds Linux's 0x0000011d. The picture is mostly right from then on.
- The rest: a noisy strip (the right quarter at 8 bpc, the top at 10 bpc, `rdna4-dscboot=4`). The sink reports RC
  buffer under-run and overflow, and OPTC0's underflow flag comes back after clearing.
- A live register dump against Linux's snapshot: every DSC, OPTC-DSC and DIG register the table writes is the same.
  The rest differs because of Linux's surface (DET, watermarks, tiling), its DRR, audio and VSC packets, other pipes'
  clocks, and VSTARTUP-derived OTG values. OPTC_SEGMENT_WIDTH (Linux 1920) made no difference.
- Clocks under Fedora at 4K144 (pp_dpm_*): UCLK held at its top level (1258 MHz), FCLK 2016, SOCCLK 1280,
  DCEFCLK 514. Ours left UCLK free (97 MHz): the firmware's memory clock switches do not fit this stream's blank.
  With `rdna4-uclk=1258` the strip and OPTC0's underflow are gone. The kext now holds UCLK at its top level (1187 MHz
  as our firmware reports it) whenever the boot display runs above 600 MHz, set before the DSC stream lights: clean
  from the first frame, 144.011 Hz (card, 2026-11-26). The sink still reports DSC_STATUS 0x03 after clearing; the
  picture is clean.
- Still to do: the DISPCLK target following DML for the mode (`rdna4-dispclk=1400` by hand), and mode switches with
  DSC on (refused while the test stream runs).

Mode switches (card, 2026-11-26): with `rdna4-dispclk=1400` and a 3840x2160 console the boot display offers
3840x2160@144 beside its other modes. Picking it lights pipe 0 again (RDNA4Device::relightBootPipe):
- Linux's sleep part of the table it runs;
- the OTG stopped and MPCC0 out of OPP0's tree, as the cold start's dcn401_init_hw leaves them (the lighting
  requires both);
- the memory clock held;
- the 10 bpc DSC table lit with its link trained at HBR3 (143.988 Hz).

Leaving it lights the 3840x2160 table the same way, then retimes as usual (2560x1440@144 after it: 143.965 Hz). If the
DSC table does not light, the 3840x2160 one is lit again.

Card tests: `rdna4-dscboot=1` (8 bpc) or `=4` (10 bpc) with `rdna4-dispclk=1400` boots into the DSC stream.

## Notes

- Regenerating today's tables from lib/linux (6addb4f38557) reproduces them exactly, except the six YCbCr 4:2:0 ones (one
  DIG_FIFO_CTRL0 entry in the committed tables, from 4f59ba9): check that before replacing them.
- HDMI 2.1 FRL (the other way to 4K 120) is a separate feature, not part of this.
