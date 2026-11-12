# firmware/

Nothing in this directory but this file is in the repository.

| What | Where it comes from | Used by |
|---|---|---|
| `amdgpu/*.bin`, `amdgpu/LICENSE.amdgpu` | `tools/build/fetch-firmware.sh` fetches them from linux-firmware. AMD allows their redistribution under the licence fetched with them. | The kext embeds them (`src/fwblobs.S`) for the compute bring-up. Without them it builds and says so in the stages that need them. |
| `Sapphire.RX9070XT.16384.241213.rom` | A dump of the test card's 2 MiB flash. It is the card vendor's, so it is not distributed here. TechPowerUp's VGA BIOS collection has it: <https://www.techpowerup.com/vgabios/274368/sapphire-rx9070xt-16384-241213-1>. Put it at this path, or name another file with `make test ROM=<file>`. | `make test` parses its AtomBIOS tables and IP discovery binary (`test/atomdump.cpp`); the emulator's option ROM is made from it (`tools/vm/emu-build.sh`). |

Without the ROM dump `make test` leaves out the checks that parse it, says so, and runs the rest; the emulator's ROM
cannot be built.

The expected values in the ROM checks are those of the dump the code was written against, a Sapphire RX 9070 XT
16 GB (PCI 1002:7550, BIOS part number `113-1E4893U-N8F`, `ATOMBIOSBK-AMD VER023.008.000.068.000001`):

```
SHA-256  874e6b4646e5dcee5d3d7cba01944fad4c9824e2bdc3140427dd81b0ba58c8cc
size     2097152 bytes
```

A dump of another RX 9070 XT should pass the same checks where they are about the chip (IP versions, register
offsets); that has not been tried.
