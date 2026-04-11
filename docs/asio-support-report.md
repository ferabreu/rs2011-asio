# ASIO Support Branch — Status Report

**Branch**: `asio-support`  
**Goal**: Enable Rocksmith 2011 guitar audio input via ASIO (WineASIO/PipeWire) on
Linux/Proton, without requiring the physical Real Tone Cable to be connected.  
**Status**: awaiting further development.

---

## Background

This branch extends the RS_ASIO project, which already works for Rocksmith 2014 on Linux.
The original RS_ASIO project works by injecting a DLL that intercepts WASAPI device
enumeration and replaces the results with fake devices backed by an ASIO driver (WineASIO).
When the game activates one of those fake devices, RS_ASIO redirects the audio stream to the
configured ASIO channel.

For Rocksmith 2014, no cable is needed: it uses generic WASAPI device enumeration, so RS_ASIO
can inject a suitable fake device into the list. Rocksmith 2011 is different in that it also
requires the Real Tone Cable to be physically connected — the Wine WASAPI stack only
enumerates the cable's endpoint when the cable is present.

The goal of this branch was to fake the presence of the cable entirely in software, so that
no physical cable is needed.

---

## How Rocksmith 2011 Detects and Uses the Cable

Understanding the game's cable-handling code drove all the work on this branch. There are
two separate phases:

### Phase 1 — USB (KS) device detection

The game uses the Windows Device Installation API (SetupDi) to scan for USB audio capture
devices. It calls `SetupDiGetClassDevsW(KSCATEGORY_CAPTURE, ...)`, iterates the result with
`SetupDiEnumDeviceInterfaces`, and for each device:
1. Reads the device interface path with `SetupDiGetDeviceInterfaceDetailW`
2. Checks whether it has a render alias via `SetupDiGetDeviceInterfaceAlias` — if it does,
   the device is treated as full-duplex and skipped
3. Opens the interface registry key with `SetupDiOpenDeviceInterfaceRegKey` and reads
   `FriendlyName` via `RegQueryValueExW`
4. Checks the hardware ID for `VID_12BA&PID_00FF` via `SetupDiGetDeviceRegistryPropertyW`
5. Opens the device interface path with `CreateFileW` to get a KS handle
6. Makes a series of `DeviceIoControl(IOCTL_KS_PROPERTY, ...)` calls to query KS properties

If all checks pass, the game associates the detected friendly name with a WASAPI endpoint ID
stored under the device interface registry key, and proceeds to Phase 2.

### Phase 2 — WASAPI audio session (the target interception point)

With the cable's WASAPI endpoint ID in hand, the game calls `IMMDeviceEnumerator::GetDevice`
to retrieve the endpoint, then `IMMDevice::Activate` to open an `IAudioClient`. This is the
point where RS_ASIO's existing interception machinery operates — it detects the device ID
match (via the `WasapiDevice=` INI setting) and returns an ASIO-backed audio client instead
of the real WASAPI one.

For RS2011, RS_ASIO patches `CoCreateInstance` in the IAT so that requests for
`IMMDeviceEnumerator` return `RSAggregatorDeviceEnum` — a fake enumerator that aggregates
the real WASAPI devices with the fake ASIO-backed ones. This way, when the game calls
`GetDevice(cableWasapiId)` and then `Activate`, RS_ASIO can intercept and redirect.

---

## What Was Implemented

All code lives in `RS_ASIO/Patcher_e0f686e0.cpp`. The file targets the Rocksmith 2011 Steam
executable (CRC32 `0xe0f686e0`, `ImageBase=0x00400000`) and patches its Import Address Table
(IAT) directly. IAT patching is used instead of byte-pattern scanning because the game's
`.text` section is encrypted on disk by a custom packer (`PSFD00`), making static pattern
scanning impractical; the IAT lives in `.rdata`, which is not encrypted and is populated by
the Windows loader before RS_ASIO's code runs.

### IAT slots patched (22 total)

**COM / WASAPI enumeration (3 slots)**

| Function | RVA | Purpose |
|---|---|---|
| `CoCreateInstance` | `0x0088d47c` | Returns `RSAggregatorDeviceEnum` instead of real enumerator |
| `CoMarshalInterThreadInterfaceInStream` | `0x0088d490` | No-op marshal: stores pointer directly, bypasses COM apartments |
| `CoGetInterfaceAndReleaseStream` | `0x0088d494` | Matching unmarshal: returns stored pointer |

**SetupDi — USB device presence (8 slots)**

| Function | RVA | What the patch does |
|---|---|---|
| `SetupDiGetClassDevsW` | `0x0088d2bc` | Returns `FAKE_DEVINFO` sentinel for any call |
| `SetupDiGetClassDevsA` | `0x0088d2d4` | Same |
| `SetupDiEnumDeviceInterfaces` | `0x0088d2d8` | Returns one fake device at index 0; `ERROR_NO_MORE_ITEMS` at index 1 |
| `SetupDiGetDeviceInterfaceDetailW` | `0x0088d2c8` | Returns `FAKE_DEVICE_PATH` (`\\?\USB#VID_12BA&PID_00FF#0001#{65e8773d-...}`) |
| `SetupDiOpenDeviceInterfaceRegKey` | `0x0088d2c0` | Returns `FAKE_CABLE_HKEY` sentinel |
| `SetupDiGetDeviceRegistryPropertyW` | `0x0088d2c4` | Returns hardware ID multi-string `USB\VID_12BA&PID_00FF&REV_0103\0USB\VID_12BA&PID_00FF\0` |
| `SetupDiGetDeviceInterfaceAlias` | `0x0088d2d0` | Returns `FALSE` for the render GUID (capture-only device); `TRUE` for others |
| `SetupDiDestroyDeviceInfoList` | `0x0088d2cc` | No-op for `FAKE_DEVINFO` |

**Registry (2 slots)**

| Function | RVA | What the patch does |
|---|---|---|
| `RegQueryValueExW` | `0x0088d000` | For `FAKE_CABLE_HKEY`, returns `"Rocksmith Guitar Adapter Mono"` for `FriendlyName`; returns `FAKE_WASAPI_ENDPOINT_ID` for all other value names |
| `RegCloseKey` | `0x0088d004` | No-op for `FAKE_CABLE_HKEY` |

**KS device handle (4 slots)**

| Function | RVA | What the patch does |
|---|---|---|
| `CreateFileW` | `0x0088d128` | Returns `FAKE_KS_HANDLE` (`0xC0FFEE03`) when filepath contains `VID_12BA` |
| `CreateFileA` | `0x0088d0e8` | Same |
| `CloseHandle` | `0x0088d0c8` | No-op for `FAKE_KS_HANDLE` |
| `DeviceIoControl` | `0x0088d070` | Handles all `IOCTL_KS_PROPERTY` calls on `FAKE_KS_HANDLE` (see section below) |
| `ReadFile` | `0x0088d124` | Returns `ERROR_NOT_SUPPORTED` for `FAKE_KS_HANDLE` (logged; never called in tests) |

**WinMM waveIn (9 slots)**

| Function | RVA | What the patch does |
|---|---|---|
| `waveInGetNumDevs` | `0x0088d3fc` | Returns `realCount + 1` |
| `waveInGetDevCapsA` | `0x0088d400` | For the fake device ID, returns caps with name `"Rocksmith Guitar Adapter Mono"`, 1ch, WAVE_FORMAT_48M16 |
| `waveInOpen` | `0x0088d3e4` | Redirects the fake device ID to `WAVE_MAPPER` |
| `waveInStart/Stop/Close` | various | Pass-through with logging |
| `waveInAddBuffer/PrepareHeader/GetID` | various | Pass-through |

Note: the waveIn patches were added during investigation of the MME audio path hypothesis
(see Root Cause Analysis). They allow the game to find an MME capture device by name, but
this path was not confirmed to carry actual audio.

### KS DeviceIoControl responses

The game makes exactly 10 `IOCTL_KS_PROPERTY` calls on `FAKE_KS_HANDLE` during startup. The
current patch handles each:

| # | Property set | Property ID | Direction | outSize | Response |
|---|---|---|---|---|---|
| 1 | `KSPROPSETID_Pin` | 0x01 CTYPES | GET | 4 | `DWORD 1` (one pin type) |
| 2 | `KSPROPSETID_Connection` | 0x02 STATE | SET | 0 | `TRUE` |
| 3 | `KSPROPSETID_Connection` | 0x02 STATE | GET | 8 | `DWORD 0` (KSSTATE_STOP) |
| 4 | `KSPROPSETID_Connection` | 0x01 PRIORITY | SET | 0 | `TRUE` |
| 5 | `KSPROPSETID_Connection` | 0x01 PRIORITY | GET | 8 | `{1, 0}` (KSPRIORITY_NORMAL) |
| 6 | `{1464EDA5-6A8F-11D1-...}` | 0x00 | GET | 72 | `KSMULTIPLE_ITEM{Size=8, Count=0}` + 64 zero bytes |
| 7 | `KSPROPSETID_Pin` | 0x07 DATAFLOW | GET | 4 | `DWORD 2` (KSPIN_DATAFLOW_OUT = capture) |
| 8 | `KSPROPSETID_Pin` | 0x02 DATARANGES | GET | 4 | `DWORD 1` (one data range) |
| 9 | `KSPROPSETID_Pin` | 0x05 INTERFACES | SET | 0 | `TRUE` |
| 10 | `KSPROPSETID_Pin` | 0x05 INTERFACES | GET | 32 | `KSMULTIPLE_ITEM{Size=8, Count=0}` + 24 zero bytes |

For any property not specifically handled, the patch uses a size-based fallback:
- `outSize=0`: size probe → returns `ERROR_MORE_DATA`, `*lpBytesReturned=8`
- `outSize=4`: DWORD property → returns `DWORD 1`
- `outSize≥8`: variable-length → returns empty `KSMULTIPLE_ITEM{Size=8, Count=0}`

### WASAPI fake device

The existing RS_ASIO device enumeration infrastructure was used to expose the fake cable as
a WASAPI capture endpoint. Configuration in `dist/RS_ASIO.ini`:

```ini
[Config]
EnableWasapiInputs=0
EnableAsio=1

[Asio.Input.0]
Driver=wineasio-rsasio
Channel=0
WasapiDevice=Rocksmith Guitar Adapter Mono
```

The fake device is presented with:
- WASAPI ID: `{0.0.1.00000000}.{21D5646C-D708-4E90-A57A-E1956015D4F3}`
- Friendly name: `"Rocksmith Guitar Adapter Mono"`
- Format: 1ch, 48000 Hz, 32-bit IEEE_FLOAT
- `dwChannelMask`: 1 (front left / mono)
- FormFactor: 4 (Microphone)

---

## What Works

- **Game launches** and reaches the main menu without crashing (on warm boots)
- **All 10 KS `DeviceIoControl` calls return success** — no error is logged from the KS
  detection sequence
- **WASAPI output is fully functional**: ASIO output is initialized, buffer switches fire
  every ~5ms (48kHz / 256 frames), and in-game music plays correctly
- **Device enumeration**: the fake cable device appears in `RS_ASIO.log` with the correct
  format and properties

---

## What Does Not Work — The Core Blocker

**The game never opens an audio capture session at all.**

RS_ASIO intercepts `IMMDevice::Activate` for `IAudioClient`. Each activation is logged with
its `dwClsCtx` flag. Two values matter:
- `dwClsCtx: 1` (CLSCTX_INPROC_SERVER): a discovery/probe call, used during startup
  enumeration. This is how RS_ASIO learns about a device.
- `dwClsCtx: 17` (CLSCTX_ALL): the actual streaming activation. This is what triggers
  `IAudioClient::Initialize` and opens the ASIO channel.

In every test session, the fake cable device `{0.0.1.00000000}.{21D5646C-...}` is activated
exactly once, with `dwClsCtx: 1`. The `dwClsCtx: 17` activation — which would open the ASIO
input channel — **never happens**.

The output device `{ASIO Out}` receives its `dwClsCtx: 17` activation and works correctly.
The sequence visible in the logs is:
1. All four enumerated devices get probed with `dwClsCtx: 1`
2. The 10 KS `DeviceIoControl` calls execute and succeed
3. `SetupDiEnumDeviceInterfaces - MemberIndex: 1` appears (game ends the KS scan)
4. `{ASIO Out} Activate - dwClsCtx: 17` appears (output starts)
5. Game runs normally; guitar screens show "connect cable"

Step 4 occurs without any corresponding activation of the capture device.

---

## Root Cause Analysis

### What the evidence shows

- The game successfully runs the KS detection sequence (Phase 1) and does not log any error
- After Phase 1, the game proceeds directly to set up audio output without initiating capture
- `Patched_ReadFile` on `FAKE_KS_HANDLE` is never called (no KS streaming either)
- The `IMMDevice::Activate(dwClsCtx=17)` for the cable device never appears

### Hypothesis A — Inadequate KS property responses (most likely)

The game reads the KS property responses to decide whether the device is suitable for audio.
The current responses for calls 6 (72 bytes: allocator framing descriptor) and 10 (32 bytes:
pin interfaces descriptor) return an empty `KSMULTIPLE_ITEM{Count=0}` — meaning "zero
interfaces supported." A device that advertises no streaming interfaces would correctly be
rejected as unusable for audio.

Call 6 is particularly suspicious. The property GUID `{1464EDA5-6A8F-11D1-9AA7-00A0C9223196}`
is `KSPROPSETID_General`, property 0 = `KSPROPERTY_GENERAL_COMPONENTID`. However, given the
expected response size of 72 bytes and the context of this call (after pin-related queries),
it may instead be a request for allocator framing data (a `KSALLOCATOR_FRAMING_EX` structure),
which would contain buffer size, frame count, alignment, and memory type requirements. Zeroed
framing data (zero frame size in particular) would render the device unusable.

Call 10 (32 bytes, `KSPROPSETID_Pin` / property 5 = `KSPROPERTY_PIN_INTERFACES`) currently
returns `KSMULTIPLE_ITEM{Count=0}`. A real capture device should return at least one interface
identifier here — specifically `KSINTERFACE_STANDARD_STREAMING`. An empty list would mean
"no supported interfaces," which the game might interpret as "not an audio device."

Fixing these responses would require knowing the exact structure layouts expected. The values
for a real Real Tone Cable could be obtained via a KS trace on Windows (using ETW or the
`kstrace.exe` tool) or by logging the actual DeviceIoControl responses on a system where the
cable is connected.

### Hypothesis B — The game uses waveIn (MME), not WASAPI, for capture

The entire RS_ASIO interception strategy is built on the assumption that the game uses WASAPI
(`IAudioClient`) for guitar capture. If Rocksmith 2011 uses the legacy MME waveIn API instead,
then `IMMDevice::Activate` will never be called for capture regardless of how well the KS
detection goes, and the ASIO redirect will never trigger.

This hypothesis prompted adding the waveIn IAT patches. However, even with those patches
installed (which present a fake "Rocksmith Guitar Adapter Mono" MME device and redirect
`waveInOpen` to `WAVE_MAPPER`), the guitar screens still show "connect cable." Either the
waveIn path is not the actual capture path, or the redirection to `WAVE_MAPPER` doesn't
deliver audio in the expected format.

Note that `WAVE_MAPPER` is the system default input device. On the test system, the system
default input may not be the ASIO channel carrying guitar audio, so even if the game does
use waveIn, routing to `WAVE_MAPPER` would not deliver guitar audio anyway — it would require
routing to the specific WineASIO/PipeWire device stream.

### Hypothesis C — The game uses WASAPI, but the KS detection abort prevents activation

This is a refinement of Hypothesis A. The game's device handling code may follow this pattern:
1. Run the KS detection sequence
2. If detection succeeds fully → store cable endpoint ID, proceed to WASAPI activation later
3. If detection fails at any step → abort, treat the device as absent

The current code passes all 10 calls with status `TRUE`, so step 3 seems unlikely. However,
the *content* of the responses might cause step 2 to not record the endpoint ID, which would
make the later WASAPI activation call never happen — not because the game explicitly fails,
but because it never finds the stored ID to activate.

This is structurally the same as Hypothesis A but frames it differently: the KS calls don't
"fail" (they return TRUE), but the responses don't contain what the game needs to commit to
using the device for audio.

### The waveIn evidence and its limits

The waveIn patches were added to cover the possibility that RS2011 uses a different audio
API than RS2014. The `Patched_waveInOpen` logs would show if the game attempted to open the
fake MME device. The absence of these log entries in test sessions suggests either:
- The game does not use waveIn for capture in the code path exercised (possibly the waveIn
  path is only used as a fallback or is an older unused path)
- The game never reaches the code that opens the capture device because of an earlier gate
  (consistent with Hypotheses A/C)

---

## Known Side Issue: Cold-Boot Crash

On the first game launch after a system boot or audio server restart, the game crashes
with WineASIO reporting an error at stream stop (approximately 96 seconds into the session).
This is a WineASIO/PipeWire timing issue — PipeWire is not fully stabilised when the ASIO
stream attempts to stop cleanly. It is unrelated to the capture problem. The workaround is
to launch the game a second time in the same session, which succeeds. No fix in this branch.

---

## Logical Next Steps

### Step 1: Determine the actual audio capture API

This is the pivotal question that determines which path to take. Three approaches, in
increasing order of difficulty:

**Option A — Test with a real Real Tone Cable on the same Linux/Proton setup.**
Run the game with the physical cable while RS_ASIO logging is active. Look for:
- `{0.0.1.00000000}.{21D5646C-...} Activate - dwClsCtx: 17` — confirms WASAPI capture
- `Patched_waveInOpen` log entry with the cable's MME device index — confirms waveIn
- `Patched_ReadFile` on a KS handle — confirms raw KS streaming

A real cable test is the fastest and most definitive test. It would also show exactly what
the KS property call responses look like from a real driver, which is directly actionable.

**Option B — Compare with a working RS_ASIO-on-Windows log.**
If someone on the RS_ASIO project has a working log from Rocksmith 2011 running on Windows
with a real cable, checking whether `dwClsCtx: 17` appears on the cable device would
confirm the WASAPI path. The RS_ASIO GitHub issues/discussions page is the best place to ask.

**Option C — Decompile `Rocksmith.exe`.**
Using Ghidra or IDA with `ImageBase=0x00400000`:
1. Search for string references to `"Rocksmith Guitar Adapter"` or USB VID `12BA`
2. Find the code that calls `waveInOpen` or `IAudioClient::Initialize` for capture
3. Trace backwards from those calls to understand what gate conditions precede them

This is the most work but gives a definitive and complete answer, and would also reveal the
exact KS property values the game expects.

### Step 2 (if WASAPI): Fix the KS property responses to allow capture activation

If Step 1 confirms WASAPI capture, the problem is in the KS responses causing the game to
not commit to using the device. Specific things to fix:

**Call 10 (32 bytes, `KSPROPERTY_PIN_INTERFACES`):**
Currently returns `KSMULTIPLE_ITEM{Size=8, Count=0}` — no interfaces. Change to:
```
KSMULTIPLE_ITEM { Size=32, Count=1 }
KSIDENTIFIER    { Set={1A8766A0-62CE-11CF-A5D6-28DB04C10000}, Id=0, Flags=0 }
                   ^^^^^^^^ KSINTERFACE_STANDARD, Id=0 = KSINTERFACE_STANDARD_STREAMING
```
This tells the game that the pin supports the standard streaming interface — the minimum
required for a KS audio capture pin.

**Call 6 (72 bytes, `KSPROPERTY_GENERAL_COMPONENTID` or allocator framing):**
If this is `KSPROPERTY_GENERAL_COMPONENTID` (as suggested by the GUID `1464EDA5-...`), the
expected response is a fixed `KSCOMPONENTID` struct (44 bytes):
```c
KSCOMPONENTID {
    Manufacturer = 0x12BA,   // VID
    Product      = 0x00FF,   // PID
    Component    = 0,
    Name         = GUID_NULL,
    Version      = 1,
    Revision     = 0
}
```
There is already specific handling for this (`GUID_KSPROPSETID_General`, `propId==0`) in the
code that returns the correct VID/PID values — but only when `nInBufferSize >= sizeof(GUID) +
sizeof(ULONG)`. If the 72-byte call uses a different input format or a different property
index, this path may not be taken. Verify with additional logging: log the full property ID
for call 6 and compare it against the GUID in `GUID_KSPROPSETID_General`.

If this call is instead for allocator framing (`KSALLOCATOR_FRAMING_EX`), the fix requires
constructing a minimal valid `KSALLOCATOR_FRAMING_EX` struct:
```
KSALLOCATOR_FRAMING_EX {
  CountItems  = 1,
  PinFlags    = 0,
  OutputCompression = { RatioNumerator=1, RatioDenominator=1, RatioConstantMargin=0 },
  PinWeight   = 0,
  FramingItem[0] = {
    MemoryType    = KSMEMORY_TYPE_KERNEL_NONPAGED,
    BusType       = GUID_NULL,
    MemoryFlags   = 0, BusFlags=0, Flags=0, Frames=2, FileAlignment=3,
    MemoryTypeWeight = 0,
    PhysicalRange = { Alignment=4, MinFrameSize=4, MaxFrameSize=192000 },
    FrameRange    = { Alignment=4, MinFrameSize=960, MaxFrameSize=960 }  // 960 = 48000/50 (20ms)
  }
}
```

### Step 3 (if waveIn): Route waveIn to ASIO

If Step 1 confirms the game uses waveIn, the existing `Patched_waveInOpen` needs to be
extended to deliver actual ASIO input audio instead of redirecting to `WAVE_MAPPER`.

The mechanism: instead of calling `waveInOpen(WAVE_MAPPER, ...)`, open a dedicated waveIn
stream that reads from WineASIO's input channel. This requires either:
- A virtual loopback device that WineASIO creates in PipeWire/JACK's graph
- A small helper thread that copies samples from the ASIO input buffer into the waveIn buffer
  that the game provides via `waveInAddBuffer`

This is more complex than the WASAPI path but technically feasible. The game calls
`waveInAddBuffer(hwi, pWaveHdr, ...)` to supply buffers; the patch needs to fill those
buffers with 48kHz/16-bit_PCM mono samples from the ASIO channel at the right time.

Note: RS2011 uses `WAVE_FORMAT_48M16` (48kHz/16-bit/mono), not 32-bit float. If the ASIO
channel delivers 32-bit float, a format conversion step is needed.

### Step 4: Add targeted diagnostic logging

Before attempting any fix, adding more log output would quickly narrow down which hypothesis
is correct:

1. **Log all input bytes for DeviceIoControl call 6 specifically** — compare the property
   GUID and ID against known GUIDs to confirm what is being asked
2. **Log all bytes written back by each DeviceIoControl response** — currently only the
   input is logged; logging `nOutBufferSize` and the first 32 bytes written would reveal
   which response path was taken
3. **Add a log line in `Patched_waveInOpen` even when `uDeviceID != fakeCableID`** — this
   would show if the game calls `waveInOpen` at all, with which device ID, and what format
4. **Log `Patched_ReadFile` when `hFile != FAKE_KS_HANDLE`** — there may be other KS handles
   not captured by the fake; this would detect them even for real handles

These changes are purely additive (logging only) and carry no risk.

---

## Repository File Reference

| File | Purpose |
|---|---|
| `RS_ASIO/Patcher_e0f686e0.cpp` | **All IAT patches for Rocksmith 2011** (CRC `0xe0f686e0`) — everything on this branch |
| `RS_ASIO/Patcher.cpp` | CRC32 detection and dispatcher that calls `PatchOriginalCode_e0f686e0()` |
| `RS_ASIO/RSAsioDeviceEnum.cpp` | Fake WASAPI device enumeration; fake device construction |
| `RS_ASIO/RSAsioDevicePropertyStore.cpp` | Property store for fake WASAPI devices (FriendlyName, FormFactor, etc.) |
| `RS_ASIO/RSAsioDevice.cpp` | `Activate` interception; ASIO channel binding |
| `RS_ASIO/Configurator.cpp` | INI parsing; `WasapiDevice=` matching logic |
| `RS_ASIO/DebugWrapperDevice.cpp` | `Activate` logging; `GetWasapiRedirectDevice` matching |
| `docs/tech-details.md` | Architecture documentation written during main-branch development |
| `docs/failed-asio-support-chat.md` | First AI session: project setup and initial RS2011 analysis |
| `docs/development-chat.md` | Earlier AI sessions: RS2014 support, main-branch work |

---

## Summary

The branch successfully fakes all of the game's cable detection checks, passes the KS
property query sequence without error, presents a correctly-configured WASAPI capture
endpoint, and runs the game with working audio output. The single remaining blocker is that
the game never proceeds from cable detection to capture activation — `IAudioClient::Initialize`
for the cable device is never called.

The most likely cause is that the KS property responses, while not returning errors, return
inadequate data (specifically: empty interface and framing lists) that causes the game's
device-selection logic to silently discard the device before scheduling its WASAPI activation.
The fix is a targeted correction to calls 6 and 10, but verifying this hypothesis first with
a real-cable test or decompiler analysis would avoid guesswork.
