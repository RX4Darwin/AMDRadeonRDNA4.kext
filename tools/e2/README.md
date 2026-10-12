# E2 helpers (docs/metal-spike.md, "E2 result")

Read-only analysis of Apple's macOS 26.6.2 (25G83) x86_64 installer, everything kept OUTSIDE the repo (`~/work/tools/macos-full`). No Apple binary is in git; these are the helpers that produced the numbers in the doc.

1. `InstallAssistant.pkg` (xar) from Apple's CDN (URL, size, chunk list: see the doc); verified against Apple's per-chunk SHA-256 `*.integrityDataV1`.
2. The xar's `com.apple.FinderInfo` member is really the 18.4 GB `SharedSupport.dmg` (UDIF, HFS+): carved with `dd` at `heap + offset`, opened with `7z`.
3. Its `com_apple_MobileAsset_MacSoftwareUpdate/<hash>.zip` (universal installer asset) holds `AssetData/payloadv2/payload.NNN` = pbzx -> YAA archives.
4. `ipsw ota extract` (blacktop/ipsw v3.1.729, checksum-verified release binary) shells out to Apple's `aa`, which does not exist on Linux, so `zpay-main.go.txt` is a 100-line Go program (build it inside a checkout of blacktop/ipsw at v3.1.729 as `cmd/zpay/main.go`) that uses ipsw's own `pkg/ota/pbzx` and `pkg/ota/yaa` to list/extract files by regex: `zpay asset.zip '<regex>' <outdir|-> [payload-regex]`.
5. `kc.py` (kernel collection reader: fileset entries, symbols, x86_64 chained-fixup pointers) and `amdtables.py` (decodes `getTargetAndMethodForIndex` of the AMD user clients and the `IOExternalMethod` / `IOExternalMethodDispatch` tables).
   They read `out/payload/System/Library/KernelCollections/SystemKernelExtensions.kc`, extracted with `zpay`.
6. NOT obtainable on Linux: the OS cryptex (`payloadv2/image_patches/cryptex-system-x86_64`, a RIDIFF10 raw-image diff that only Apple's libParallelCompression applies: `ipsw`'s `ridiff.RawImagePatch` is darwin-only). That is where `AMDRadeonX6000MTLDriver`, `AMDShared`'s `libSC`, `AMDRadeonX6000Shared`, `libAppleParavirtCompilerPlugin` and the x86 dyld cache live.
