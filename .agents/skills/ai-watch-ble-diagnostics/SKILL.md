---
name: ai-watch-ble-diagnostics
description: "Diagnose and repair BLE advertising, connection, GATT, TimeSync, controller recovery, and reconnect failures on AI Watch (SF32LB52, openvela/NuttX, zblue, SiFli LCPU controller). Use when a user reports BLE cannot be found, will not connect, drops after connecting, fails to rescan after disconnect, or when changing BLE, HCI, motion, display, sensor, memory-layout, or controller-recovery code in this project."
---

# AI Watch BLE Diagnostics

Use this project-specific workflow to separate radio startup failures from link, GATT, and reconnect failures. Read the main [AI Watch playbook](../ai-watch-playbook/SKILL.md) and its [BLE stability notes](../ai-watch-playbook/references/ble.md) before changing firmware.

## Triage

First inspect the contest repository status, current firmware version, recent commits, and uncommitted edits. The workspace is a multi-repository checkout; application code belongs in `contest2026_403_anpai/`, and mapped `packages/` or `vendor/openvela/` paths are not edit targets. Preserve user changes.

Classify the failure by the last verified stage:

| Stage | Evidence to collect |
|---|---|
| Boot / advertising | `AI Watch version`, HCI command completions, `aiwatch-ble: advertising as`, `ready`, and scan results |
| Connection setup | Host `Connection successful`, board `aiwatch-ble: connected`, service discovery, CCCD subscriptions, and TimeSync result |
| Connected link | `Hardware error`, controller recovery, disconnect reason, and connection duration |
| Reconnect | Disconnect reason, first advertising-resume opcode/result, address change, subsequent advertising, and fresh-scan result |

Do not treat a scan timeout as proof that the board did not advertise. Confirm it against the board log and, when needed, a live interactive `bluetoothctl` scan. Random addresses change after reboot and controller recovery; always rescan by `AI-Watch-403` before reconnecting.

## Capture And Test

Before opening the serial port for a test, stop the auto-viewer so only one process reads the port:

```bash
systemctl --user stop ai-watch-viewer
```

Use `tools/serial_watch.py` for persistent board logs; it releases DTR/RTS immediately. Opening a test serial port can reset the board, so record the reset method and avoid repeated warm resets. For the host-driven path, start with:

```bash
python3 tools/ble_adv_lottery.py -n 6 --scan-s 12
python3 tools/ble_connect_test.py -n 1 --load none --hold 45
```

Use `--load frames` only for a separate comparison against `--load none`. The connection test should reach scan, connect, service discovery, Status/DataUpload subscription, TimeSync, and the hold interval. Verify that the test tool's board-side log contains `BLE: time synced`; a host-side `Connected` message alone does not prove the full path.

The automated scanner and `bluetoothctl` output parsing have produced false `NO_ADV` results. If the script misses the name, compare the board's `advertising as` line with an interactive scan before changing firmware. A `NO_ADV` trial and a connection-stage Hardware Error are different failures. Keep the firmware hash, version, number of attempts, power-cycle/reset method, and board log with each A/B result; one boot is not enough to compare versions on this controller.

For serial or HCI capture details, see the BLE playbook and `tools/ble_adv_lottery.py`, `tools/ble_connect_test.py`, and `tools/ble_session_stats.py` in the contest repository.

## Known Application Pitfalls

The following defects were confirmed while diagnosing the 4.8.53 regression and fixed in the 4.8.55 test image:

- **Connection isolation must be per link.** `g_last_sync_utc` survives disconnects as clock status; it cannot indicate whether the current connection has synced. Start a new quiet window on each transition to `CONNECTED`, end it on this link's TimeSync or the 8-second cap, and clear its state on disconnect.
- **Do not race TX-queue cleanup with a blocking indication.** The main loop clears stale entries when disconnected while the TX worker can be inside a blocking GATT indication. Protect queue metadata, copy and consume an entry before sending, and never let the old worker advance a queue index after a disconnect reset.
- **Settle after disconnect before the first advertising command.** In the 4.8.54 capture, a normal disconnect (`reason 0x13`, link duration about 300 seconds) was followed by an immediate `0x200a` timeout (`-11` / `-110`). Controller recovery advertised again, but an immediate rescan/reconnect then produced Hardware Error after about 890 ms. After the controller settled and the host scanned the new random address, the connection succeeded. The 4.8.55 change delays the first resume attempt by 3 seconds, then retries once per second.
- **GATT write tests must use valid client transport.** `bluetoothctl write` rejected the packed TimeSync payload in this environment. A D-Bus `GattCharacteristic1.WriteValue` call with a byte array reached the board and produced `timesync accepted` / `BLE: time synced`. Do not interpret a host CLI argument-format error as an HCI failure.

User testing on `4.8.55-ble9` reported that the watch connected immediately and reconnect stability was good. Automated trials in the same session also hit several boot attempts with no advertisement, while a later captured boot completed HCI initialization and advertised. Keep boot-lottery variability separate from the measured reconnect fix; do not claim every startup is fixed without repeated runs.

## Repair Boundaries

- Inspect logs and call paths before editing. Prefer one small, reversible application-layer change per test.
- Do not change the LCPU patch blob, mailbox reservation, linker layout, zblue HCI timeout/recovery ladder, legacy advertising mode, or BT Kconfig based only on a single failed attempt. These have separate upstream dependencies and are covered by `../ai-watch-playbook/references/ble.md`.
- Changes to public `vendor/sifli`, zblue, or other shared repositories require their own branch/PR workflow; do not edit mapped vendor symlinks.
- Treat `bt ring preboot` garbage as an uninitialized-state clue only; compare it with `bt ring ready` (`size=492`) and the HCI completion sequence before diagnosing memory corruption.
- Do not assume ADC's `EN_VBAT_MON` or PA38 VSYS pinmux caused an RF failure without new evidence. The normal ADC conversion path calls `HAL_ADC_Stop()`, which clears the monitor bit; PA38 is the VSYS power-detect/gate GPIO, not an RF signal mux.
- Keep LCD power/brightness writes on the LVGL main thread. A display-thread race can present as a controller startup failure.

## Build And Flash

For code changes, bump `AI_WATCH_VERSION`, run `git diff --check`, and build with the workspace's `./build403.sh` / `cmake_out/prod` instructions. Read `../ai-watch-playbook/references/build-flash.md` and the workspace `AGENTS.md` for the current build environment.

Flash only when the user asks for hardware validation or the current task clearly authorizes it. Before flashing, back up the exact candidate image to a unique absolute path. Use the documented absolute-path `sftool` command for `/dev/ttyUSB0`; verify 100% completion and exit status 0. After a test, restore `ai-watch-viewer` if it was active before the test.

Report the failure stage, log evidence, code change, build result, flashed version/hash, number of trials, and any remaining limitation. If no current board log exists, label the cause as a hypothesis rather than a confirmed fix.
