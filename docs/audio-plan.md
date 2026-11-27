# Audio over HDMI and DisplayPort, plan

Status, 2026-11-27: macOS lists "Odyssey G70D" as a DisplayPort sound output (2 channels, 48 kHz) after
`sudo sysctl debug.rdna4_hda=1` and `=2`; whether sound comes out is not checked yet (needs someone at the monitor). The route found so far is Apple's own GPU-audio driver,
AppleGFXHDA, which still ships on Tahoe (AppleHDA itself is gone; the rig's onboard audio runs on SimpleHDA).

## What the card and macOS have

- Navi 48's audio is PCI function 1 beside the GPU, 1002:AB40, class 0x040300 (an HD Audio controller). macOS gives it no
  driver.
- AppleGFXHDA (com.apple.driver.AppleGFXHDA 550.1, in the system kernel collection) has an AMD personality,
  `AppleGFXHDAEGController`, for 1002:AAE0, AAF0, AAF8, ABF8, AB20, AB38 and AB28 only. Its `probe` reads the function's
  vendor and device IDs from config space (`IOPCIDevice::extendedConfigRead16`) and looks them up in a 12-entry table
  (Intel 9DC8, A348, 9D71, 34C8, 38C8 and the seven AMD ones); the AMD entries all carry the same settings. A function
  not in the table is refused without a message (`probe`, then `free`, in the kernel log).
- After probe, `AppleGFXHDAEGController::locateAssociatedGraphicsController` takes the GPU on the same bus and device,
  waits for `AppleDisplay` services (IOGraphics' display under each framebuffer) to appear, and then works with the
  IOFramebuffers: `registerFramebufferNotification`, and `getAttributeForConnection` / `setAttributeForConnection` from
  its output engine. So the framebuffer side (ours: the IONDRV translator) has to answer whatever audio attributes it
  asks for.
- The display hardware side, as Linux does it (dc/dce/dce_audio.c, dcn401 stream encoder): `dce_aud_hw_init`, the audio
  DTO (`wall_dto_setup`), `az_configure` (the sink's audio capabilities from the EDID into the Azalia endpoint, which is
  what the HD Audio codec's pin reports as ELD and presence), `az_enable`, then on DP `dp_audio_setup` and
  `dp_audio_enable` (audio SDPs: ASP, AIP, ATP in DP_SEC_CNTL). The pipegen captures in src/pipe2_linux*.inc hold none of
  it (only audio mute writes); a pipegen "audio" scenario could generate it as for the other sequences.

## Step 0: does AppleGFXHDA take the function?

`sudo sysctl debug.rdna4_hda=1` (src/plugin.cpp, nothing at boot): marks the function `rdna4-hda`, which the
`AMDRadeonRDNA4HDA` personality in Info.plist needs to match, routes `IOPCIDevice::extendedConfigRead16` so the function
reads 1002:AB28 (Navi 21's audio) as its device ID, and has it matched again. A reboot undoes all of it.

- 2026-11-26, first run: the personality matched and AppleGFXHDA loaded, `probe` refused the function (AB40 not in the
  table; a first route on `configRead16` missed, since probe reads through `extendedConfigRead16`).
- Second run, `extendedConfigRead16` routed: probe took it, `AppleGFXHDAController::start` ran, found the GPU ("GFX BDF")
  and one codec (status-mask 1): `AppleGFXHDADriver` for codec 1002:AA01 (AMD's HDMI/DP codec). It then stopped:
  `AppleGFXHDAFunctionGroupFactory::createAppleHDAFunctionGroup` picks the codec's function group by the controller's
  `device-id` registry property (AA01 codecs: AB28, AB20, AB38, AAE0..., ABF8 get `AppleGFXHDAFunctionGroupATI_Tahiti`),
  and that property still said AB40 (assertion at line 94). Assertions at AppleGFXHDAController 4775 and 5107 and
  AppleGFXHDADriver 4838 came before it; not looked at yet.
- Third run, `device-id` set to AB28 as well (the personality also lists 1002:AB28, since matching reads that property):
  `AppleGFXHDADriver::start` ran and published an IOAudioDevice (transport `dprt`, `HDMIDPAudioCapabilities` yes), with
  no audio engine and nothing in Sound settings: no pin reports a display yet. The three earlier assertions remain.

## Step 1: the display side for the boot display (written, card-run, no output yet)

`tools/gen/pipegen/run.sh lib/linux audio` runs pipegen's "audio" scenario (sequence_audio): on the lit DP stream of
pipe 0 / DIG0, Linux's dce_aud_hw_init, build_audio_output + the stream encoder's dp_audio_setup + dce_aud_az_configure
(the sink from the EDID: the Samsung's IDs, name, LPCM 2 channels 32-48 kHz, front left/right), dce_aud_wall_dto_setup
and dce110_enable_audio_stream (az_enable, ASP/AIP/ATP on). mkaudio.py writes it to src/audio_linux.inc (93 steps); the
recorder models the Azalia index/data pair (rec_indirect) so its read-modify-writes stay updates. The kext replays it
(`RDNA4Device::enableBootAudio`) when `debug.rdna4_hda=1` is written, before handing the function to AppleGFXHDA.

- 2026-11-26 on the rig: 93 of 93 steps written, DP0_DP_SEC_CNTL 0x00901111 (audio packets on beside HDR's), both
  displays unaffected, AppleGFXHDA started as before, still no audio engine or Sound output. The three assertions are
  in setRequireMaxBusStall, setCodecProperties and readVendorRevisionInfo (missing properties, not the blocker).
- Same day: AppleGFXHDA's AppleDisplay match handler (`gfxMatchedHandler`) reads `hda-gfx`, `connector-type` (it
  compares 0x400 DisplayPort / 0x800 HDMI), `built-in`, `port-number`, `AAPL,DisplayPipe` and the display's
  `IODisplayEDID` (`edidIsValidForAudio`). With `hda-gfx` = "onboard-1" on the function and on all six framebuffers and
  `connector-type` DisplayPort on the framebuffers (a named stash, "experiment: audio hda-gfx + connector-type"), nothing
  changed: no `IOFBEvent` on the controller, no engine. So the handler is either not reached or refuses for another reason.
- Next: why AppleGFXHDADriver makes no engine: its graphics association (AppleDisplay matching, the framebuffer's
  connector type, registerFramebufferNotification) and whether the codec's pin reports presence and ELD (HDA verbs
  through the controller, or AppleGFXHDA's own logging).

## Step 2: tying AppleGFXHDA to our framebuffers (2026-11-27, an output appears)

The kext logs a few AppleGFXHDA functions through Lilu routes when it loads (processGfxHda). What they showed:

1. `AppleGFXHDAEGController::locateAssociatedGraphicsController` asks for `AppleDisplay` services with
   gIOMatchedNotification and stores the notifier when the call returns; IOKit hands it the AppleDisplays that already
   exist inside that call, and the AMD class's own `gfxMatchedHandler` (it overrides the base one) drops them since
   their notifier is not the stored one yet. On a Mac it loads before any display; here it loads after ours. So
   `debug.rdna4_hda=2` registers the AppleDisplays under the GPU again: then both are taken (EDIDs valid for audio,
   `registerFramebufferNotification` on both framebuffers).
2. On each framebuffer event it reads, from the framebuffer: `connector-type` (0x400 DisplayPort / 0x800 HDMI),
   `av-signal-type` (IOGraphicsTypes.h's kIOFBAVSignalTypeDP 0x10, HDMI 0x08), `audio-codec-info` (byte 0 the codec's
   address, byte 1 non-zero, byte 2 the pin's node ID; also a boot-arg of that name) and, in findValidDisplayCodec,
   `AAPL,DisplayPipe` (non-zero, not 0xffff). The kext sets all of them, with `hda-gfx` "onboard-1" on both sides; the
   boot display gets pin 0x03 (Azalia endpoint 0, which step 1 programs), the others the next pins.
3. Then `createAudioOutputEngineDP` and `activateAudioEngines` run: AppleGFXHDAEngineOutputDP with its stream, DMA
   started, and Sound settings list "Odyssey G70D" (transport DisplayPort, 2 channels, 48 kHz). macOS made it the system
   (alert) output; the rig's was set back to its default output (~/rdna4/sysout follow).

## Step 3: from boot (2026-11-27)

`rdna4-audio=1`: at the boot framebuffer's attach the kext programs the boot display's audio (step 1) and hands the
function to AppleGFXHDA (step 0); the routes tie each display's framebuffer before the match handler reads it, and
register the AppleDisplays again once `locateAssociatedGraphicsController` has asked for them (by hand:
`debug.rdna4_hda=2`). After every switch of the boot pipe and when the display comes back on, the boot display's audio
is set up again (a mode switch can take the audio packets down). On the rig the boot with it came up normally and Sound
settings listed "Odyssey G70D" and "LEN G25-10"; the Lenovo's has no Azalia endpoint set up (only the boot display's
is), so it is most likely silent. Not heard yet.

## Step 4: every DisplayPort display, from its own EDID (2026-11-27)

pipegen's audio scenario runs for each stream encoder DIG0-3 with its own Azalia endpoint (amdgpu gives a stream the
endpoint of its encoder's number); mkaudio.py writes the one-time setup (kInit, dce_aud_hw_init, which Linux only runs
for endpoint 0) and the four tables, and each endpoint's index register. The traces carry the Samsung's audio; the kext
encodes the monitor's own from its EDID (Audio::sinkFromEdid, as dm_helpers_parse_edid_caps reads it) into the 21
endpoint registers dce_aud_az_configure fills for a DisplayPort SST stream (speaker allocation, ACP, the 14 audio
descriptors, IDs, the name; Audio::sinkValue) and puts them in at the writes to the endpoint's data register. The host
test holds the encoding to the Samsung's table and to Linux's own values for the Lenovo (pipegen with PIPEGEN_AUDIO).
The second display's pipe gets its audio when it is lit, switched or comes back on (its DIG: the enabled front end on its
OTG), and its framebuffer the pin of that DIG's endpoint (0x03 + 2 x DIG); framebuffers without a lit display get none.
On the rig the boot set up DIG0 for the Samsung and DIG1 for the Lenovo, each from its EDID, and Sound settings list
both. Not heard yet.

Display sleep and wake with rdna4-audio=1 (2026-11-27): both displays came back, the driver set up both endpoints again
on wake (DIG1 then DIG0), and both outputs stayed listed; CoreAudio answered throughout. The first `system_profiler
SPAudioDataType` after the wake took about 70 s (its helper in an uninterruptible wait), the next one 0.2 s; cause unknown
(our endpoint setup on wake, AppleGFXHDA, or another audio driver on the rig).

## Later steps

2. Whatever IOFramebuffer attributes AppleGFXHDA asks of our framebuffer.
3. DP audio packets on the stream encoder; then the second pipe and HDMI.
4. A proper match without the run-time spoof (a personality that passes probe, or the spoof from boot once it is
   known to be safe).

Checking that sound comes out needs someone at the monitor's speakers or headphone jack.
