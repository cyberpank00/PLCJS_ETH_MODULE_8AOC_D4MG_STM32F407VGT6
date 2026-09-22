# AGENTS.md

Firmware for the PLCJS Ethernet 8AOC module (8x 0–20 mA sourcing current
outputs: one DAC80508 + eight XTR111 behind an MCP23S17 expander, isolated
24 V loop supply; STM32F407VGT6, KSZ8863 switch, Modbus TCP). This file is the
orientation map for agents; user-facing documentation lives in `README.md` /
`README_EN.md`. Board documentation: `TechDoc/` (schematic PDF), `TechRef/`
(designer's pinout notes).

Derived from the 8AIC firmware (fw 1.4): every shared subsystem, the
write-once calibration store, the compact "grouped by quantity" register map
and the multi-client Modbus server came from there; `dac80508/`, `mcp23s17/`
and `aoc/` are new.

## Build (CMake)

Toolchain: STM32 Arm Clang (`starm-clang`) from STM32CubeCLT, generator Ninja.
Toolchain file: `cmake/starm-clang.cmake`. Presets in `CMakePresets.json`.

```
cmake --preset Debug
cmake --build --preset Debug
```

Release: substitute `Release`. Build output:
`build/<preset>/PLCJS_ETH_MODULE_8AOC_D4MG_STM32F407VGT6.{elf,hex,bin,map}`.

No host-side unit tests. "Verified" means it compiles, and where the change is
observable it was exercised against hardware.

### Tools required
- CMake >= 3.22, Ninja
- `starm-clang` (STM32CubeCLT) on PATH. Alternative GCC toolchain at
  `cmake/gcc-arm-none-eabi.cmake`.
- Node 18+ or Python for `tools/calibrate.mjs` / `tools/calibrate.py`.

## Repository layout

| Path | Owner | Notes |
|---|---|---|
| `Application/` | hand-written | All real logic. Edit here. |
| `Core/`, `Drivers/`, `Middlewares/`, `LWIP/`, `cmake/stm32cubemx/` | STM32CubeMX | Regenerated from the `.ioc`. |
| `startup_stm32f407xx.s`, `STM32F407XX_FLASH.ld` | hand-edited | Diverged from CubeMX output — see *Linker*. |
| `tools/` | hand-written | Calibration helpers (`calibrate.mjs`, `calibrate.py`, own `README.md`). |
| `TechDoc/`, `TechRef/` | assets | Schematic PDF; designer's pinout notes (`TR.txt`). |

**CubeMX regeneration hazard.** Regenerating from the `.ioc` overwrites `Core/`,
`Drivers/`, `Middlewares/`, `LWIP/` and `cmake/stm32cubemx/CMakeLists.txt`, some
of which carry hand edits outside `USER CODE` guards (notably
`LWIP/Target/ethernetif.c`, `lwipopts.h`, and the pin block in `Core/Inc/main.h`
/ `Core/Src/gpio.c`, which was edited by hand and **not** regenerated for this
board). Diff carefully afterwards.

## Module map (`Application/`)

| Module | Responsibility |
|---|---|
| `app/` | Orchestrator: boot order, factory reset, network bring-up, housekeeping loop, 10 ms output task. Start here. |
| `spi/` | SPI1 transport shared by the DAC and the expander; **mode switched per transaction** (`spi_bus_set_mode`). |
| `dac80508/` | DAC80508 driver: 24-bit frames, soft reset, CONFIG/GAIN, per-channel and broadcast code, GAIN readback as liveness. Wiring and register values in its header. |
| `mcp23s17/` | MCP23S17 driver in *channel space*: ON mask out, EF mask in, IODIR readback as liveness. The port/bit scramble of the board is a table in the `.c`. |
| `aoc/` | Output logic: setpoint (scaled or µA) → calibration → DAC code, enable, comms-loss HOLD/SAFE/OFF, EF faults, rail power-cycle (auto + operator), channel LEDs. |
| `calstore/` | **Write-once** per-channel calibration store in Flash (8 slots). Read this file before touching calibration. |
| `temp/` | On-chip MCU temperature sensor, exposed as IR126 / HR130. |
| `modbus/modbus_app.c` | Register-map adapter. **The map is documented in the header comment of `modbus_app.h`.** |
| `modbus/modbus_tcp_server.c` | Multi-client (4 slots) TCP server on LwIP netconn; when full, the longest-silent client is evicted (newest-wins). |
| `settings/` | Flash-backed settings, CRC32-protected, incl. power-on setpoints. |
| `discovery/` | PDP responder, UDP/20556 broadcast. |
| `net_id/` | MAC and link-local IPv4 derived from the 96-bit MCU UID. |
| `ksz8863/` | SMI/MIIM driver for the Ethernet switch. |
| `led/`, `button/` | STAT_LED state machine; FACT_RES button. |
| `fw_header/` | Firmware image header consumed by the bootloader. Module identity. |
| `third_party/nanomodbus/` | Vendored protocol library, locally patched (FC15/FC16 hardening). |

### Register map shape
Multi-channel quantities are grouped **by quantity** (8 registers or 8 pairs =
channels 0..7). `float32` is two registers, **high word first**.

- Compact block (FC03/06/16) `0..55`: `0..7` int16 setpoint (RW, 0..32767 =
  scale_lo..scale_hi), `8..15` / `16..23` scale thresholds µA, `24..31` enabled
  (default 1), `32..39` comms-loss mode, `40..47` safe value µA, `48..55`
  setpoint in µA (RW). The two setpoint views write the same value — last
  write wins. Threshold writes are validated against each other (lo < hi ≤
  22000 µA) — move `hi` first when raising the whole span.
- Status (FC04) `300..333`: commanded current f32 ×8, flags ×8 (fault code in
  bits 15..8: 1 EF, 2 MCP dead, 3 DAC dead, 4 rail off), DAC code ×8, rail
  on, comms-loss active.
- `HR100` comms-loss timeout ×100 ms, `HR133` EF/liveness poll period.
- `HR118 = 0xA0FF` power-cycles the analog rail (in addition to the family
  magics).
- Calibration coefficients: `540 + ch*4` — gain, offset (mA). Nominals:
  `620..621` R_SET, `622..623` V_REF.
- `IR127` = calibration lock bitmask, bit = channel.

## Invariants

### Calibration is irreversible — treat it as destructive

`calstore/` implements a **write-once** store: each of the 8 channel slots can
be committed exactly once.

- Modbus writes to `540 + ch*4` are a **live preview only** (they change the
  output immediately) and are rejected once the slot is locked.
- Committing is `HR131 = 0xCA00 | ch`. **This is irreversible.**
- The only undo is `calstore_erase()` — two-factor: arm with `HR132 = 0xC1A5`,
  then a button hold within 30 s.
- Never issue a commit or an erase while testing, and never add a code path that
  can commit without explicit operator intent.

Calibration direction differs from the input modules: here the coefficients
**pre-distort the setpoint** (`I_cmd = gain·I_set + offset`), so the tool fits
`I_meas = a·I_cmd + b` and stores `gain = 1/a`, `offset = −b/a`.

### Single sources of truth
- **Module identity** — `Application/fw_header/fw_header.h`:
  `FW_PRODUCT_ID = 0x504C0806`, `FW_HW_REVISION = 0x0101`,
  `FW_VERSION_VALUE = 0x0100`.
- **Firmware version over Modbus** — IR120/IR121 derive from `FW_VERSION_VALUE`.
- **Register map** — the header comment of `modbus_app.h`, mirrored by the
  `MB_*` constants. Keep comment and constants in step.
- **Module ID** — `MODULE_ID_08AOC = 0x08A0`, reported in IR125.
- **Output maths** — `code = I_cmd · R_SET/10/1000 / V_REF · 65535` in
  `aoc_module.c` (`s_code_per_ma`), nominals from settings.
- **Board polarities** — `PVD_CTRL_ON_LEVEL` (`main.h`) and
  `MCP23S17_ON_ACTIVE_HIGH` (`mcp23s17.h`). Unverified on hardware at the time
  of writing; flip there, nowhere else.

### Version policy — bump the minor on every change

**Mandatory.** Every change to firmware behaviour ships with `FW_VERSION_VALUE`
in `fw_header.h` incremented by one minor (`0x0100` → `0x0101`).

- Minor bump: any firmware-only change.
- Major bump: only together with a `FW_HW_REVISION` major change (MCU pinout).
  OTA requires `fw_version` major == `hw_revision` major.
- Pure documentation-only commits do not need a bump.

Bump checklist: `FW_VERSION_VALUE` in `fw_header.h`, the version rows in
`README.md` and `README_EN.md`.

### Persistence and Flash sectors

| Sector | Address | Content |
|---|---|---|
| 10 | `0x080C0000` | Settings (`settings.c`), incl. power-on setpoints |
| 11 | `0x080E0000` | Write-once calibration (`calstore.c`) |

- `settings_t` layout is frozen; reordering or resizing requires bumping
  `SETTINGS_VERSION` (currently 1, magic `0x08A04A57`).
- Setpoints written over Modbus update `settings.ch_setpoint_ua[]` in RAM but
  are only persisted by SAVE — the Flash is not touched on every write.
- **Sector 11 conflict:** the bootloader's `flash_map.h` nominally lists sector
  11 as a third staging sector (unused). If the bootloader is ever extended to
  use it, 8AOC/8AIC/4RTD calibration is destroyed.

### Threading
- LwIP calls must run in the tcpip thread; the live network re-apply goes through
  `tcpip_callback()`.
- Flash writes and resets requested over Modbus/discovery are deferred to the
  housekeeping loop in `app_run()` via the `*_take_pending_*()` flags.
- **All SPI traffic to the isolated side stays on the `AOC` task** (10 ms tick).
  Modbus writes only update RAM and set the dirty flag; `aoc_module_tick()`
  pushes DAC codes and the ON mask. The rail power-cycle (`osDelay` ≈ 0.7 s)
  also runs there, never from the tcpip thread.
- Any loop blocking longer than the IWDG period must call
  `HAL_IWDG_Refresh(&hiwdg)`.

### Analog rail bring-up order
The DAC and the expander are powered from the isolated rail. Order is
load-bearing: rail ON → 150 ms → `dac80508_init()` (soft reset, gain ×1, all 0)
→ `mcp23s17_init()` (all OFF) → apply setpoints. A configuration attempt with
the rail down silently produces a dead device and the module will power-cycle
it 5 polls later — do not "optimise" the delay away.

### Boot order (`app_run()`)
Two ordering constraints inherited from 12DI, both load-bearing:
- The LED task starts **before** the FACT_RES button check.
- Factory reset writes Flash **before** the visual confirmation.

## Gotchas

- **Device name is 15 chars + NUL in a fixed 16-byte field**, and the PDP
  IDENTIFY response is a fixed 38 bytes. Must stay identical across every module
  variant and ModbusTool.
- Modbus TCP serves up to 4 clients (see `modbus_tcp_server.c`); register
  callbacks are shared and sequential — last write wins. Needs
  `MEMP_NUM_NETCONN/NETBUF/TCP_PCB = 8` in `lwipopts.h`.
- HR118 multiplexes distinct magics: `0xB00B` reboot, `0xB007` bootloader,
  `0x8863` switch reset, `0xA0FF` analog rail power-cycle. HR117 = `0xA5A5`
  save, HR119 = `0xDEAD` factory reset, HR131 = calibration commit, HR132 =
  calibration-erase arm.
- HR130 (on-chip temperature) is read-only despite living in holding space.
- `float32` registers are high-word-first.
- **SPI modes differ**: DAC80508 latches on the falling edge (mode 1), the
  MCP23S17 only speaks mode 0/3. `spi_bus_set_mode()` is called inside each
  driver's `cs_assert()`; a new SPI user must do the same.
- The MCP23S17 pin scramble (EF/ON swapped between GPA and GPB, channels out
  of order) is confined to `s_map[]` in `mcp23s17.c`; the rest of the code is
  channel-space.
- The comms-loss timer runs from boot. A module powered without a master goes
  to the per-channel loss mode after the timeout — intended fail-safe.
- Full scale is `V_REF·10/R_SET` ≈ 22.7 mA; setpoints and thresholds are
  capped at `SETTINGS_SCALE_MAX_UA` (22000 µA).
- There is no current readback: IR300+ reports the *commanded* current. The
  only feedback is the XTR111 EF flag (open loop / compliance / over-temp).

## Linker / memory contract with the bootloader

`STM32F407XX_FLASH.ld` is **not** a stock CubeMX script: `FLASH` origin is
`0x08040000` (256 K application slot), `RAM` length is `0x1FFF0` so the top 16
bytes hold the no-init boot-request cell at `0x2001FFF0`
(`BOOT_REQUEST_MAGIC = 0xB007CAFE`). `.fw_header` is padded to offset `0x200`.

`fw_header_t` must stay byte-identical to the bootloader's
`Application/validate/app_validate.h` (28 bytes, packed). OTA acceptance:
`product_id` exact match **and** `hw_revision` major byte match.

## Multi-repo workspace

| Repo | Role |
|---|---|
| `PLCJS_ETH_MODULE_8AOC_D4MG_...` | This module — `0x504C0806` / IR125 `0x08A0`. |
| `PLCJS_ETH_MODULE_8AIC_D4MG_...` | 8x 4–20 mA inputs, `0x504C0804` / `0x08AC`. Direct ancestor. |
| `PLCJS_ETH_MODULE_4RTD_D4MG_...` | 4x RTD, `0x504C0403` / `0x04D1`. |
| `PLCJS_ETH_MODULE_12DI_D4MG_...` | 12 discrete inputs, `0x504C1201` / `0x12D1`. Origin of the shared subsystems. |
| `PLCJS_ETH_MODULE_12DQ_D4MG_...` | 12 discrete outputs, `0x504C1202` / `0x12D0`. Origin of the comms-loss idea. |
| `BOOTLOADER_PLCJS_ETH_MODULE_STM32F407VGT6` | Shared bootloader. Owns `flash_map.h`, `app_validate.h`, `scripts/variants.csv` (`8aoc,0x504C0806,0x010101`). |
| `PLCJS_Module_ModbusTool` | Qt6/C++17 desktop client. `build8AOC()` in `src/maps/ModuleMaps.cpp` mirrors this module's register map. |

**`Application/` subsystems are copy-pasted between firmware variants, not shared
via a submodule.** A fix here is not a fix elsewhere, and vice versa.

Cross-repo contracts that must change in lockstep:
- **Wire format** (PDP frame layout, 38-byte IDENTIFY, 16-byte name) — every
  firmware + `Pdp.cpp`.
- **`fw_header_t` layout, `FW_HEADER_OFFSET`, `BOOT_REQUEST_FLAG_ADDR`/`MAGIC`,
  flash map** — every firmware + bootloader + both linker scripts.
- **product_id** — `fw_header.h` here and `scripts/variants.csv` in the
  bootloader.
- **Register map changes** — `modbus_app.h` here and `build8AOC()` in
  `ModuleMaps.cpp`, or the tool shows stale registers.

## Maintaining this file

`AGENTS.md` is a living document, not a one-time write. Update it **in the same
commit** as the change it describes. Touch it when:

- an invariant, gotcha or threading rule is added or changes — especially the
  calibration write-once / sector-11 rules and the rail bring-up order;
- a module is added, removed or repurposed (`Application/` map);
- the build procedure, toolchain or linker contract changes;
- a register-map change alters the header comment of `modbus_app.h`;
- `FW_VERSION_VALUE` is bumped and the version-policy text needs the new
  example value;
- a board polarity constant is confirmed or flipped on hardware;
- a cross-repo contract changes — update the *Multi-repo* section here **and**
  the corresponding section in the sibling repo(s).
