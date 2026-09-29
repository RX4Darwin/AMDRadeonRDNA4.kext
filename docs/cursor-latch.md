# W25: what makes a DCN 4.01 cursor update latch (and why ours stays pending)

Round 3 on the card (`RDNA4FB,Cursor`, boot 6 and 9): after every cursor write `CM_CUR0_CURSOR0_CONTROL` reads
`0x000100a4/a5`, i.e. `CUR0_UPDATE_PENDING` (bit 16) is 1, while `OTG_MASTER_UPDATE_LOCK` reads 0 (released),
`OTG_GLOBAL_SYNC_STATUS` is `0x00104104` (VUPDATE occurred) and the OTG double-buffer control reads 0. The hardware
accepted the writes but never latched them, so nothing moved into the live cursor state.

Sources are amdgpu `drivers/gpu/drm/amd/display/` (paths below relative to it; `dc/` = `display/dc/`) and the register
headers `drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_*.h`.

## 1. The answer: a separate MPC cursor lock, per OPP

Every cursor update in DC is bracketed by `cursor_lock(true)` ... writes ... `cursor_lock(false)`:

| Step | Where |
|---|---|
| set attributes: lock, `set_cursor_attribute` (HUBP then DPP), `dc_send_update_cursor_info_to_dmu`, `set_cursor_sdr_white_level`, unlock | `dc/core/dc_stream.c:287-336` (`program_cursor_attributes`; lock at `:313-316`, unlock at `:332-335`) |
| move/enable: lock, `set_cursor_position` (HUBP position/hot spot/dst offset/enable, DPP `CUR0_ENABLE`), unlock | `dc/core/dc_stream.c:446-495` (`program_cursor_position`; lock `:476`, unlock `:491`) |

On DCN 4.01 `.cursor_lock = dcn10_cursor_lock` (`dc/hwss/dcn401/dcn401_init.c:45`), whose body is
(`dc/hwss/dcn10/dcn10_hwseq.c:2290-2314`):

1. `delay_cursor_until_vupdate` (`:2242-2287`): if VUPDATE is less than ~70 us away, sleep through it so the lock
   is not held across the latch event;
2. `should_use_dmub_inbox1_lock(...)`: false on this ASIC (section 3), so
3. `dc->res_pool->mpc->funcs->cursor_lock(mpc, opp_inst, lock)`.

`mpc1_cursor_lock` (`dc/mpc/dcn10/dcn10_mpc.c:458-463`; DCN 4.01 uses it, `dc/mpc/dcn401/dcn401_mpc.c:637`) is one register write:

```c
REG_SET(CUR[opp_id], 0, CUR_VUPDATE_LOCK_SET, lock ? 1 : 0);
```

`CUR[opp]` is `regCUR_VUPDATE_LOCK_SET<opp>` (`VUPDATE_SRII(CUR, VUPDATE_LOCK_SET, inst)`, `dc/mpc/dcn10/dcn10_mpc.h:47`,
macro `dc/resource/dcn42/dcn42_resource.c:191-193`). In `dcn_4_1_0_offset.h:5458-5467` (MPC block, **BASE_IDX 3**):

| Dword (opp 0) | Register |
|---|---|
| `0x02bf` | `MPC_DPP_PENDING_STATUS` |
| `0x02c0` | `MPC_PENDING_STATUS_MISC` |
| `0x02c1` | `ADR_CFG_CUR_VUPDATE_LOCK_SET0` |
| `0x02c2` | `ADR_CFG_VUPDATE_LOCK_SET0` |
| `0x02c3` | `ADR_VUPDATE_LOCK_SET0` |
| `0x02c4` | `CFG_VUPDATE_LOCK_SET0` |
| **`0x02c5`** | **`CUR_VUPDATE_LOCK_SET0`** (bit 0 = `CUR_VUPDATE_LOCK_SET`, `dcn_4_1_0_sh_mask.h:17570-17571`) |
| `0x02c6..0x02ca` | the same five for OPP 1 (stride 5) |

The kernel comment for the callback (`dc/inc/hw/mpc.h:497-515`): "Lock cursor updates for the specified OPP. OPP defines the
set of MPCC that are locked together for cursor." While the bit is 1 the cursor-domain registers (HUBP `CURSOR0_0_*`, DPP
`CM_CUR0_*`) are written into pending space and their latch at VUPDATE is inhibited; clearing it lets the next VUPDATE take them.
It is independent of `OTG_MASTER_UPDATE_LOCK` (the pipe lock of `dcn10_lock`, surface/config updates) which the kext already
releases (`ensureUpdateLatch`). It is the only place DC ever writes `CUR_VUPDATE_LOCK_SET` (grep of `dc/`: `dcn10_mpc.c:462` only);
the ADR/CFG variants are not written by the host driver at all.

**The kext never wrote it.** Neither the parked standalone cursor code nor the W14/W21 path touched `CUR_VUPDATE_LOCK_SET0`, and the
diagnostics never read it. The GOP programs its pipe once and has no reason to touch a cursor lock, so its value at kext start is
whatever the boot firmware left; if it left the cursor lock held (or never established the unlocked state the hardware expects between
updates) every cursor write stays pending exactly as observed. This is the observation "OTG lock released, VUPDATE occurred, pending stuck".

## 2. Sequence amdgpu uses on this ASIC

**Set (`cursor set`, attributes)**, per stream, `program_cursor_attributes`:
1. `cursor_lock(true)` on the top pipe (and the next ODM pipe);
2. `dcn10_set_cursor_attribute` (`dc/hwss/dcn10/dcn10_hwseq.c:3932-3940`): `hubp32_cursor_set_attributes` (`dc/hubp/dcn32/dcn32_hubp.c:108-182`:
   address, size, control mode/pitch/lines-per-chunk, `CURSOR_SETTINGS`), then `dpp401_set_cursor_attributes`
   (`dc/dpp/dcn401/dcn401_dpp_cm.c:88-123`: `CUR0_MODE`, `CUR0_EXPANSION_MODE`, `CUR0_ROM_EN`);
3. `dc_send_update_cursor_info_to_dmu` (only for PSR/Replay panels, section 3);
4. `dcn10_set_cursor_sdr_white_level` (`dcn10_hwseq.c:3942-...`): FP scale/bias via `dpp401_set_optional_cursor_attributes` (`dcn401_dpp_cm.c:147-165`);
5. `cursor_lock(false)`.

**Move/enable (`cursor pos`)**, `program_cursor_position`:
1. `cursor_lock(true)`;
2. `dcn401_set_cursor_position` (`dc/hwss/dcn401/dcn401_hwseq.c:1040-...`): recout-space translation, hot spot for negative positions, then
   `hubp401_cursor_set_position` (`dc/hubp/dcn401/dcn401_hubp.c:806-897`: `CURSOR_ENABLE` only if it changed, `CURSOR_POSITION`, `CURSOR_HOT_SPOT`, `CURSOR_DST_OFFSET`) and
   `dpp401_set_cursor_position` (`dcn401_dpp_cm.c:125-145`: `CUR0_ENABLE` only if it changed);
3. `cursor_lock(false)`.

Nothing else is written per move. The enable bits are not written on every move; the lock is toggled around every cursor update
(twice per pointer motion).

## 3. Does the DMUB firmware own the cursor on DCN 4.01? No, not for this configuration

- **Firmware HW lock manager is bypassed.** `should_use_dmub_inbox1_lock` (`dc/dce/dmub_hw_lock_mgr.c:107-121`) returns `false` when
  `dce_version >= DCN_VERSION_4_01`, so `dcn10_cursor_lock` takes the MPC register path above, never `dmub_hw_lock_mgr_cmd(lock_cursor)`.
  (For older ASICs it would only apply to PSR-SU/Replay/eDP-PSR1 links anyway, `dmub_hw_lock_mgr.c:72-91`.)
- **Cursor offload is off by default.** It needs `dc->config.enable_cursor_offload` (`dc/dc_dmub_srv.c:1206`; nothing in the tree
  sets it, grep of `display/`), the firmware feature bit `cursor_offload_v1_support` (`:1209`), an initialised shared
  state and a `DMUB_CMD__CURSOR_OFFLOAD_INIT` (`:1221-1230`); only then does `dc_dmub_srv_is_cursor_offload_enabled` (`:2364-2367`) turn the
  `begin/update/commit_cursor_offload_update` path on (`dc_stream.c:294,310,324,329`), in which the HUBP/DPP writes are skipped
  (`dcn32_hubp.c:143`, `dcn401_hubp.c:861,866`, `dcn401_dpp_cm.c:103,112,138`). The default is the register path.
- **`DMUB_CMD__UPDATE_CURSOR_INFO` is a PSR/Replay feature.** `dc_send_update_cursor_info_to_dmu` (`dc_dmub_srv.c:1111-1155`) returns
  immediately unless `dc_dmub_should_update_cursor_data` (`:1019-1036`): PSR v1/SU or Replay panels only. A DVI/HDMI monitor
  (the card's lit pipe is `signal DVI`) never gets that command. The command exists for the firmware to replay the cursor on a panel
  in self-refresh, not to program the plane.

So no DMUB command is needed to light the cursor: the plane is programmed by host registers under the MPC cursor lock. The earlier
suspicion that `CM_CUR0` bits 2 and 7 are "firmware-owned" (commit `3c713b7`) is not supported by DC: `dpp401_set_cursor_attributes`
does not write them, and the card's own GOP state already reads `0x84` before the kext writes anything
(round-3 `armed: cm ctl=0x00000084`), so they are simply GOP/reset state.

## 4. Ranked fix plan

1. **Bracket every cursor update with the MPC cursor lock** (`CUR_VUPDATE_LOCK_SET<opp>`, dword `0x02c5 + 5*opp`, base 3) and release a
   stuck lock at arming. Host-register only. **Implemented** behind `rdna4-cursor` (`cursor.cpp`: `cursorMpcLock`, arming release, brackets in
   `cursorProgramPlane` and `drawHardwareCursor`), with a bounded wait after the unlock that logs whether `CUR0_UPDATE_PENDING` cleared.
2. **Read the whole latch picture** (all five lock-set registers, `MPC_DPP_PENDING_STATUS`, `MPC_PENDING_STATUS_MISC`) at arming and
   in every state dump, so a fix that does not work says which lock or pending bit is set. **Implemented.**
3. If the pending bit still does not clear with the lock released: check the MPCC update-lock select (`MPCC_UPDATE_LOCK_SEL`,
   already logged as `lock_sel`) points at this OTG, and that the OTG VUPDATE pulse is programmed for the cursor's OPP (already forced by
   `ensureUpdateLatch`). The evidence lines from (2) decide.
4. `delay_cursor_until_vupdate` (skip the lock if VUPDATE is < 70 us away): not needed to make the latch happen; add only if cursor tearing shows.
5. DMUB cursor offload / `UPDATE_CURSOR_INFO`: not indicated (section 3); needs the maintainer's approval before any DMUB command.

## What the card should show next (`RDNA4FB,Cursor`)

`locks:` lines at `armed`, `set`, `shown`, ...: `cur=0x00000001` at `armed` means the GOP left the cursor lock held (the hypothesis
confirmed); after `set`/`shown` the same registers read 0 and `cm ctl ... (update pending 0)` within a few milliseconds. If `cur` was already 0
and `update pending` still stays 1, item 3 of the plan is next and the `lock_sel` / `MPC_DPP_PENDING_STATUS` values say where.

## Implemented and verified (W25)

Behind `rdna4-cursor` (both `=1` and `=2`), `cursor.cpp`:

- `cursorMpcLock(bool)` writes `CUR_VUPDATE_LOCK_SET<opp>` (MPC seg 3, dword `0x02c1 + 5*opp + 4`); nested calls count. Every cursor update is
  bracketed as amdgpu does: attributes (`cursorProgramPlane`), position/enable (`drawHardwareCursor`), and the self-test's position + attributes in one bracket.
- At arming the kext reads all five lock-set registers and both MPC pending-status registers (`armed, as the GOP left it: mpc locks ...`); a held
  cursor lock is NOT released there (W25 review S1: only programmed state may latch) but by the unlock that ends the first bracket (`released the GOP-held CUR_VUPDATE_LOCK_SET at the end of the first bracket`).
- After each unlock the kext polls `CUR0_UPDATE_PENDING` for up to 40 ms and logs `cursor update LATCHED after N ms` or `still PENDING` (first
  10 checks, off the per-move path except for the first three moves).
- Every state dump also prints the `mpc locks` line.
- `rdna4-cursorlock=0` (set-boot.sh boot 17 = boot 9 plus it) leaves the lock alone: the A/B control on the card.

Emulator: the model now honours the cursor lock (no cursor plane while `CUR_VUPDATE_LOCK_SET0` is 1, `emu/qemu/rdna4.c` `rdna4_get_cursor`) and the
option `cursor-lock-stuck=on` starts it held. This encodes the hypothesis, not a measurement: it shows that the kext handles a held lock, not that the
card's lock was held. Runs (boot-6 arguments, `RDNA4_DEV=cursor=on,...`, `rdna4-cursor=2`, screendump of the 64x64 square at (100,100)):

| Setup | Result |
|---|---|
| lock stuck, fix on | log `CUR=0x00000001` at arming, `released a held CUR_VUPDATE_LOCK_SET`, `selftest: cursor update LATCHED`, **4096 magenta pixels** |
| lock stuck, `rdna4-cursorlock=0` | `CUR=0x00000001` throughout, **0 magenta pixels** |

## On the card, next

Boot 9: read the `mpc locks` line of `armed, as the GOP left it`. `CUR=0x00000001` confirms the hypothesis; the square should then appear and
`cursor update LATCHED` be logged. `CUR=0x00000000` with `still PENDING` means the cursor lock was not the problem: plan item 3
(`MPCC_UPDATE_LOCK_SEL` / MPC pending status, DPP update domain) is next, and the `dpp pending` / `misc pending` values are the first clue.
Boot 17 repeats boot 9 without the lock handling as the control.
