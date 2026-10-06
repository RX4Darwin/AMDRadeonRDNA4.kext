# Card tests still to run

Written 2026-10-06. Everything here is built and passes `make test`, and none of it has run on the card. The first
three tests need a second DisplayPort cable and a second monitor with a DisplayPort input; the last needs nothing
extra.

Rig as before: RX 9070 XT, Big Sur 11.6.6, Samsung Odyssey G70D (4K) and Lenovo G25-10 (1080p, 144 Hz).

For every boot:

- Run `sudo bash tools/diagnostic-log.sh` before rebooting, and check the `active:` line near the top of the log:
  it must show the boot-args the test asks for.
- If a display stays dark, take the log from the other display or over SSH, then reboot. A reboot restores the
  firmware's state; nothing here is persistent.
- Report per test: the numbered answers, and the log.

What is already known and matters here (`docs/second-pipe.md`, section "DisplayPort"):

- The kext's link training works on this card and this Samsung: 4 lanes at HBR2 locked on the second attempt, five
  runs out of five once the first transfer was waited out (2026-10-06, `rdna4-dptrain=2`).
- The firmware retrains a DisplayPort link **it** lit when the monitor's hot-plug pin drops and returns. Whether it
  also touches a link the **kext** trained is not known. Test 1 is where that shows.
- With two DisplayPort monitors the firmware decides which one is the boot display. The kext lights the other one
  at 1920x1080@60, whichever it is.

## Test 0: read-only look (optional, no risk)

Cabling: Samsung on one DisplayPort, Lenovo on the other, nothing on HDMI.

```
rdna4-head2=2 rdna4-modeset=1 rdna4-dmubhist=1 rdna4-trace=1
```

Level 2 writes no display register, so the second monitor stays dark. macOS still gets a second display that
nothing shows, and windows or the pointer can wander onto it: boot, take the log, reboot. The log shows what level
4 would do.

1. Which monitor shows the boot screen and the desktop?

Lines to find:

- `pipe: lit pipe OTG0 DIGn -> linkn (HPDn) ... signal DP`: the boot display's connector.
- `pipe2: DisplayPort sink on AUXn: DPCD x.y, up to N lane(s) at rate 0x..`: the second monitor answers.
- `pipe2: N lane(s) at rate 0x.. for 148500 kHz`: the link the kext would train (4 lanes at rate 0x06 on a
  four-lane monitor).
- `pipe2: pipe 1 DIGn link n HPDn, 1920x1080 at 148500 kHz, surface at ...: N steps`: the plan was built.

If instead there is `pipe2: not lit: ...`, the text after it is the reason; send the log and stop here.

## Test 1: a second display on DisplayPort

Same cabling.

```
rdna4-head2=4 rdna4-modeset=1 rdna4-hotplug=2 rdna4-dmubhist=1 rdna4-trace=1
```

1. Which monitor is the boot display?
2. Does the other monitor show the second desktop, sharp and stable?
3. Leave it for two minutes. Does the second monitor keep its picture, without blinking or going dark? (This is
   where the firmware would show if it meddles with a link the kext trained.)
4. Let both displays sleep for a few minutes, then wake them. Do both come back?
5. Pull the second monitor's cable, wait 5 s, plug it back into the same connector. Does macOS drop the display
   and bring it back?
6. Switch the second monitor off at its power button, wait 5 s, switch it on. Does it come back?

Lines to find, in order:

- `pipe2: DisplayPort sink on AUXn: ...` and `pipe2: N lane(s) at rate 0x.. for 148500 kHz`
- `pipe2: lighting pipe 1 DIGn link n HPDn, 1920x1080 at 148500 kHz, stream and plane: N steps`
- `pipe2: link n, N lane(s) at rate 0x..: trained after N attempt(s): swing s, pre-emphasis p, lanes 77 77, aligned 1`
- `pipe2: plan ran; OTG1 measured 59.99x Hz`
- for step 4: `power: pipe2 display off: N steps ran` and `power: pipe2 display on: N steps ran`, with another
  `pipe2: link n ...: trained after ...` before the second
- for step 5: `hotplug: display gone from HPDn: link off`, then `hotplug: the same display is back on HPDn`

What failure looks like:

- `pipe2: step N (link training): the link did not train` and `pipe2: plan STOPPED`: the result name in the
  `pipe2: link n ...:` line just above says how far training got (no clock recovery, no channel equalisation,
  hardware or AUX failure).
- `aux: AUXn DPCD read/write 0x... failed: <reason>`: a transfer to the monitor failed, with why.
- `pipe2: not lit: the firmware's pipe ... uses a block of the plan's`: the firmware lit both monitors itself, or
  put the boot display where the plan wants to be. Not handled; send the log.
- `hotplug: the display has no 1920x1080 mode at 148500 kHz`: the second monitor's EDID lacks the CEA 1080p60
  timing the plan lights in.

Known gaps, so they are not surprises: if training fails four times there is no retry at another rate or lane
count, and a monitor that loses its link while lit and only signals that with a short pulse on the hot-plug pin
is not noticed.

## Test 2: mode switching on the DisplayPort second display

Only if test 1 lit the second monitor. Same boot, or the same boot-args again. Do this with the Lenovo as the
second display: its modes are the ones Linux was asked about (`tools/pipegen/modes.txt`). With the Samsung as the
second display only modes within 25 % of 148.5 MHz are offered besides those.

1. Switch the second monitor to 1600x900, then to 1280x720, then back to 1920x1080. Does the picture follow each
   time?
2. Switch it to 120 Hz, then to 144 Hz. It stays dark a moment longer than a normal switch: the link is taken
   down and trained larger first. Does it come back at the new rate?
3. At 144 Hz, let it sleep, then wake it. Does it come back at 144 Hz?
4. Switch back to 60 Hz. Does the picture follow?

Lines to find:

- for each switch: `pipe2: switching to id N WxH@R (K kHz), request timing from Linux for this mode`, then
  `pipe2: now WxH at K kHz, OTG1 measured R Hz; ... DET n`
- for step 2, between those two: `pipe2: N lane(s) at rate 0x.. for 285500 kHz` (or `333070`) and
  `pipe2: link n, ...: trained after N attempt(s)` at the larger link (4 lanes at rate 0x0a for 120 and 144 Hz
  on a four-lane monitor). The link going down and up for this does not log `power:` lines.
- for step 3: `pipe2: N lane(s) at rate 0x.. for 333070 kHz` before the wake's training line

What failure looks like:

- `pipe2: the larger link did not come up: back to N lane(s) at rate 0x..`: the switch is refused and the old
  link is trained again; the picture should return at the old mode.
- `pipe2: switch to id N refused: ...` or `failed: ...`, and `pipe2: the previous mode is NOT restored`.
- A picture at the wrong size or with wrong colours after a switch: say which mode, and what it looks like.

## Test 3: a DisplayPort display plugged in after boot

Only if test 1 passed. Boot with the second monitor's DisplayPort cable unplugged, boot-args as in test 1.

1. After the desktop is up, plug the second monitor in. Does it show the second desktop within about 10 s?
2. Unplug it again, wait 5 s, and plug it into the card's **other** free connector if there is one (HDMI with
   an HDMI cable counts). Does it stay dark? (Expected: once lit, the pipe stays with the connector it was lit on.)

Lines to find: `head2: ... offline until a display is plugged in`, `hotplug: HPDn went high`,
`pipe2: DisplayPort sink on AUXn: ...`, `pipe2: lighting ...`, `hotplug: display connected on HPDn: N mode(s)`.

## Test 4: the stretched console (no extra hardware)

This checks the fix for the zoomed picture of 2026-10-06, which could not be tried then because the console went
back to 4K first. Samsung on DisplayPort; the Lenovo may stay on HDMI. Run it after test 5b: if the scaler in
bypass cuts the top of the picture off on a normal boot, it may do the same here, and then the answer to step 2
is "the whole width, but the top is cut off".

1. In OpenCore's `config.plist` set `UEFI -> Output -> Resolution` to `1920x1080`. Boot with
   `rdna4-head2=4 rdna4-modeset=1 rdna4-hotplug=2 rdna4-trace=1`.
2. The Apple logo is larger than usual (expected). At the desktop, does the Samsung show the **whole** desktop,
   not one corner scaled up?
3. Switch the Samsung to 1600x900 and back to 1920x1080 in Displays. Whole desktop each time?
4. Set `Resolution` back to `Max` and reboot. Is 3840x2160 back?

Lines to find: `console framebuffer: ... 1920x1080`, `modes: the console is 1920x1080 and the firmware stretches
it onto its 3840x2160 timing`, then `modes: switching to id N ...` and `modes: now ...`.

In this state the Samsung has no 3840x2160 mode: the kext offers nothing larger than the console. That is a
known limit, not part of this test.

## Test 5: pointer and input lag on the 4K display (no extra hardware)

Observed 2026-10-06: in applications like Google Chrome the pointer and typing lag on the Samsung (DisplayPort)
and behave almost normally on the Lenovo (HDMI). At that time the Samsung ran 1920x1080 HiDPI, which is a
3840x2160 surface, and the Lenovo a plain 1920x1080 one. There is no graphics acceleration yet, so macOS draws
every pixel of both on the CPU, and the pointer is drawn the same way (a software cursor).

Two things differed between the screens then, and either could explain it without DisplayPort being at fault:
the Samsung had four times as many pixels, and the Lenovo ran at 144 Hz against the Samsung's 60 Hz.

Tried the same day with `rdna4-cursor=1 rdna4-vbl=1` (a hardware cursor on the boot display): the kext ran it on
the 4K DisplayPort pipe next to `rdna4-head2=4` without an error in the log, through a switch to 2560x1440 and
back, and the Samsung was still reported less responsive. So the pointer being drawn by macOS is not the whole
cause. The log has nothing that measures responsiveness.

Result, the same day: with the Samsung at 2560x1440 (the low-resolution mode Displays offers there) Chrome was
equally responsive on both screens. So it is the number of pixels macOS has to draw on the CPU, not DisplayPort
and not that pipe. 1920x1080 HiDPI on a 4K panel will stay sluggish in heavy applications until there is
acceleration; 2560x1440 is the workable setting until then.

### Test 5b: the top of the picture in the Samsung's smaller modes

At 2560x1440 the menu bar is cut off at the top of the screen: only its bottom edge shows. At 1920x1080 (the real
mode, not HiDPI) it is cut off too, a little less.

Ruled out on the card 2026-10-06 (kext `42047888-...`): the scaler. With the scaler left exactly as the firmware
has it, the top was still cut off, and the log line after the switch shows the rectangles right
(`modes: scaler mode 0x00000001 autocal 0x00000000; viewport 0x05a00a00 at 0x00000000, recout 0x05a00a00 at
0x00000000, mpc 0x05a00a00`).

What fits, not yet confirmed: on the boot display a mode switch keeps the firmware's request timing for the plane,
which was worked out for 3840x2160 with its 62 lines of vertical blanking. 2560x1440 has 41 and 1920x1080 has 45,
so the plane gets 21 and 17 lines less lead than that timing expects, and a menu bar is 24 lines high: its bottom
3 lines would be left at 2560x1440, about 7 at 1920x1080. That is what was seen.

The check, no reboot, usual boot-args. Displays preferences offers only a few of the Samsung's modes (it lists
3840x2160, 2560x1440 low resolution, 1920x1080, 1280x720 and 1080p), so the modes are set with
`tools/cgmode.py`, which reaches all of them. The change lasts until logout; `/usr/bin/python tools/cgmode.py`
alone lists the displays and modes.

1. `/usr/bin/python tools/cgmode.py 1600x900` (100 lines of blanking, more than the firmware's mode). Is the
   menu bar whole?
2. `/usr/bin/python tools/cgmode.py 1280x720` (30 lines). Is the menu bar gone completely?
3. `/usr/bin/python tools/cgmode.py 3840x2160` to go back.

Yes to both confirms it. The fix is then what the second display already has: the plane's request timing from
Linux for each mode (`tools/pipegen`), generated for the boot display's pipe and its 3840x2160 surface.

## After the tests

For each test that passes, the README's status list and `docs/second-pipe.md` get the date and what ran, as for
the earlier ones. Daily boot-args stay `rdna4-head2=4 rdna4-modeset=1 rdna4-hotplug=2`.
