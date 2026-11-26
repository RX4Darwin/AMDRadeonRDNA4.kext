# Audio over HDMI and DisplayPort, plan

Status, 2026-11-26: step 0 done (Apple's driver takes the card's audio function and publishes an audio device for its
codec, with no output yet). No sound yet. The route found so far is Apple's own GPU-audio driver,
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
- Next: why AppleGFXHDADriver makes no engine: its graphics association (AppleDisplay matching, the framebuffer's
  connector type, registerFramebufferNotification) and whether the codec's pin reports presence and ELD (HDA verbs
  through the controller, or AppleGFXHDA's own logging).

## Later steps

2. Whatever IOFramebuffer attributes AppleGFXHDA asks of our framebuffer.
3. DP audio packets on the stream encoder; then the second pipe and HDMI.
4. A proper match without the run-time spoof (a personality that passes probe, or the spoof from boot once it is
   known to be safe).

Checking that sound comes out needs someone at the monitor's speakers or headphone jack.
