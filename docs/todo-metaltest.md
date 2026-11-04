# Card tests for the Metal work

Written 2026-11-04 for the tip of `devel/metal` (f4dc631), for anyone with an RX 9070 XT under macOS Tahoe (OpenCore, Lilu, this
kext); the author's rig is away for repair. Everything after the iTerm2 run of 2026-10-22 (875b747's refactor,
9aa0c98, 03d842e, b3c7a9d, 2606981, f4dc631) has only been host-tested; these steps card-run it before devel/metal merges into
next.

## 0. The machine itself

First boot Tahoe with the default kext (`make`, no PVGPU) and without the `rdna4-pvgpu` boot-args: the displays come up and the
card is seen (`ioreg -l | grep -i '"device-id"'` shows `<50 75 00 00>`). On the author's rebuilt rig, also note the ESP's UUID
(`diskutil info /Volumes/EFI`) and the host name: the new board changes them.

## 1. Build (a Mac with Xcode; `make metal-helper` builds LLVM the first time, about 40 minutes)

```bash
make PVGPU=1
```

```bash
make metal-helper
```

```bash
clang -arch x86_64 -mmacosx-version-min=11.0 -fobjc-arc tools/metal/m1/mtlprobe.m -framework Foundation -framework Metal -framework IOSurface -o Build/mtlprobe
```

Outputs: `Build/AMDRadeonRDNA4-0.1.0-RELEASE/AMDRadeonRDNA4.kext`, `Build/m3/x86_64/air2amdgpu`, `Build/mtlprobe`. Copy the
helper, mtlprobe, `tools/metal/m3/install-helper.sh` and `test/glyphs2-ref.raw` to `~/rdna4/` on the test machine.

## 2. Install (test machine)

- The kext into `EFI/OC/Kexts/AMDRadeonRDNA4.kext` (back up the old one).
- Boot-args: add `rdna4-pvgpu=1 rdna4-pvgpu-class=0xff0000 rdna4-pvgpu-delay=120`.
- The helper: `sudo ~/rdna4/install-helper.sh ~/rdna4/air2amdgpu` (it is needed: the translator changed).
- Shut down and **cold boot** (a warm restart can leave compute failing at stage 4, and then every probe fails).

## 3. Probes (test machine, at least 150 s after boot: the fake GPU appears at about 121 s)

```bash
~/rdna4/mtlprobe info
```

Expect `device: Apple Paravirtual device`. Then all 29 probes, each in its own process:

```bash
~/rdna4/mtlprobe all > ~/rdna4/all.txt 2>&1
```

Pass: every probe reports its checks correct (0 pixels / values off) and every `child ended` line says `exit 0`. New on the card
this time: `coverage2`, `ctex`, `fbfetch`; all the others are re-runs after the refactor.

## 4. iTerm2's text draw against another GPU

`test/glyphs2-ref.raw` is the same draw on Apple's GPU (an Apple M5, made 2026-11-04 with
`Build/mtlprobe glyphs2 test/glyphs2-ref.raw`; the same twice in a row). On the test machine:

```bash
~/rdna4/mtlprobe glyphs2 ~/rdna4/glyphs2-card.raw
```

Compare (linear sampling and blending may round differently by a step or two):

```bash
python3 -c "import sys;a=open(sys.argv[1],'rb').read();b=open(sys.argv[2],'rb').read();d=[abs(x-y) for x,y in zip(a,b)];print(len(a),len(b),'max',max(d),'over 2:',sum(x>2 for x in d))" ~/rdna4/glyphs2-ref.raw ~/rdna4/glyphs2-card.raw
```

Pass: both 16384 bytes, nothing over 2.

## 5. iTerm2

Open iTerm2, type, scroll, resize the window, open a second tab. Pass: text as on 2026-10-22 (sharp, nothing cut off, right
colours). The author's run: no gaps between glyphs, nothing in the top-right quarter only, no cut-off letters.

## 6. Logs (either way)

```bash
sudo dmesg | grep AMDRadeonRDNA4 > ~/rdna4/dmesg.txt
```

and `/var/log/rdna4-air2amdgpu.log`. A bad boot (only the DP display, no fake GPU) is the SimpleHDA race: note it, cold boot again. Send `all.txt`, the glyphs2
comparison's line, `dmesg.txt`, the helper's log and a screenshot of iTerm2.
