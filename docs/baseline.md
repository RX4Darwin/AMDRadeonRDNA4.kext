# Performance baseline

The numbers for roadmap step 10 ([issue 7](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext/issues/7)), so that "performant" is something
another person can check. Measured on 2026-11-24 with the RELEASE build of `0cc42aa`.

| | |
|---|---|
| GPU | RX 9070 XT, 16 GB, BAR0 256 MiB (Resizable BAR off) |
| CPU, memory | Ryzen 7 9700X (AM5), 16 GiB |
| macOS | Tahoe 26.6.2 (25G83) |
| Displays | a 4K display over DisplayPort, a 1920x1080 display at 144 Hz |

## Compute: `rdna4-run bench`

| Test | Result |
|---|---|
| Host to GPU, 256 MiB | 5.87 GB/s |
| GPU to host, 256 MiB | 5.22 GB/s |
| GPU reading host memory into VRAM, 64 MiB | 35.3 GB/s |
| VRAM copy on the GPU, 256 MiB | 305 GB/s copied (the tool prints 610.1, counting each byte read and written) |
| SGEMM, n=4096 | 12.7 TFLOPS on the GPU, 1.27 TFLOPS on the CPU with Accelerate |
| SGEMM, n=8192 | 12.7 TFLOPS on the GPU, 1.32 TFLOPS on the CPU |
| Matrix units, FP16, n=4096 | 66.7 TFLOPS |
| Matrix units, BF16, n=4096 | 72.4 TFLOPS |

The runs below n=1024 take well under a millisecond and several report the same time, so they say little about speed.

## Graphics

| Test | Desktop mode | Result |
|---|---|---|
| vkQuake (MoltenVK), `timedemo demo1`, 1920x1080 window, vsync off | 1920x1080 HiDPI | 27.1, 27.1 and 27.2 fps |
| The same | "looks like 2560x1440" | 26.5, 26.4 and 26.6 fps |
| `mtlprobe spin 20` | "looks like 2560x1440" | 60.0 fps, 1201 frames in 20 s |

`mtlprobe spin` is held to the display's 60 Hz, so it shows steady frame delivery and not how fast the card can draw.

## Idle power

An hour of the idle desktop, one `rdna4-run sensors` sample a minute, the display kept awake and nothing touched, on 2026-11-24 with
the RELEASE build of `8ff4f3d` and BAR0 at 1 GiB (Resizable BAR on, `ResizeAppleGpuBars` 10). All 60 samples were read, WindowServer
stayed up and no GPU hang was logged.

| | Median | Range |
|---|---|---|
| Card power (the SMU's socket power) | 47 W | 46 to 52 W |
| Graphics clock | 93 MHz | 23 to 471 MHz |
| Edge temperature | 47 °C | 46 to 47 °C |
| Hotspot temperature | 48 °C | 48 to 49 °C |
| Memory clock | 2505 MHz | not lowered at idle |
| Fans | stopped | 0 RPM throughout |

The memory clock stays at its highest step with the displays on; how much of the 47 W that costs was not measured.

## Not measured yet

- **MetalQuake's timedemo:** the app stops at its launcher and ignores the command line, so it needs a scripted way in.
- **The animated wallpaper:** measuring it means changing the desktop's wallpaper.
- **A 24-hour stability run with the Metal desktop.**

## Repeating it

Install the kext and `.pkg` from one commit and close other apps. `mtlprobe` is built from this repository:

```bash
clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -O2 -w -Iinclude -framework Foundation -framework Metal -framework AppKit -framework QuartzCore -framework IOSurface -framework IOKit tools/metal/m1/mtlprobe.m -o mtlprobe
```

```bash
sudo rdna4-run bench
```

```bash
./mtlprobe spin 20
```

vkQuake writes its result when it exits, so stop it about 45 seconds after the demo starts and read the `frames ... fps` line:

```bash
MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0 /Applications/vkQuake.app/Contents/MacOS/vkquake -basedir ~/Quake -window -width 1920 -height 1080 +vid_vsync 0 +host_maxfps 1000 +timedemo demo1 > vkquake.log
```
