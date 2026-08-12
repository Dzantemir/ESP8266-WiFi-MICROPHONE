<div align="center">

# 🎤 ESP8266 WiFi Microphone

### High-Fidelity Wireless Audio Streaming from ESP8266 to PC

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ESP8266 RTOS SDK](https://img.shields.io/badge/ESP8266%20RTOS%20SDK-v3.4-blue.svg)](https://github.com/espressif/ESP8266_RTOS_SDK)
[![PowerBASIC](https://img.shields.io/badge/PowerBASIC-10-red.svg)](https://www.powerbasic.com/)
[![Firmware](https://img.shields.io/badge/Firmware-v2.2-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/Platform-ESP8266%20%7C%20Windows-lightgrey.svg)]()

**24-bit I2S capture • TPDF dithering • IMA ADPCM / PCM • UDP / TCP / Raw 802.11 TX • BWSOLA packet-loss concealment • Adaptive jitter buffer • MxR-malloc heap • Real-time playback • WAV recording • Supervisor watchdog • Deep sleep recovery**

</div>

---

## ✨ Features

<table>
<tr>
<td width="50%" valign="top">

### 🎙️ Professional Audio Pipeline
- **24-bit I2S capture** with INMP441 MEMS microphone
- **TPDF dithering** (Wannamaker/Vanderkooy/Lipshitz) for 24→16-bit reduction
- **Software AGC** with 9 presets (Studio→Surveillance), per-preset attack/release/target/noise gate, and a **min-gain floor** so Limiter/Surveillance can truly *attenuate* (not just boost)
- **Fixed digital gain** (0–64×, +0 to +36 dB)
- **IRAM-optimized** ADPCM encoder (zero flash cache stalls)
- **DMA alignment fix** — zero-click audio on all sample rates (8–48 kHz)
- **I2S RX timing delays** (`AT+TIMING`) for skew compensation on long wires

</td>
<td width="50%" valign="top">

### 📡 Triple Transport Modes
- **UDP Mode**: Standard WiFi via router, 5+ Mbps throughput
- **TCP Mode**: ESP = listener, server connects. Length-prefix framing,
  guaranteed delivery, non-blocking send with backpressure. Persistent
  listening socket across stop→start cycles (no EADDRINUSE).
  Configurable via menuconfig: TCP_NODELAY, keepalive (idle/interval/count),
  SO_LINGER=0 (skip TIME_WAIT, prevent heap exhaustion),
  per-frame send deadline + EAGAIN retry cap
- **Raw 802.11 TX Mode**: Broadcast directly to Monitor Mode receiver
  - No router needed, fixed 11 Mbps TX rate (802.11b)
  - Sequence-numbered frames with auto-increment
  - WiFi channel 1–14 (`AT+WCH`)

Switchable at runtime via `AT+XPORT=0|1|2` + `AT+RST`

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 🎵 Dual Codec Support
- **IMA ADPCM** (DVI4/RFC 3551): 4 bits/sample, ~32 kbps at 16 kHz
  - RFC 3551 nibble packing (high nibble first)
  - Per-channel DVI4 header with predictor + step index
  - Stereo = two independent DVI4 blocks per packet
- **Raw PCM**: 16-bit or 24-bit signed, little-endian
  - 24-bit PCM passes through bit-perfect
  - Stereo interleaved

</td>
<td width="50%" valign="top">

### 🔄 On-the-Fly Format Switching
- Change sample rate, channels, bit depth, codec, transport **without rebooting**
- `AT+HOTRESTART` restarts the stream pipeline in ~200 ms
- Transport switch (UDP↔TCP↔RawTX) with automatic old-transport cleanup
- Receiver **auto-detects** format change from packet header
- Reopens WaveOut with new format seamlessly
- Resets ADPCM decoder state on codec change

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 🧠 Receiver DSP (EASSP Server)
- **BWSOLA PLC** (Bilateral Waveform Similarity Overlap-Add, per Yeh et al. 2013) —
  pitch-synchronous packet-loss concealment with 4 cases (both-voiced /
  prev-voiced / next-voiced / both-unvoiced). Master switch, simple
  frame-repeat fallback available
- **Adaptive jitter buffer** — 256 KB ring per device, grow-only
  (120 ms initial → 80 ms refill → 200 ms ceiling), drops oldest on
  overflow instead of triggering re-prebuffer chaos
- **Startup click suppression** — skips first 1 s (INMP441 transient +
  stale DMA), then 1 s raised-cosine fade-in (zero slope at both ends)
- **Clock drift fix** — WaveOut opens at ESP's *actual* I2S rate
  (e.g. 43860 Hz for nominal 44100), Windows resamples the rest
- **HOTRESTART detection** — distinguishes a real hot-restart (seq
  backward jump > 32768 + timestamp < 1000 ms) from natural 65535→0 wrap

</td>
<td width="50%" valign="top">

### 🛡️ Reliability & Auto-Recovery
- **Supervisor task** (software watchdog): monitors heap, pipeline liveness
  (I2S/TX frame counters), and stack high-water marks → instant
  `esp_restart()` on failure. Catches "soft" deadlocks the HW WDT can't see
- **WiFi boot retry + deep sleep**: tries AP N times, then deep sleeps 1–2 min
  → reboot → retry. Prevents zombie state when AP is unreachable
- **WiFi reconnect backoff**: exponential 1–15 s between retries at runtime
- **TCP leak fixes**: `init_listen()` no longer kills valid pre-connected
  clients; SO_LINGER=0 skips TIME_WAIT; per-frame send deadline bounds mutex hold
- **Sleep/wake recovery**: receiver detects PC sleep/wake (WM_POWERBROADCAST),
  forces CONFIGURE resend + TCP reconnect + WaveOut reopen. Survives 15+ min sleep
- **WM_DEVICECHANGE debounce**: 2 s timer absorbs HDMI/USB audio re-init bursts

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 🎛️ Full AT Command Interface
- Configure everything over UART (115200 baud)
- All settings persist in NVS flash immediately (no separate save)
- **Query form** for every parameter: `AT+CMD?` → `+CMD:value`
- **Quoted WiFi form** for SSIDs with commas:
  `AT+WIFI="My,Home,Network","secret"`
- `AT+GMR` — firmware/version info
- `AT+LOG=0|1` — mute/restore ESP_LOG output (frees UART bandwidth)
- `AT+BATT?` — battery voltage & charge (when battery monitoring enabled)
- `AT+HOTRESTART` applies audio + transport changes instantly
- `AT+HELP` — live command list straight from the device

</td>
<td width="50%" valign="top">

### 💾 MxR-malloc Heap (NEW)
- Custom heap allocator that **replaces the ESP8266 RTOS SDK default heap**
- 8-byte descriptors, **best-fit** allocation with early-exit heuristic
- **Anti-fragmentation**: anti-sliver expansion, cross-region fallback
  (DRAM ↔ IRAM) with conservative/moderate/aggressive guards
- **IRAM support** with EXEC zone reserve (exec allocations isolated)
- Up to 32 DRAM + 32 IRAM-fallback regions, configurable via menuconfig
- Three integration modes: Wrap (`--wrap=heap_caps_*`), Compat, Port
- Rich diagnostics: `mxr_get_status()` / `mxr_dump()` expose fragmentation %,
  gap count, cross-region stats, allocation failure reasons
- Transparent — existing `malloc`/`free`/`pvPortMalloc`/`heap_caps_*`
  calls are intercepted automatically in Wrap mode

</td>
</tr>
<tr>
<td width="100%" valign="top" colspan="2">

### 💻 Windows Receiver (EASSP Server)
- **Multi-device**: stream from up to 16 ESP8266s simultaneously
- **Multi-port discovery**: up to 8 simultaneous UDP discovery ports
  (default 3950, add more via the *Discovery Ports* dialog)
- **Auto-discovery**: UDP broadcast, devices appear automatically
- **Manual device list**: save up to 64 IP:port entries in INI, auto-discovered on startup
- **Device names**: shows `hostname (MAC)` in ListView — identify devices at a glance
- **Per-device output**: right-click → *Output Device* submenu → route each
  microphone to a different WaveOut device (speakers, VB-Cable, …) for
  independent use in Discord, Zoom, OBS simultaneously
- **Virtual microphone**: select VB-Cable as output → audio appears as a
  virtual mic in any application
- **Context menu**: Start/Stop, Select All / Clear All, per-device output, Stop All
- **System menu**: *Add Device…*, *Discovery Ports…*, *About…*
- **Transport auto-detect**: reads `transport_mode` from INFO, opens UDP
  socket or TCP connection automatically; per-device auto-restart on switch
- **13-column ListView**: Name(MAC), IP:Port, Status, Rate, Bits, Ch, Codec,
  RSSI, Heap, FW, Pkts, Lost, Duration — all column widths persisted to INI
- **WAV recording**: 1 GB auto-split, correct headers for all formats,
  per-device filter (only the selected device is dumped)
- **24-bit playback**: native WaveOut, auto-fallback to 16-bit
- **Real-time stats**: RSSI, heap, packet loss, duration, 64-bit byte counter
- **Single-instance**: second launch brings the running instance to the front
- **Async logging**: worker threads PostMessage to the GUI thread — no
  deadlocks during shutdown
- **Common Controls v6** themed UI, resizable window, incremental refresh (no flicker)

</td>
</tr>
</table>

---

## 📸 Social Preview

![ESP8266 WiFi Microphone](social_preview.jpg)

## 🖥️ EASSP Server (Windows Receiver)

![EASSP Server Screenshot](server.jpg)

---

## 🏗️ Architecture

### Firmware (ESP8266 RTOS SDK v3.4)

```
                    ESP8266 Firmware
 ┌─────────────────────────────────────────────────────────┐
 │                                                         │
 │  INMP441 ──I2S──> TPDF Dither ──> ADPCM/PCM ──> WiFi  │
 │   24-bit           24→16 bit       Encode        TX    │
 │   capture          dither          DVI4/PCM            │
 │                                                         │
 │  ┌──────────┐  ┌───────────┐  ┌──────────┐             │
 │  │ I2S Task │->│ Enc Task  │->│ TX Task  │             │
 │  │ prio: 5  │  │ prio: 3   │  │ prio: 2  │             │
 │  └────┬─────┘  └─────┬─────┘  └────┬─────┘             │
 │       │              │              │                   │
 │  Gain/AGC       ADPCM nibbles   transport_send()       │
 │  TPDF dither    or PCM copy     (vtable dispatch)      │
 │  I2S timing                     ┌─────┬─────┬───────┐  │
 │                                 │ UDP │ TCP │ RawTX │  │
 │                                 └─────┴─────┴───────┘  │
 │                                                         │
 │  ┌──────────────────────────────────────────────────┐  │
 │  │ MxR-malloc   (replaces SDK heap)              │  │
 │  │   best-fit · anti-fragment · DRAM↔IRAM fallback  │  │
 │  └──────────────────────────────────────────────────┘  │
 │  ┌──────────────────────────────────────────────────┐  │
 │  │ Supervisor Task (prio: 1 — software watchdog)   │  │
 │  │   • Heap < 15KB? → esp_restart()               │  │
 │  │   • Pipeline stalled 15s? → esp_restart()      │  │
 │  │   • Stack < 256B? → esp_restart()              │  │
 │  └──────────────────────────────────────────────────┘  │
 │  ┌──────────────────────────────────────────────────┐  │
 │  │ WiFi Boot Retry + Reconnect Backoff             │  │
 │  │   • Boot: try AP ×3 → deep sleep 2 min → reboot │  │
 │  │   • Runtime: backoff 1–15s between retries      │  │
 │  └──────────────────────────────────────────────────┘  │
 └─────────────────────────────────────────────────────────┘
                          │
            ┌─────────────┼─────────────┐
            ▼             ▼             ▼
         UDP socket   TCP listener   Raw 802.11
         (datagram)   (framing)      (broadcast)
            │             │             │
            └─────────────┼─────────────┘
                          ▼
                    Windows Receiver
 ┌─────────────────────────────────────────────────────────┐
 │                                                         │
 │  Transport RECV ──> Header Parse ──> ADPCM Decode ──>  │
 │  (UDP/TCP)           or PCM copy    WaveOut Playback   │
 │                                 │                       │
 │                       ┌─────────┴──────────┐            │
 │                       │  Adaptive Jitter    │            │
 │                       │  Ring (256 KB)      │            │
 │                       └─────────┬──────────┘            │
 │                                 │                       │
 │  + BWSOLA PLC (packet-loss concealment)                │
 │  + Burst-submit prebuffer (click-free startup)         │
 │  + Startup skip + raised-cosine fade-in                │
 │  + Overflow vs underrun distinction (no false clicks)  │
 │  + Clock drift fix (EspActualRate → 43860 Hz)          │
 │  + WAV Recording (1 GB auto-split, per-device filter)  │
 │  + ListView with live device stats (13 columns)        │
 │  + Multi-device simultaneous streaming                │
 │  + TCP reconnect on disconnect                        │
 │  + Sleep/wake recovery (CONFIGURE + TCP + WaveOut)     │
 │  + WM_DEVICECHANGE debounce (HDMI/USB re-init)        │
 │                                                         │
 └─────────────────────────────────────────────────────────┘
```

### Firmware Module Layout (R3-A refactor)

The firmware was refactored from a monolithic `main.c` into focused modules.
`board_config.h` is now a thin aggregator over six sub-headers (`board_audio`,
`board_wifi`, `board_network`, `board_tasks`, `board_battery`, `board_protocol`)
with compile-time cross-validation `#error` checks.

```
firmware/main/
├── main.c              app_main, WiFi boot retry, supervisor launch
├── pipeline.c          start/stop_streaming, 4 pipeline tasks, memory pools
├── supervisor.c        software watchdog (heap/stall/stack)
├── stream_control.c    streaming_* API + FreeRTOS EventGroup
├── wifi_sta.c          WiFi STA + Raw TX + reconnect backoff
├── svc_port.c          EASSP service port (DISCOVER/CONFIGURE/INFO)
├── at_cmd.c            AT command parser + dispatch table
├── at_handlers.c       AT command handlers (+HELP/+STATUS text)
├── config_mgr.c        NVS config + AGC_PRESETS[] table
├── i2s_capture.c       I2S capture + frame_ms computation
├── agc.c               AGC (Q16.16 gain loop, extracted from i2s_capture)
├── adpcm_encoder.c     IMA ADPCM encoder (IRAM hot path)
├── tpdf_dither.c       TPDF dithering
├── udp_stream.c        UDP transport (independent)
├── tcp_stream.c        TCP listener + framing + backpressure
├── rawtx_stream.c      Raw 802.11 TX transport (independent)
├── stream_mode.c       transport vtable (ops tables)
├── socket_util.c       shared setsockopt helpers
└── battery.c           optional TOUT ADC battery monitoring
```

### Server Module Layout (modularized)

The Windows receiver was split from a single `eassp_server.bas` into a main
file plus 19 focused `.inc` modules:

```
server/
├── eassp_server.bas     PBMAIN, single-instance mutex, resources, dialog
├── config.inc           constants (protocol, UI IDs, jitter, BWSOLA)
├── types.inc            DeviceInfo, LV_COL/ITM, DiscPort, DeviceUiCache
├── globals.inc          global state, device array, critical sections
├── util.inc             FormatIP, AddLog (async), IMA ADPCM tables
├── discovery.inc        multi-port UDP discovery (up to 8 ports)
├── net_cmd.inc          SendDiscover/Configure/Stop/StopForced
├── ini.inc              INI persistence (devices, ports, window, columns)
├── audio_codec.inc      ADPCM decode, EspActualRate, ring, BWSOLA PLC
├── dump.inc             WAV recording (1 GB auto-split, per-device filter)
├── stream.inc           stream lifecycle (start/stop/stop-all)
├── heartbeat.inc        HeartbeatThread + stall detection
├── device.inc           UpdateDevice (parse INFO, transport auto-restart)
├── audio_thread.inc     per-device AudioThread (playback, PLC, reconnect)
├── ui_listview.inc      ListView init, 13 columns, output routing
├── ui_layout.inc        atomic DeferWindowPos resize
├── ui_refresh.inc       incremental UI refresh (point-update cache)
├── ui_dialogs.inc       Add Device / Discovery Ports / Port Input dialogs
├── ui_main.inc          MainDlgProc (timer, menu, power, devicechange)
├── eassp_server.manifest Common Controls v6, Win7–11 supportedOS
└── eassp_server.ico     application icon
```

---

## 🔧 Hardware

### Bill of Materials

| Component | Purpose | Price |
|-----------|---------|-------|
| ESP8266 / ESP8285 (ESP-12F, ESP-12S, NodeMCU, Wemos D1, ESP-07S) | Microcontroller + WiFi | ~$2–4 |
| INMP441 I2S MEMS microphone module | 24-bit audio capture | ~$1–2 |
| USB-to-UART adapter (CP2102 / CH340) | Flashing + AT commands | ~$1–2 |

> **ESP8285**: fully compatible — an ESP8266 with integrated 1 MB flash in a
> single chip. Any ESP8285-based module (ESP-M1, ESP-M2, WROOM-02) works
> without modifications.

> **Battery operation (optional)**: enable `STREAMER_BATTERY_ENABLED` in
> menuconfig and add a voltage divider (R1=100 kΩ, R2=33 kΩ) from the battery
> to the TOUT pin. The firmware deep-sleeps on critical voltage and reports
> level via `AT+BATT?`. See `docs/wiring.md` for details.

### Alternative I2S MEMS Microphones

The project targets the INMP441 but works with any I2S MEMS mic supporting
**Philips I2S** (MSB-first) or **LSB-justified** format. Configure via
`AT+FMT=0` (Philips) or `AT+FMT=1` (LSB).

| Microphone | Bits | Format | Notes |
|------------|:----:|--------|-------|
| **INMP441** | 24-bit | Philips I2S | Default, recommended. Best SNR (61 dB). |
| **ICS-43434** | 24-bit | Philips I2S | Drop-in compatible, similar quality. |
| **ICS-43432** | 24-bit | Philips I2S | Lower power variant of ICS-43434. |
| **SPH0645LM4H-B** | 18-bit | LSB-justified | Use `AT+FMT=1` (LSB mode). 18-bit left-justified in 32-bit word. |
| **MSM261S4030H0R** | 16-bit | Philips I2S | 16-bit output, use `AT+BITS=16`. |
| **SPH0641LU4H-1** | 16-bit | LSB-justified | Use `AT+FMT=1` + `AT+BITS=16`. |

> **Wiring is identical** for all listed microphones: SCK→GPIO13, WS→GPIO14,
> SD→GPIO12, L/R→GND (left) or VDD (right). No hardware changes needed — only
> `AT+FMT` and `AT+BITS` settings differ.
>
> **24-bit vs 16-bit**: the firmware supports both natively. In 24-bit mode,
> TPDF dithering is applied before ADPCM encoding. In 16-bit mode, samples
> pass through directly (no dithering needed). Use `AT+BITS=16` for 16-bit mics.

### Wiring Diagram

```
 ┌──────────────┐              ┌──────────────┐
 │   INMP441    │              │    ESP8266   │
 │              │              │              │
 │  VDD ────────┼──────────────┼─ 3.3V        │
 │  GND ────────┼──────────────┼─ GND         │
 │  SCK ────────┼──────────────┼─ GPIO13      │
 │  WS  ────────┼──────────────┼─ GPIO14      │
 │  SD  ────────┼──────────────┼─ GPIO12      │
 │  L/R ────────┼──────────────┼─ GND (left)  │
 │              │              │              │
 └──────────────┘              └──────────────┘
```

> ⚠️ **Note**: add a 100 kΩ pulldown resistor on GPIO12 (INMP441 SD line) to
> ensure correct boot mode.

> 💡 **Tip**: place a 0.1 µF ceramic capacitor between INMP441 VDD and GND, as
> close to the microphone as possible.

> 🎧 **Studio-quality mods** (see `docs/wiring.md`): ferrite bead on VDD,
> 33–100 Ω series resistors on I2S lines, and a direct GND connection —
> yields 6–12 dB SNR improvement over basic wiring.

---

## 🚀 Quick Start

### 1. Build & Flash Firmware

We recommend using [ESP8266-IDF](https://github.com/Dzantemir/ESP8266-IDF) — a
VS Code extension that provides a complete development environment for the
ESP8266 RTOS SDK (toolchain, SDK, build system, flash/monitor) integrated
into VS Code.

```bash
# Clone this project
git clone https://github.com/Dzantemir/ESP8266-WiFi-MICROPHONE.git

# Open the firmware folder in VS Code with ESP8266-IDF installed
code ESP8266-WiFi-MICROPHONE/firmware

# In VS Code:
# 1. Copy the patched I2S driver into the SDK:
#    cp i2s.c <IDF_PATH>/components/esp8266/driver/i2s.c
# 2. Press F7 (Build)   — or Command Palette → "ESP8266-IDF: Build"
# 3. Press F8 (Flash)   — or Command Palette → "ESP8266-IDF: Flash"
# 4. Press F9 (Monitor) — or Command Palette → "ESP8266-IDF: Monitor"
```

<details>
<summary>📖 Manual build (without the VS Code extension)</summary>

```bash
# Install ESP8266 RTOS SDK v3.4
git clone --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
cd ESP8266_RTOS_SDK && git checkout release/v3.4

# Install toolchain: xtensa-lx106-elf (GCC 8.4.0)

# Copy the patched I2S driver (REPLACE the SDK file)
cp firmware/i2s.c $IDF_PATH/components/esp8266/driver/i2s.c

# Build
cd firmware
export IDF_PATH=/path/to/ESP8266_RTOS_SDK
idf.py build
idf.py flash monitor
```

</details>

> **MxR-malloc**: the custom heap allocator is enabled and configured under
> the *MxR-Malloc* menu in `idf.py menuconfig`. The defaults (single flat
> DRAM region, Wrap integration) work out of the box — no action needed for a
> first build. Tune region splits / IRAM fallback only if you want to reduce
> fragmentation under heavy allocation.

### 2. Configure WiFi & Transport

Connect to the ESP8266 UART (115200 baud) and send:

```
AT+WIFI=YourWiFiSSID,YourWiFiPassword
AT+RATE=48000
AT+BITS=24
AT+AGC=3              # 0=OFF 1=Studio 2=Podcast 3=Balanced 4=Fast 5=Noisy 6=Music 7=Limiter 8=Surveillance
AT+XPORT=0            # 0=UDP (default), 1=TCP, 2=Raw 802.11 TX
AT+HOTRESTART         # Apply changes without reboot
```

If your SSID contains commas, use the quoted form:

```
AT+WIFI="My,Home,Network","secret123"
```

<details>
<summary>⚙️ Advanced: tune reliability features via menuconfig (optional)</summary>

The firmware includes several reliability features that are **on by default**
but can be tuned via `idf.py menuconfig` → **ADPCM Streamer Configuration**:

- **WiFi Boot Retry** — retry AP connection N times, then deep sleep + reboot.
  Tune: attempts (default 3), sleep duration (default 2 min), sleep mode
  (deep vs soft).
- **WiFi Reconnect Backoff** — runtime reconnect uses exponential backoff
  between `RECONNECT_BACKOFF_MIN_MS` (1 s) and `RECONNECT_BACKOFF_MAX_MS` (15 s).
- **Supervisor Task** — software watchdog that resets the ESP on heap
  exhaustion, pipeline stall, or stack overflow. Tune: min heap (15 KB),
  stall timeout (15 s), check interval (2 s).
- **TCP socket options** — TCP_NODELAY, keepalive (idle/interval/count),
  SO_LINGER=0, per-frame send deadline (2500 ms), EAGAIN retry cap. All
  enabled by default; the send timeout is hard-required (a `#warning` fires
  if disabled).
- **Battery Monitoring** — enable + tune critical/start/bad voltages,
  divider ratio, ADC sampling, check interval, deep-sleep duration.

These defaults work well for most setups. Change them only if you experience
false resets or need different behavior.

</details>

### 3. Start the Receiver

Run `eassp_server.exe` on your Windows PC. The ESP8266 appears automatically:

1. ✅ Check the checkbox next to the device
2. ▶️ Click **Start Stream**
3. 🔊 Audio plays through your speakers!

The receiver auto-detects the transport mode from the device's INFO packet and
opens a UDP socket or TCP connection accordingly.

**Right-click** the ListView for a context menu:
- **Start Stream / Stop Stream** — for checked devices
- **Output Device ▶** — route this microphone to a specific WaveOut device
  (the currently selected device is checkmarked)
- **Select All / Clear All** — batch checkbox operations
- **Stop All Streams** — stop everything

**System menu** (click the app icon in the title bar):
- **Add Device…** — manually discover a device by IP:port (saved to INI)
- **Discovery Ports…** — open/close/change UDP discovery ports (up to 8)
- **About…** — version info

### 4. Virtual Microphone (Optional — use in Discord/Zoom/OBS)

To route the ESP8266 audio into any application as a virtual microphone:

1. Download and install **VB-Cable** from [vb-audio.com/Cable](https://vb-audio.com/Cable/) (free)
2. In EASSP Server: right-click the device → **Output Device** → **CABLE Input**
   (or select it from the "Output:" dropdown at the top)
3. In Discord/Zoom/OBS: select **CABLE Output** as your microphone
4. Audio from ESP8266 now appears as a microphone input!

For multiple microphones: install multiple VB-Cable instances (A, B, C…) and
route each ESP8266 to a different cable. Each appears as a separate microphone.

### 5. Record (Optional)

Click **DUMP** to record to WAV. Files auto-split at 1 GB and are named
`dump_HHMMSS_<idx>.wav` (e.g. `dump_143022_1.wav`). Only the selected
device is recorded; if nothing is selected, all devices are dumped (legacy).

---

## ⌨️ AT Command Reference

Every parameter command also accepts a **query form** `AT+CMD?` that returns
`+CMD:value`. Settings auto-save to NVS flash — there is no separate save
command.

| Command | Description | Example |
|---------|-------------|---------|
| `AT` | Check connection | `AT` |
| `AT+RST` | Reboot device | `AT+RST` |
| `AT+GMR` | Show firmware/version | `AT+GMR` |
| `AT+STATUS` | Full device status | `AT+STATUS` |
| `AT+WIFI=ssid,pass` | Set WiFi (quoted form supported for commas) | `AT+WIFI="My,Net","pw"` |
| `AT+HOST=name` | DHCP hostname (max 23 chars, restart to apply) | `AT+HOST=esp-mic` |
| `AT+PORT=n` | Service/discovery port (restart required) | `AT+PORT=3950` |
| `AT+TXPWR=n` | WiFi TX power (dBm, 0–20) | `AT+TXPWR=20` |
| `AT+RATE=n` | Sample rate (Hz) | `AT+RATE=48000` |
| `AT+BITS=n` | Bit depth (16 or 24) | `AT+BITS=24` |
| `AT+CH=n` | Channel: 0=L, 1=R, 2=stereo | `AT+CH=0` |
| `AT+CODEC=n` | 0=ADPCM, 1=PCM | `AT+CODEC=0` |
| `AT+AGC=n` | AGC preset 0–8 (see below) | `AT+AGC=3` |
| `AT+GAIN=n` | Fixed gain 0–64 (0=bypass) | `AT+GAIN=32` |
| `AT+FMT=n` | 0=Philips I2S, 1=LSB | `AT+FMT=0` |
| `AT+XPORT=n` | Transport: 0=UDP, 1=TCP, 2=RawTX | `AT+XPORT=1` |
| `AT+WCH=n` | WiFi channel 1–14 (RawTX only) | `AT+WCH=6` |
| `AT+TIMING=sd,ws,bck` | I2S RX input delays (0–3 each) | `AT+TIMING=0,1,0` |
| `AT+LOG=0|1` | Mute/restore ESP_LOG output | `AT+LOG=0` |
| `AT+BATT?` | Battery voltage & charge (if enabled) | `AT+BATT?` |
| `AT+HOTRESTART` | Restart stream (apply changes) | `AT+HOTRESTART` |
| `AT+FACTORY` | Factory reset | `AT+FACTORY` |
| `AT+HELP` | Show all commands | `AT+HELP` |

> **Tip**: `AT+LOG=0` mutes all `ESP_LOG` output on UART0, freeing UART
> bandwidth for AT commands. Use `AT+LOG=1` to restore logging for debugging.

---

## 🎛️ AGC Presets

9 presets selectable via `AT+AGC=0..8` or menuconfig. The AGC operates in
Q16.16 fixed-point on sign-extended int32 samples, after I2S extraction and
before TPDF dither.

| # | Name | Attack | Release | Target | Noise Gate | Min Gain | Character |
|---|------|:------:|:-------:|:------:|:----------:|:--------:|-----------|
| 0 | OFF | — | — | — | — | — | Bypass (use fixed gain via `AT+GAIN`) |
| 1 | Studio Soft | 30 | 5 | −18 dBFS | −48 dBFS | 1.0× | Very smooth, minimal pumping |
| 2 | Podcast | 50 | 15 | −18 dBFS | −42 dBFS | 1.0× | Smooth voice control |
| 3 | Voice Balanced | 75 | 20 | −18 dBFS | −42 dBFS | 1.0× | Default, good for speech |
| 4 | Voice Fast | 90 | 40 | −18 dBFS | −36 dBFS | 1.0× | Fast reaction for dynamic speech |
| 5 | Noisy Room | 60 | 25 | −15 dBFS | −30 dBFS | 1.0× | High noise gate, cuts background |
| 6 | Music | 15 | 60 | −12 dBFS | −60 dBFS | 1.0× | Slow attack (no transient squash), fast release |
| 7 | Limiter | 100 | 5 | −6 dBFS | −60 dBFS | 1/64× | Peak limiting — can attenuate to −36 dB |
| 8 | Surveillance | 95 | 80 | −12 dBFS | −60 dBFS | 1/64× | Aggressive, constant level for monitoring |

- **Attack** = gain-drop speed when signal is loud (% per frame, higher = faster)
- **Release** = gain-rise speed when signal is quiet (% per frame)
- **Target** = desired output level in dBFS (lower = more headroom)
- **Noise gate** = below this level, gain is frozen at 1× (prevents noise amplification)
- **Min gain** = floor the gain can drop to. `1.0×` (boost-only) for presets 1–6;
  `1/64×` (−36 dB) for Limiter/Surveillance so they can truly *attenuate* peaks
  instead of hard-clipping them (GROK-5 fix)

---

## 🎵 Audio Quality

This project prioritizes audio quality at every stage:

<details>
<summary>🔬 Detailed quality analysis</summary>

### Capture Stage
| Parameter | Value |
|-----------|-------|
| Microphone | INMP441 (24-bit I2S MEMS) |
| SNR | 61 dB SPL |
| Sensitivity | −26 dBFS @ 94 dB SPL |
| Bit depth | 24-bit (configurable to 16-bit) |
| Sample rates | 8, 11.025, 16, 22.05, 32, 44.1, 48 kHz |

### Processing Stage
| Technique | Purpose |
|-----------|---------|
| TPDF Dithering | Linearizes quantizer, decorrelates error (24→16 bit) |
| AGC | 9 presets with per-preset attack/release/target/noise gate/min-gain |
| Gain Smoothing | Prevents zipper noise on gain changes |
| IRAM Encoder | Zero flash cache stalls during WiFi SPI operations |
| DMA Alignment | `samples_per_frame &= ~7` — eliminates SLC word-boundary artifacts |
| I2S RX Timing | `AT+TIMING` — programmable input delays for skew compensation |

### DMA Alignment Fix

ESP8266 SLC (DMA) transfers data in 32-bit words. If `blocksize`
(= `dma_buf_len × sample_size`) is not a multiple of 4, the SLC handoff at
descriptor boundaries loses or duplicates one sample per buffer — audible as
periodic clicks at `sample_rate / dma_buf_len` Hz.

The fix aligns `samples_per_frame` to a multiple of 8 (16-bit) or 4 (24-bit)
in `main.c`, ensuring `blocksize ≡ 0 (mod 4)` and `want ≡ 0 (mod buf_size)`
— zero clicks on all sample rates (8–48 kHz), both codecs.

### ADPCM Details
- IMA ADPCM (DVI4 / RFC 3551)
- 4 bits per sample (8:1 compression vs 16-bit PCM)
- Per-channel DVI4 header (predictor + step index)
- Step table: 89 entries, index table: 16 entries
- Encoder hot path in IRAM for deterministic timing

### PCM Details
- 16-bit: 2 bytes/sample, direct passthrough
- 24-bit: 3 bytes/sample (low 3 bytes of int32), bit-perfect
- Stereo: interleaved L/R/L/R
- No compression artifacts

### Clock Drift Fix (Receiver)

ESP8266 I2S uses integer clock dividers from 160 MHz. At 44100 Hz, the actual
capture rate is 43860 Hz (−0.545%). If the receiver opens WaveOut at the
nominal 44100 Hz, the playback buffer drains faster than ESP fills it →
underruns → periodic clicks in live mode.

The receiver's `EspActualRate()` function mirrors the firmware's
`i2s_set_rate()` divider search (enumerates all divisor pairs) and opens
WaveOut at the actual rate (43860 Hz). The Windows audio engine handles the
final resample to the sound card's native rate.

</details>

---

## 📡 Protocol

<details>
<summary>📋 EASSP Protocol Specification (v2.2)</summary>

### Service Port (UDP 3950, multi-port capable)

| Command | Code | Direction | Description |
|---------|------|-----------|-------------|
| DISCOVER | 0x01 | Server→Device | Heartbeat / discovery (no payload) |
| CONFIGURE | 0x02 | Server→Device | Start streaming to port (2-byte payload: stream_port u16) |
| STOP | 0x03 | Server→Device | Stop streaming immediately (no payload) |
| INFO | 0x81 | Device→Server | Status response (58-byte payload) |

> **Protocol principle**: the receiver **never** dictates audio parameters.
> CONFIGURE only tells the device *where* to send audio. The device is the
> audio authority — the server learns the format from INFO and adapts WaveOut.

### INFO Payload (58 bytes, v2.2)

```
Offset  Field             Type     Description
0       status            u8       0=IDLE, 1=STREAMING, 2=ERROR
1       codec_id          u8       5=ADPCM, 6=PCM
2       error             u8       Error code (0=NONE…5=WATCHDOG)
3       channels          u8       1=mono, 2=stereo
4       sample_rate       u32      Hz (e.g., 44100)
8       frame_ms          u8       Frame duration in ms
9       mac[6]            u8[6]    Device MAC address
15      packets_sent      u32      Since stream start
19      free_heap         u32      Current free heap
23      wifi_rssi         i8       dBm (sign-extended)
24      firmware[8]       char[8]  Firmware version (e.g. "v2.2")
32      bits_per_sample   u8       16 or 24               (v2.0)
33      transport_mode    u8       0=UDP, 1=TCP, 2=RawTX   (v2.1)
34      hostname[24]      char[24] DHCP hostname           (v2.2)
```

### Audio Packet Header (16 bytes)

```
Offset  Field           Type     Description
0       seq_num         u16      Sequence number (wraps at 65535)
2       timestamp_ms    u32      Frame timestamp in milliseconds
6       codec           u8       5=ADPCM, 6=PCM
7       sample_rate     u8       Enum: 0=8k..6=48k
8       channels        u8       1=mono, 2=stereo
9       frame_ms        u8       Frame duration in ms
10      bitrate         u32      Audio bitrate in bps
14      bits            u16      Bits per sample (16 or 24)
```

### TCP Framing

TCP is a stream protocol (no message boundaries). Each audio frame is prefixed
with a 2-byte big-endian length:

```
[u16 length BE][16-byte pkt_header][payload]
 length = 16 + payload_len  (≤ 16384, fits in u16)
```

The receiver reads 2 bytes (length), then reads `length` bytes (frame). This
preserves frame boundaries over TCP. The 16384-byte ceiling accommodates
24-bit stereo 48 kHz / 20 ms frames (5776 B) that would exceed the UDP MTU.

### On-the-Fly Format Switching

When the ESP8266 changes format via `AT+HOTRESTART`:
1. Receiver detects changed fields in the packet header
2. Closes WaveOut → opens new WaveOut with the new format
3. Resets ADPCM decoder state
4. Continues playing — no user intervention needed

A real hot-restart is distinguished from the natural 65535→0 sequence wrap by
checking: seq backward modular jump > 32768 **and** timestamp < 1000 ms **and**
lastSeq > 1000.

</details>

---

## 🏗️ Transport Architecture

The firmware uses a **vtable pattern** (`stream_mode_ops_t`) to abstract
transport differences. Three independent transport modules share one common
pipeline:

```
                    Common Pipeline (pipeline.c)
                    ┌──────────────────────────┐
  I2S capture  ───> │ i2s_task_fn              │
                    │   ↓ (PCM frames)          │
  ADPCM/PCM    ───> │ adpcm_task_fn/pcm_task_fn │
                    │   ↓ (encoded frames)      │
  Send         ───> │ stream_task_fn            │
                    │   ↓ transport_send()       │  ← vtable dispatch
                    └──────────┬──────────────────┘
                               │
              ┌────────────────┼────────────────┐
              ▼                ▼                ▼
         ┌─────────┐     ┌──────────┐     ┌───────────┐
         │ UDP     │     │ TCP      │     │ RawTX     │
         │ socket  │     │ listener │     │ 802.11 TX │
         │         │     │ +framing │     │ broadcast │
         └─────────┘     └──────────┘     └───────────┘
         udp_stream.c    tcp_stream.c     rawtx_stream.c
```

Each transport module is **fully independent** — own state, own header file,
no shared variables, no `if (transport == ...)` branches. Adding a 4th
transport requires only a new `.c` file + one entry in `stream_mode.c`.
Shared socket-option boilerplate lives in `socket_util.c`.

---

## 🛡️ Reliability & Auto-Recovery

This project is designed for **unattended 24/7 operation**. Multiple layers of
fault detection and recovery keep the system working even when things go wrong.

### Supervisor Task (Software Watchdog)

The ESP8266 hardware WDT is fed by the IDLE task and only fires when a task
**hogs the CPU without yielding**. The most insidious bugs involve tasks that
*do* yield (via `vTaskDelay`, `xQueueReceive` with timeout, `sendto` with
`SO_SNDTIMEO`) but produce no useful work — soft deadlocks the HW WDT can't see.

The **supervisor task** (low priority, runs when idle) checks every 2 s:

| Check | Threshold | What it catches |
|-------|-----------|-----------------|
| Free heap | < 15 KB | TIME_WAIT leak, pbuf leak, heap fragmentation death spiral |
| Pipeline liveness | I2S + TX counters stalled 15 s | Deadlocked mutex, sendto timeout loop, orphaned task |
| TX stalled, I2S active | TX not sending 15 s, I2S reading | TX task stuck in send(), transport not ready |
| Stack high-water | < 256 bytes | Stack overflow (would corrupt heap or crash erratically) |

If any check fails: log the reason → `esp_restart()` → clean reboot.
Configurable via menuconfig.

### WiFi Boot Retry + Deep Sleep + Runtime Backoff

On boot, the ESP tries to connect to the AP multiple times. If all attempts
fail, it enters deep sleep, then reboots and retries. At runtime, lost
connections use exponential backoff.

```
Boot → WiFi init → Attempt 1/3 (15s timeout)
                         ├─ Success → normal operation
                         └─ Fail → Attempt 2/3 → ... → Fail
                                    └─ DEEP SLEEP 2 min → reboot → repeat

Runtime drop → reconnect backoff 1s → 2s → 4s → ... → 15s (cap)
```

- **Only for UDP (0) and TCP (1)**. RawTX (2) doesn't use AP association.
- **Deep sleep** (default): requires GPIO16 (XPD_DCDC) connected to RST.
  Lowest power. On wake, ESP reboots and runs `app_main` from scratch.
- **Soft sleep**: `vTaskDelay` + `esp_restart()`. Works without hardware mod.

### TCP Hardening

- **Reconnect leak fix**: `tcp_stream_init_listen()` no longer kills valid
  pre-connected clients when reusing the listening socket (was leaking
  ~8 KB pbuf per killed connection → heap exhaustion after 3–4 stop/start cycles).
- **SO_LINGER=0** on client sockets: RST close skips TIME_WAIT entirely.
- **Per-frame send deadline** (`TCP_FRAME_SEND_DEADLINE_MS` = 2500 ms): bounds
  the total wall-clock time to send one complete frame (partial sends +
  EAGAIN retries combined), so a slow-reader client can't hold the send mutex
  indefinitely.
- **EAGAIN retry cap** (`TCP_EAGAIN_MAX_RETRIES` = 0): one send attempt per
  call — total worst-case send time stays under the stream stop timeout.
- **Cross-validation**: `#error` in `board_config.h` enforces
  `stop_timeout > send_timeout × (retries+1)` and `stop_timeout > frame_deadline`,
  so `stop_streaming()` can never force-delete the TX task while it still
  holds the send mutex.

### BWSOLA Packet-Loss Concealment (Receiver)

When a UDP packet is lost, simple silence insertion produces an audible click
and a gap. The receiver implements **Bilateral Waveform Similarity
Overlap-Add** (BWSOLA, per Yeh et al. 2013) to synthesize a concealed frame:

1. Copy the last good PCM frame into stable storage.
2. On the next good packet, classify both edges as **voiced** or **unvoiced**
   using normalized autocorrelation (pitch detection, 80–400 samples @ 48 kHz).
3. Apply one of four cases: **BV** (both voiced) / **PV** (prev voiced) /
   **NV** (next voiced) / **BU** (both unvoiced), each with a tailored
   linear crossfade.
4. Write the concealed frame into the ring *before* the current packet.

Multi-packet loss: one BWSOLA reconstruction + up to three simple repeats.
Master switch `BWSOLA_PLC_ENABLE`; simple frame-repeat fallback available.

### Adaptive Jitter Buffer (Receiver)

A 256 KB per-device ring absorbs WiFi jitter. The target depth is **grow-only**:

| Phase | Target | When |
|-------|--------|------|
| Initial prebuffer | 120 ms | cold start, before first play |
| Refill after underrun | 80 ms | faster recovery, accepts bursty WiFi |
| Adaptive ceiling | 200 ms | grows +20 ms every 30 s if underruns persist |

On **overflow** (network faster than sound card), the oldest bytes are dropped
and the newest audio is kept — no packet drop, no re-prebuffer chaos. On
**underrun**, one silence frame is injected and the consumer pauses until the
ring refills to the 80 ms threshold.

### Burst-Submit Prebuffer (Click-Free Startup)

The prebuffer does **not** call `waveOutWrite` per frame during fill — instead
it marks buffers ready and, once the jitter target is reached, **burst-submits
all ready buffers at once**. WaveOut sees a full queue from the very first
moment, eliminating underrun during the critical startup window.

Overflow (all 16 buffers INQUEUE) is no longer falsely counted as underrun —
the packet is simply dropped (the network is ahead, losing one frame is fine).

### Startup Click Suppression

The first second of audio after stream start is skipped (INMP441 startup
transient + stale I2S DMA), then a 1 s **raised-cosine fade-in**
(`0.5 × (1 − cos(π·progress))`) ramps the gain — zero slope at both ends,
no derivative-discontinuity click.

### Sleep/Wake Recovery (Receiver)

When the PC enters sleep/standby and wakes:

1. **PBT_APMSUSPEND**: immediately flag WaveOut reopen (closes TCP socket
   before the network stack goes down).
2. **PBT_APMRESUMEAUTOMATIC / SUSPEND**: 2 s debounce timer, then reopen.
3. **WM_DEVICECHANGE** (HDMI/USB audio re-init): same 2 s debounce.
4. Reopen path: close dead TCP socket → reconnect → reset pipeline state
   (ADPCM predictors, ring, seq tracking, PLC, skip/fade) → reopen WaveOut
   with `EspActualRate` drift fix → re-arm startup skip + fade-in.

A 5 s reopen-suppression window prevents duplicate reopens when multiple
sources (power + devicechange + waveOutWrite error) fire simultaneously.

---

## 💾 MxR-malloc (Custom Heap Allocator)

The firmware ships with **MxR-malloc**, a custom heap allocator that
replaces the ESP8266 RTOS SDK's default heap. It lives in
`firmware/components/mxr_malloc/` and is configured under the *MxR-Malloc*
menu in `idf.py menuconfig`.

### Why a custom allocator?

The SDK's heap is a simple first-fit allocator that fragments badly under the
bursty allocation pattern of an audio pipeline (variable-size frames, frequent
alloc/free, IRAM-critical encoders). MxR-malloc improves on this with:

- **8-byte descriptors** (vs. the SDK's larger block headers) → less overhead
- **Best-fit** allocation with an early-exit heuristic (accept a gap if waste
  ≤ 25% of the request) → smaller fragmentation
- **Anti-sliver** expansion: if the leftover after carving a block is smaller
  than `MXR_MIN_SLICE_BYTES`, the block is expanded to fill the whole gap
  (no unusable fragments)
- **Cross-region fallback**: DRAM ↔ IRAM with configurable guards
  (conservative 50% / moderate 75% / aggressive 90% / all 100%) — a request
  that doesn't fit in its home region can spill into the other, bounded by a
  guard so one region isn't starved
- **IRAM EXEC zone reserve**: executable allocations are isolated into a
  reserved IRAM zone; non-exec 32-bit allocations can use IRAM fallback but
  not the EXEC zone
- **Up to 32 DRAM + 32 IRAM-fallback regions**, configured as a
  comma-separated `"<bytes>-<percent>%"` string (e.g. `"4-25%,64-50%,512-25%"`)

### Integration modes

| Mode | How it hooks in | When to use |
|------|-----------------|-------------|
| **Wrap** (default) | `--wrap=heap_caps_*`, `malloc`/`free`/`calloc`/`realloc`/`zalloc`, `esp_get_free_heap_size` | Drop-in — existing code works unchanged |
| **Compat** | ESP heap compatibility layer (`heap_caps_*` shims) | Selective use via `mxr_malloc_caps()` |
| **Port** | Defines `malloc`/`free`/… directly | Maximum control; requires excluding newlib libc symbols |

### Diagnostics

```c
mxr_status_t s;
mxr_get_status(&s);
// s.free_bytes, s.min_free_bytes, s.largest_free_block_bytes,
// s.fragmentation_pct, s.gap_count, s.sliver_count,
// s.cross_region_allocs, s.alloc_fail_no_memory, ...
mxr_dump();   // full region dump to log
```

The defaults (single flat DRAM region, Wrap mode) work out of the box — no
configuration needed for a first build.

---

## 📁 Project Structure

```
ESP8266-WiFi-MICROPHONE/
├── README.md                      # You are here
├── LICENSE                        # MIT
├── social_preview.jpg             # GitHub social preview image
├── server.jpg                     # EASSP Server screenshot
│
├── firmware/                      # ESP8266 firmware (ESP8266 RTOS SDK v3.4)
│   ├── CMakeLists.txt             # Project definition
│   │
│   ├── components/
│   │   ├── mxr_malloc/            # MxR-malloc custom heap allocator
│   │   |   ├── CMakeLists.txt     # Region validation + wrap flags
│   │   |   ├── Kconfig.projbuild  # MxR-Malloc menu
│   │   |   ├── mxr_malloc.c       # Core allocator
│   │   |   ├── mxr_heap_wrap.c    # Wrap-mode hooks (--wrap=heap_caps_*)
│   │   |   ├── mxr_heap_compat.c  # ESP heap compat layer
│   │   |   ├── mxr_heap_port.c    # Port-mode (direct malloc/free)
│   │   |   └── include/mxr_malloc.h
│   │   |
|   |   └── i2s_custom/
|   |       ├── CMakeLists.txt
|   |       ├── i2s.c
|   |       └── include/driver/i2s.h
|   |
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild      # menuconfig options
│       ├── main.c                 # app_main + WiFi boot retry
│       ├── pipeline.c             # start/stop_streaming + 4 pipeline tasks + pools
│       ├── supervisor.c           # software watchdog (heap/stall/stack)
│       ├── stream_control.c       # streaming_* API + EventGroup
│       ├── stream_mode.c          # Transport vtable (UDP/TCP/RawTX ops)
│       ├── wifi_sta.c             # WiFi STA + Raw TX + reconnect backoff
│       ├── udp_stream.c           # UDP transport (independent)
│       ├── tcp_stream.c           # TCP listener + framing + backpressure
│       ├── rawtx_stream.c         # Raw 802.11 TX transport (independent)
│       ├── at_cmd.c               # AT command parser + dispatch table
│       ├── at_handlers.c          # AT command handlers
│       ├── config_mgr.c           # NVS config + AGC_PRESETS[] table
│       ├── svc_port.c             # EASSP service port (DISCOVER/CONFIGURE/INFO)
│       ├── i2s_capture.c          # I2S capture + frame_ms computation
│       ├── agc.c                  # AGC (Q16.16 gain loop)
│       ├── adpcm_encoder.c        # IMA ADPCM encoder (IRAM)
│       ├── tpdf_dither.c          # TPDF dithering
│       ├── socket_util.c          # shared setsockopt helpers
│       ├── battery.c              # optional TOUT ADC battery monitoring
│       └── include/               # Header files
│           ├── board_config.h         # thin aggregator (R3-C)
│           ├── board_audio.h          # audio params + AGC presets + I2S
│           ├── board_wifi.h           # WiFi defaults + boot retry + backoff
│           ├── board_network.h        # service port + transport + TCP tuning
│           ├── board_tasks.h          # task priorities/stacks + supervisor
│           ├── board_battery.h        # battery monitoring constants
│           ├── board_protocol.h       # FIRMWARE_VERSION + packet sizes
│           ├── stream_mode.h          # transport ops vtable + wrappers
│           ├── stream_control.h       # streaming_* API
│           ├── pipeline_internal.h    # pipeline externs (tasks, counters)
│           ├── udp_stream.h / tcp_stream.h / rawtx_stream.h
│           ├── config_mgr.h           # device_config_t + setters
│           ├── svc_protocol.h         # EASSP protocol (INFO v2.2, 58 bytes)
│           ├── packet_format.h        # audio packet header (16 bytes)
│           ├── agc.h / adpcm_encoder.h / tpdf_dither.h / i2s_capture.h
│           ├── wifi_sta.h / svc_port.h / at_cmd.h / at_internal.h
│           ├── socket_util.h / battery.h
│           └── rawtx_stream.h
│
├── server/                        # Windows receiver (PowerBASIC 10)
│   ├── eassp_server.bas           # PBMAIN, single-instance mutex, dialog
│   ├── config.inc                 # constants (protocol, UI, jitter, BWSOLA)
│   ├── types.inc                  # DeviceInfo, LV_COL/ITM, DiscPort, UiCache
│   ├── globals.inc                # global state + critical sections
│   ├── util.inc                   # FormatIP, AddLog (async), ADPCM tables
│   ├── discovery.inc              # multi-port UDP discovery (up to 8)
│   ├── net_cmd.inc                # SendDiscover/Configure/Stop/StopForced
│   ├── ini.inc                    # INI persistence (devices/ports/window)
│   ├── audio_codec.inc            # ADPCM decode + EspActualRate + BWSOLA PLC
│   ├── dump.inc                   # WAV recording (1 GB auto-split)
│   ├── stream.inc                 # stream lifecycle (start/stop/stop-all)
│   ├── heartbeat.inc              # HeartbeatThread + stall detection
│   ├── device.inc                 # UpdateDevice + transport auto-restart
│   ├── audio_thread.inc           # per-device AudioThread (playback/PLC)
│   ├── ui_listview.inc            # ListView + 13 columns + output routing
│   ├── ui_layout.inc              # atomic DeferWindowPos resize
│   ├── ui_refresh.inc             # incremental UI refresh (point-update)
│   ├── ui_dialogs.inc             # Add Device / Discovery Ports dialogs
│   ├── ui_main.inc                # MainDlgProc (timer/menu/power/devicechange)
│   ├── eassp_server.manifest      # Common Controls v6, Win7–11
│   └── eassp_server.ico           # application icon
│
└── docs/
    ├── wiring.md                  # Hardware wiring + studio-quality mods
    └── protocol.md                # EASSP binary protocol specification
```

---

## ⚙️ menuconfig Options

All options live under `idf.py menuconfig` → **ADPCM Streamer Configuration**
(and the separate **MxR-Malloc** menu). Key options:

### Audio

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_SAMPLE_RATE` | 16000 | 8k / 11.025k / 16k / 22.05k / 32k / 44.1k / 48k |
| `STREAMER_I2S_BITS_PER_SAMPLE` | 24 | 16 or 24 |
| `STREAMER_I2S_COMM_FORMAT` | Philips | Philips I2S or LSB-justified |
| `STREAMER_AUDIO_CHANNELS` | Left | Left / Right / Stereo |
| `STREAMER_AUDIO_CODEC` | ADPCM | ADPCM or PCM |
| `STREAMER_AUDIO_GAIN` | 32 | 0 (bypass) / 8 / 16 / 32 / 48 / 64 |
| `STREAMER_AGC_MODE` | Voice Balanced | OFF → Surveillance (9 presets) |

### Transport & I2S Timing

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_TRANSPORT_MODE` | UDP | UDP / TCP / Raw 802.11 TX |
| `STREAMER_RAWTX_CHANNEL` | 1 | WiFi channel 1–14 (RawTX only) |
| `STREAMER_I2S_TIMING_SD_DELAY` | 0 | RX SD delay (0–3) |
| `STREAMER_I2S_TIMING_WS_DELAY` | 0 | RX WS delay (0–3) |
| `STREAMER_I2S_TIMING_BCK_DELAY` | 0 | RX BCK delay (0–3) |

### Battery Monitoring

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_BATTERY_ENABLED` | n | Enable TOUT ADC battery monitoring |
| `STREAMER_BATT_CRITICAL_MV` | 3700 | Below → deep sleep (mV) |
| `STREAMER_BATT_START_MV` | 3900 | Below on boot → don't start (mV) |
| `STREAMER_BATT_BAD_MV` | 2500 | Below → reading invalid (mV) |
| `STREAMER_BATT_DIVIDER_RATIO` | 5711 | R1=100k, R2=33k calibration |
| `STREAMER_BATT_ADC_SAMPLES` | 15 | ADC samples averaged per reading |
| `STREAMER_BATT_ADC_DELAY_MS` | 50 | Delay between ADC samples (ms) |
| `STREAMER_BATT_CHECK_MIN` | 1 | Check interval during operation (min) |
| `STREAMER_BATT_SLEEP_MIN` | 30 | Deep sleep duration on critical (min) |
| `STREAMER_BATT_TASK_STACK` | 1024 | Battery task stack |
| `STREAMER_BATT_TASK_PRIO` | 3 | Battery task priority |

### WiFi Defaults & Boot Retry

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_WIFI_SSID` | YOUR_WIFI_SSID | Default SSID (override via `AT+WIFI`) |
| `STREAMER_WIFI_PASSWORD` | 12345678 | Default password |
| `STREAMER_WIFI_HOSTNAME` | esp-streamer | DHCP hostname |
| `STREAMER_WIFI_CONNECT_TIMEOUT_MS` | 15000 | Per-attempt connect timeout |
| `STREAMER_WIFI_RECONNECT_BACKOFF_MIN_MS` | 1000 | Runtime reconnect backoff start |
| `STREAMER_WIFI_RECONNECT_BACKOFF_MAX_MS` | 15000 | Runtime reconnect backoff cap |
| `STREAMER_WIFI_BOOT_RETRY_ENABLED` | y | Retry + deep sleep on boot failure |
| `STREAMER_WIFI_BOOT_RETRY_ATTEMPTS` | 3 | Attempts before sleep |
| `STREAMER_WIFI_BOOT_SLEEP_MINUTES` | 2 | Deep sleep duration (min) |
| `STREAMER_WIFI_BOOT_SLEEP_MODE` | 0 | 0=deep sleep, 1=soft sleep |

### Service Port (EASSP)

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_SVC_PORT` | 3950 | Discovery/service UDP port |
| `STREAMER_SVC_WATCHDOG_TIMEOUT_MS` | 15000 | Stop if no DISCOVER heartbeat |
| `STREAMER_SVC_INFO_INTERVAL_MS` | 1000 | INFO announce interval |
| `STREAMER_SVC_ANNOUNCE_ENABLED` | n | Broadcast announcements |
| `STREAMER_SVC_ANNOUNCE_MIN_MS` | 1000 | Announce jitter min |
| `STREAMER_SVC_ANNOUNCE_MAX_MS` | 5000 | Announce jitter max |
| `STREAMER_SVC_RECV_BUF_SIZE` | 256 | Receive buffer size |
| `STREAMER_SVC_RECONFIGURE_STOP_TIMEOUT_MS` | 2000 | Stop timeout on reconfigure |

### Pipeline & Tasks

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_TASK_PRIO_I2S` | 5 | I2S capture task priority |
| `STREAMER_TASK_PRIO_ADPCM` | 3 | Encoder task priority |
| `STREAMER_TASK_PRIO_UDP` | 2 | Sender task priority |
| `STREAMER_TASK_PRIO_AT` | 1 | AT command task priority |
| `STREAMER_TASK_PRIO_SVC` | 2 | Service port task priority |
| `STREAMER_TASK_PRIO_TCP_ACCEPT` | 4 | TCP accept task priority |
| `STREAMER_TASK_STACK_I2S` | 3584 | I2S task stack |
| `STREAMER_TASK_STACK_ADPCM` | 2560 | Encoder task stack |
| `STREAMER_TASK_STACK_UDP` | 3072 | Sender task stack |
| `STREAMER_TASK_STACK_AT` | 3584 | AT task stack |
| `STREAMER_TASK_STACK_SVC` | 3584 | Service port task stack |
| `STREAMER_TCP_ACCEPT_TASK_STACK` | 1024 | TCP accept task stack |
| `STREAMER_STREAM_STOP_TIMEOUT_UDP_MS` | 3000 | UDP stream stop timeout |
| `STREAMER_STREAM_STOP_TIMEOUT_TCP_MS` | 3000 | TCP stream stop timeout |

### AT Command Interface

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_AT_CMD_ENABLED` | y | Enable AT command interface |
| `STREAMER_UART_BAUD_RATE` | 115200 | UART baud rate |

### Network / UDP / TCP

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_UDP_RECEIVE_TIMEOUT_MS_ENABLED` | y | UDP SO_RCVTIMEO |
| `STREAMER_UDP_RECEIVE_TIMEOUT_MS` | 2000 | UDP receive timeout |
| `STREAMER_UDP_SEND_TIMEOUT_MS_ENABLED` | y | UDP SO_SNDTIMEO |
| `STREAMER_UDP_SEND_TIMEOUT_MS` | 2000 | UDP send timeout |
| `STREAMER_TCP_SEND_TIMEOUT_MS_ENABLED` | y | TCP SO_SNDTIMEO (**required**) |
| `STREAMER_TCP_SEND_TIMEOUT_MS` | 2000 | TCP send timeout |
| `STREAMER_TCP_NODELAY_ENABLED` | y | Disable Nagle (low-latency) |
| `STREAMER_TCP_KEEPALIVE_ENABLED` | y | TCP keepalive on client sockets |
| `STREAMER_TCP_KEEPIDLE` | 10 | Seconds before first probe |
| `STREAMER_TCP_KEEPINTVL` | 3 | Seconds between probes |
| `STREAMER_TCP_KEEPCNT` | 3 | Failed probes = dead (~19 s total) |
| `STREAMER_TCP_LINGER_ENABLED` | y | SO_LINGER=0 (RST close, skip TIME_WAIT) |

### Supervisor Task (software watchdog)

| Option | Default | Description |
|--------|:-------:|-------------|
| `STREAMER_SUPERVISOR_ENABLED` | y | Software watchdog |
| `STREAMER_TASK_STACK_SUPERVISOR` | 2048 | Supervisor task stack |
| `STREAMER_TASK_PRIO_SUPERVISOR` | 1 | Supervisor priority (MUST be low) |
| `STREAMER_SUPERVISOR_MIN_HEAP` | 15360 | Min free heap before reset (bytes) |
| `STREAMER_SUPERVISOR_STALL_TIMEOUT` | 15000 | Pipeline stall timeout (ms) |
| `STREAMER_SUPERVISOR_MIN_STACK` | 256 | Min free stack before reset (bytes) |
| `STREAMER_SUPERVISOR_CHECK_INTERVAL` | 2000 | Check interval (ms) |

### MxR-Malloc

Configured under the separate **MxR-Malloc** menu. Key options: max DRAM/IRAM
descriptors, descriptor table placement, integration mode (Wrap/Compat/Port),
region configuration, anti-sliver, best-fit early-exit, cross-region fallback
guards. Defaults work out of the box.

> **Important**: `CONFIG_LWIP_SO_REUSE=y` must be set in sdkconfig
> (Component config → LWIP → Enable SO_REUSEADDR option) for TCP transport to
> work across stop→start cycles.

---

## 🙏 Acknowledgments

- **Espressif** — ESP8266 RTOS SDK
- **PowerBASIC** — Windows application development
- **Z.ai & Qwen** — AI-assisted development
- **INMP441** — High-quality I2S MEMS microphone
- **Grok (xAI)** — Collaborative debugging of sleep/wake, click, and limiter issues
- **Yeh et al. (2013)** — BWSOLA packet-loss concealment algorithm

---

## 📄 License

MIT License — see [LICENSE](LICENSE) for details.

---

<div align="center">

**⭐ Star this project if you find it useful!**

Made with ❤️ and AI

</div>
