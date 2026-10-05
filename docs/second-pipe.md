# Lighting a second pipe

Status 2026-10-04: **works on the card** (section 7): with `rdna4-head2=4` the second monitor shows the second
desktop next to an undisturbed boot display. The macOS half of a second display works
(`docs/second-head-ndrv.md`): macOS draws into a spare VRAM surface that no pipe scans out. This is the hardware half:
light a pipe the firmware left dark and point it at that surface.

Read `[F]` as taken from the Linux source named, `[I]` as inference, `[U]` as unknown until a card boot.

## 1. What it does

`rdna4-head2=4` lights pipe 1 (OTG1, OPP1, HUBP1, DPP1, MPCC1) through stream encoder DIG2 and link 2 (UNIPHY C,
HPD3) at CEA 1920x1080@60, 8 bpc RGB over HDMI, scanning the phantom head's surface. That is the test rig's Lenovo on
the HDMI connector next to the Samsung on DisplayPort. The lit pipe (pipe 0, DIG0, link 0) is not written to.

The levels are a ladder, so that each boot answers one question:

| `rdna4-head2` | What happens | What it shows |
|---|---|---|
| 1 | Phantom head only, as before. | macOS takes a second display. |
| 2 | The plan is built and every step is published with the register's value now and after (`RDNA4FB,Pipe2` in the registry, and in `tools/diagnostic-log.sh`). **Nothing is written.** | What the firmware left in the dormant pipe; whether the requirements hold. |
| 3 | The stream only: clock, timing generator, encoder, PHY. No plane. Afterwards the survey of what the plane would still change is published. | The monitor wakes and shows a solid teal-blue. The DMUB commands work for a pipe the firmware never lit. |
| 4 | Stream and plane. | The monitor shows the second desktop. |

At 2 and above the phantom head only serves the one mode the plan is for; a sink without it stays unserved. With no
second monitor answering on DDC there is no second head at all (an invisible display is level 1, asked for by name).

## 2. Where the sequence comes from

Not from reading Linux and typing it in. `tools/pipegen` compiles amdgpu's display core as a host program and runs
it [F]:

- `pipegen.c` includes `dc/resource/dcn401/dcn401_resource.c` for Linux's own register tables and block constructors,
  and builds one fake pipe: an HDMI stream, a visible 32-bit linear plane, a link.
- It calls `dce110_apply_single_controller_ctx_to_hw` (which runs `dcn401_enable_stream_timing`, then
  `link_set_dpms_on` with `enable_link_hdmi` and `dcn401_enable_stream`), then `dcn401_program_pipe` between
  `pipe_control_lock` calls. The per-pipe part of `dcn401_init_hw` comes first (`mpc_init_single_inst`, `tg_init`).
- The HUBP request, deadline and TTU values, VSTARTUP and the DET size are Linux's too: `run_dml()` runs DML 2.1
  (`dml2_build_mode_programming`) for the 4K DisplayPort stream plus this one, with the static DCN 4.01 bounding box.
- `rec.c` replaces Linux's register helpers and records every write, update, wait and delay, with the name of the
  Linux function that made it.
- `mkinc.py` names each register from `dcn_4_1_0_offset.h` and writes `src/pipe2_linux.inc`.

`src/pipe2.cpp` turns that table into a plan and adds what only exists at run time: the three DMUB commands (set
pixel clock for OTG1 on PLL 2, stream setup for DIG2 twice as Linux sends it, transmitter enable for link 2), the
surface address, and the lit pipe's VM aperture.

Regenerate with `tools/pipegen/run.sh <linux tree>`; the script says which sparse checkout is enough. The checked-in
table is from Linux `6addb4f38557` and has 402 entries.

## 3. What keeps it off the lit pipe

- **Two runs.** Some Linux functions read a register and write it back (`dccg401_wait_for_dentist_change_done`
  rewrites `DENTIST_DISPCLK_CNTL`). A recording of one run would turn that into "write 0". The generator runs
  everything twice, with unknown registers reading all-zeros and then all-ones; a bit that differs between the runs
  was read back, and the step becomes an update that leaves it alone. A different *sequence* between the runs means a
  read steered the code; each of those is settled by hand in `pipegen.c` with the reason (five of them, section 4).
- **Names.** `mkinc.py` refuses a table in which any register is not one of pipe 1's, DIG2's or link 2's by its Linux
  name, or one of six shared registers, and then only the listed bits. Where Linux writes a shared register whole
  (`MPC_OUT_CSC_COEF_FORMAT`), the step is narrowed to this pipe's bit.
- **Layout.** `testPipe2` in `tools/atomdump.cpp` checks the built plan again, by block address and stride instead of
  by name. Planting a write to OPP0's output mux or a whole write of `DENTIST_DISPCLK_CNTL` fails it.
- **Requirements.** Before the first write the plan checks that OTG1 is not running, DIG2's front-end and back-end are
  off, MPCC1 feeds no OPP, and the IP request window is open. If the firmware lit this pipe after all, nothing is
  written.
- `RDNA4Device::lightSecondPipe` refuses if the lit pipe uses any of the plan's blocks, if the VBIOS has no connector
  on HPD3 and link 2, or (for the plane) if the DET buffer has no 4 free segments.

The shared registers the plan touches: `OTG_PIXEL_RATE_DIV` (OTG1's TMDS divider bit), `DPPCLK_CTRL` (DPPCLK1 enable),
`MPC_OUT_CSC_COEF_FORMAT` (OPP1's bit), and
`DENTIST_DISPCLK_CNTL` and `DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A`, both written back as read.

## 4. Where it departs from Linux, and what is assumed

Settled reads (the state the plan assumes; all visible in the level-2 survey):

1. `OTG_PIXEL_RATE_DIV`: Linux skips the divider write if it already holds /4. The plan always writes it.
2. `DC_IP_REQUEST_CNTL`: open, as the firmware leaves it on the card (section 7). Linux then does not touch it. It is
   a requirement: the HUBP power-up write is ignored through a closed window.
3. DIG2's symbol clock is running when the FIFO is reset (the step before enabled it).
4. The info-packet memory is asleep: the plan always wakes it.
5. MPCC1's LUTs are off, and MPCC1 feeds no OPP (this one is also a requirement).

Departures:

- **DPP clock.** Linux divides the DPP reference clock down to what the pipe needs. The frequency the firmware left
  that reference at is not known, so DPP1 takes it undivided (`DPPCLK1_DTO_PARAM` phase = modulo). Never too slow;
  costs a little power.
- **Bandwidth.** Linux's `prepare_bandwidth` raises the display clocks and the shared watermarks for the new total
  before lighting anything. The plan does neither: the clocks are the SMU's, and the watermarks belong to the lit pipe
  too. DML asks for DISPCLK 545 MHz and DCFCLK 228 MHz for both displays together [F]; whether the firmware's
  single-display setting leaves that much is the main unknown for level 4 [U]. Too little shows as underflow.
- **The other pipes.** `dcn401_program_front_end_for_ctx` also takes the update lock of every lit pipe and can move
  DET segments between them. The plan locks only OTG1 and takes only free DET segments.
- **Link encoder `hw_init`** is left out: it programs the AUX channel, which a TMDS link does not use and whose
  instance is board wiring.
- **VM aperture.** amdgpu does not program a system aperture on a discrete card. The plan copies the three registers
  from the lit HUBP, so that the surface address means the same on both.
- **SCDC.** Where Linux calls `write_scdc_data`, the plan writes the sink's `TMDS_CONFIG` (0 at this clock) if the
  sink's EDID announces SCDC. Linux also reads the scrambler status back afterwards, only to log it; the plan does not.
- **No audio, no HDCP, no hot-plug.** The AVI infoframe is Linux's (VIC 16, RGB, full range).
- DML ran with the static bounding box, not the SMU's clock table, and with GPU VM off (the surface is in the frame
  buffer aperture).

## 5. Reading a card log

Boot `rdna4-head2=2` first. In `tools/diagnostic-log.sh` output, under "second head and second pipe":

- `pipe2: DET segments a b c d of 21: 4 for the plane fit` — the DET budget.
- `pipe2: N require ...` lines — each requirement with the register's value; `(not so now)` means level 3 would refuse.
- `pipe2: survey of 398 steps ...: N would change a register` and the registry copy, one line per step:
  step, `segment:dword`, value now, `->` or `==`, value after, Linux function and register.

At level 3 or 4:

- `pipe2: lighting pipe 1 ...` then, if a step fails, `pipe2: step N (...)`. A DMUB command the firmware does not
  take stops the plan there.
- `pipe2: plan ran; OTG1 measured 60.000 Hz (the mode is 60.000); HUBP1_DCHUBP_CNTL=... ODM1_OPTC_INPUT_GLOBAL_CONTROL=...`
  — the timing generator's real frame rate (0.000 means OTG1 is not counting: no pixel clock), and the raw registers
  that hold the plane's and the OPTC's underflow flags.

- `pipe2: colour: 2:0ed2 <value>, lit pipe 2:0d67 <value>  <Linux function and register>` (level 4) — read-only:
  each colour-path register (DPP, blender, output CSC, formatter) where the pipe Linux's code programmed and the
  pipe the firmware programmed differ, then a count. Sizes, positions and instance numbers differ by nature.

Escape: remove the boot-arg. The plan has no undo; a reboot restores the firmware's state.

## 6. Limits

- One configuration to light: pipe 1, DIG2, link 2, HPD3, 1920x1080@60 at 148.5 MHz. Another board, or another mode
  to start in, needs `tools/pipegen/run.sh` run with other arguments. Once lit the pipe can change mode (below).
- The second display must be HDMI or DVI (TMDS). A second DisplayPort display needs link training. A mode above
  340 MHz needs the table regenerated for it (the generator then turns the scrambler on by itself).
- The boot display must not be on the plan's blocks: with the Lenovo alone, the firmware puts it on DIG2 and link 2.
- System sleep is not handled (it stays vetoed for the whole driver).

### Mode switching

Verified on the card 2026-10-05 (section 7). With `rdna4-modeset=1` the second head offers the sink's
other modes, and a switch runs the boot display's mode-set engine (`src/modeset.cpp`) pointed at pipe 1, DIG2 and
link 2: blank, stream and timing generator off, `SET_PIXEL_CLOCK`, new timing, stream on, viewport and output
rectangle, unblank. Then the AVI infoframe is sent again for the new timing (VIC, picture aspect, bar ends and
checksum; `testPipe2` holds the words against what Linux sends for five modes). Display sleep and wake follow the
running mode's pixel clock and infoframe.

What else changes with the mode comes from Linux too (verified on the card 2026-10-05, section 7). `tools/pipegen/run.sh` runs the generator once for each timing in `tools/pipegen/modes.txt`,
with the plane on the lit mode's surface, and `mkinc.py` keeps the registers Linux leaves different from mode to
mode: 34 of them for the 15 modes of the test rig's Lenovo. They are the HUBP's request timing from DML (DLG, TTU,
`REF_FREQ_TO_PIX_FREQ`), the DET size (2 to 7 segments), `VSTARTUP`, `VUPDATE`, `VREADY` and the timing itself. The
engine adds them inside its update lock (`Pipe2::modeSteps`, `ModeSet::Target::extra`). `testPipe2` switches to each
mode, from the lit state and from the mode before it, and requires every one of the 34 registers to end as Linux
has it; that also holds the engine's own timing arithmetic against Linux for 15 modes.

That is what lets the pipe run its 120 and 144 Hz modes (285 and 333 MHz), which the lit mode's request timing does
not carry. Limits:

- The surface stays, so no mode is larger than the plan's 1920x1080.
- The link stays unscrambled: nothing above 340 MHz.
- A timing that is not in `modes.txt` keeps the lit mode's request timing, and is only offered within 25 % of the
  lit mode's pixel clock (the margin the boot display switches with on the firmware's values).
- A mode is only offered if the DET buffer has its segments next to what the other pipes hold.
- The shared display clocks and watermarks are still the firmware's (section 4): 4K60 next to 1080p144 is more
  than they were set up for, and an underflow there would show in the flags the log line prints.

Log lines: `head2: mode id N ...` (the table), `pipe2: switching to id N ..., request timing from Linux for this
mode`, then `pipe2: now 1920x1080 at 333070 kHz, OTG1 measured 143.999 Hz; HUBP1_DCHUBP_CNTL=...
ODM1_OPTC_INPUT_GLOBAL_CONTROL=... DET 7`.

### Display sleep

A lit second pipe answers macOS's sync requests (`cscGetSync` / `cscSetSync`), so the head gets display sleep like
the boot display. Sleep and wake are Linux's too: the generator calls `link_set_dpms_off` and then `link_set_dpms_on`
on the stream it just lit, and the table carries them as two more parts (38 and 127 steps). Sleep sets AVMUTE, waits
out a few frames, stops the infoframes, turns the stream encoder off, disconnects it from the link encoder and sends
`SET_PIXEL_CLOCK` again, which is how amdgpu takes a TMDS transmitter down while the PLL keeps clocking the timing
generator. The timing generator, the plane and the surface stay as they are. Wake is the link half of lighting:
encoder setup, infoframes, the transmitter enable. `testPipe2` checks that after sleep and wake every register that
lighting set holds the same value again.

Sleep clears the stream encoder's half of `SYMCLKC_CLOCK_ENABLE` (`dccg401_disable_symclk_se`), and for HDMI Linux
never sets it again: the firmware does, on the encoder command (section 7). The log line shows it:
`power: pipe2 display off: 39 steps ran; DIG2_DIG_FE_EN_CNTL=... SYMCLKC_CLOCK_ENABLE=...`, and the same for `on`
(also in the `RDNA4FB,DisplayPower` registry history). `rdna4-nosleep=1` turns it off.

## 7. Card boots (Big Sur 11.6.6, Samsung 4K on DisplayPort, Lenovo on HDMI)

**Survey, 2026-10-04 07:10 (`rdna4-head2=2`).** The requirements held: `OTG1_OTG_CONTROL` 0x200, DIG2's front-end and
back-end off, `MPCC1_MPCC_OPP_ID` 0xf. `DET segments 3 3 3 3 of 21`. The dormant blocks read as reset state.
`OTG_PIXEL_RATE_DIV` 0x8421: every TMDS divider is already /4. The Samsung was on link 1 (DIG1, HPD2) this time, which
the plan does not use either. That build surveyed only the stream half (a bug, fixed since).

**Stream, 2026-10-04 20:37 (`rdna4-head2=3`).** `pipe2: lighting pipe 1 DIG2 link 2 HPD3 ... stream only: 206 steps,
4 DMUB commands`, then `scdc: DDC2: sink version 1, TMDS_CONFIG = 0 written` and `pipe2: plan ran; OTG1 measured
60.001 Hz`. **The Lenovo showed the test colour and the Samsung was undisturbed.** So the firmware the GOP leaves
running takes the pixel-clock, encoder and transmitter commands for a PHY and timing generator it never lit, and
writing the monitor's SCDC register over DDC works.

The plane survey from the same boot (what level 4 would change, with the stream lit) found one thing to change in
the plan: `DC_IP_REQUEST_CNTL` reads 1, so the firmware leaves the IP request window open, and the plan as generated
would have closed it. It is regenerated for an open window and now requires one (section 4). Otherwise: HUBP1's power
domain is already on (`DOMAIN1_PG_CONFIG` 0, status on); the VM aperture registers are 0 on the lit HUBP too, so the
copies change nothing; `CM1_CM_CONTROL` has the bypass bit set, which the plan clears; `HUBP1_DCHUBP_CNTL` reads 0x000f001a before the plane is enabled, the value to compare the
level-4 line against.

**Plane, 2026-10-04 21:37 (`rdna4-head2=4`): black.** The plan ran (`stream and plane: 399 steps`, OTG1 at 60.002 Hz,
`HUBP1_DCHUBP_CNTL` 0x000f0012: the HUBP left its blanked state, no underflow flag), macOS listed the monitor, and the
monitor showed black. The table itself was wrong: it wrote `DSCL1_RECOUT_SIZE` and `DSCL1_MPC_SIZE` as 0, a plane
with a 0x0 output rectangle. The generator had compiled Linux without `CONFIG_DRM_AMD_DC_FP`, which every DCN build
of amdgpu has; without it `resource_build_scaling_params` skips the scaler library (SPL) that DCN 4.01's DPP takes
its rectangle from. The level-3 survey already showed both registers staying 0. With the option the table differs in
exactly those two entries (1920x1080), and `testPipe2` checks them.

**Plane again, 2026-10-04 21:50 (`rdna4-head2=4`, regenerated table): both displays work.** `stream and plane: 399
steps, 4 DMUB commands`, `OTG1 measured 60.000 Hz`, `HUBP1_DCHUBP_CNTL` 0x000f0012, no underflow flag, no step timed
out. The Lenovo shows the second desktop and the Samsung is undisturbed. So the unknowns of section 4 came out well on
this card: the firmware's display clocks and watermarks for one 4K display also carry a 1080p plane next to it, and
3 + 4 DET segments are enough. The pointer shows on the second display (macOS draws it into the surface: the head
has no hardware cursor).

**Display sleep, 2026-10-05 14:51 (`rdna4-head2=4`): both displays sleep and wake.** Head 2 now answers `Status csc
11`, and macOS sends it `Control csc 11` 140 ms after the boot display's. `power: pipe2 display off: 39 steps ran;
DIG2_DIG_FE_EN_CNTL=0x00000000 SYMCLKC_CLOCK_ENABLE=0x00000001` (55 ms, most of it the frames AVMUTE waits for), and
three seconds later `power: pipe2 display on: 128 steps ran; DIG2_DIG_FE_EN_CNTL=0x00000001
SYMCLKC_CLOCK_ENABLE=0x00000211` (under 2 ms). No step timed out. So on this firmware a repeated `SET_PIXEL_CLOCK`
does take the HDMI output down far enough for the monitor to sleep, and the encoder command on wake sets the stream
encoder's symbol-clock gate again (0x1 to 0x211: enable and source link 2), which is why Linux can leave it alone.

**Colour comparison, 2026-10-05 15:25 (`rdna4-head2=4`), because the Samsung looks dimmer than the Lenovo.** 90
colour-path registers compared, 24 differ, and none of them dims the firmware's pipe. Sizes and instance numbers
aside: the firmware bypasses the DPP colour block (`CM0_CM_CONTROL` 1) and the output CSC (`MPC_OUT0_CSC_MODE` 0)
where Linux enables both with nothing in them (identity matrix, no gamma); the blender passes the top layer through
(`MPCC_CONTROL` mode 1) where Linux blends it alone at full alpha and gain (mode 2); the firmware leaves 8-bit
pixels zero-extended (`FORMAT_EXPANSION_MODE` 0, `FMT_DYNAMIC_EXP_CNTL` 0: white is 0.3 % under full scale) where
Linux replicates the top bits; and the formatters differ as 10-bit DisplayPort and 8-bit HDMI do. The gains, the HDR
multiplier, degamma and output gamma are the same on both. So the difference in brightness is the monitors' own.

**Mode switching, 2026-10-05 15:38 (`rdna4-head2=4 rdna4-modeset=1`): works.** The second head offered 13 modes and
macOS listed the same 13. `pipe2: switching to id 4 1600x900@60.000 (108000 kHz)`, `now 1600x900 at 108000 kHz, OTG1
measured 60.001 Hz`; then 1280x720 (59.999 Hz); display sleep (39 steps) and wake (128 steps) at 1280x720; back to
1920x1080 (60.001 Hz). No step timed out and no underflow flag was set. The Lenovo showed a correct, stable desktop
in each mode and after the wake, and the Samsung was undisturbed. So the HUBP request timing DML made for 1920x1080@60
also carries the slower modes, as the firmware's does on the boot display. `HUBP1_DCHUBP_CNTL` read 0x000f001a once,
after the 1600x900 switch (0x000f0012 after the others): bit 3 is `HUBP_IN_BLANK`, a live status, there most likely
caught in that mode's longer vertical blank.

Reading the log: kernel lines from other senders (ALF, Sandbox) can lack a newline, and an RDNA4FB line glued to one
carries that line's older timestamp.

**120 and 144 Hz, 2026-10-05 16:01 (`rdna4-head2=4 rdna4-modeset=1`): work.** The second head offered 15 modes.
Every switch logged `request timing from Linux for this mode`: 1920x1080 at 50 Hz (measured 50.002, DET 4), 60, 120
(119.971, DET 6), 144 (144.012, DET 7), then 1280x720 (DET 2), 1600x900 (DET 3) and back to 1920x1080@60 (DET 4).
No underflow flag in `HUBP1_DCHUBP_CNTL` or `ODM1_OPTC_INPUT_GLOBAL_CONTROL` after any of them. The Lenovo showed a
correct, stable picture at 120 and 144 Hz and the Samsung was undisturbed, so the firmware's shared clocks and
watermarks carry 4K60 next to 1080p144, and the DET buffer can be resized on the running pipe inside the blanked
switch. One optional wait timed out once, `step 47 (dpg latched) timed out, continuing`, on the switch from 144 Hz
to 1280x720; everything after it ran and the picture was normal.

Display sleep and wake at 144 Hz ran later in the same boot (16:07): `pipe2: now 1920x1080 at 333070 kHz, OTG1
measured 144.001 Hz`, then `power: pipe2 display off: 39 steps ran` and, 0.9 s later, `power: pipe2 display on: 128
steps ran; DIG2_DIG_FE_EN_CNTL=0x00000001 SYMCLKC_CLOCK_ENABLE=0x00000211`, with macOS still at 144 Hz afterwards.
The sleep and wake parts carried the running mode's pixel clock (333.07 MHz) and infoframe.

