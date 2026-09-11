# ⚡ FFXIV Standalone Latency Mitigator

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform: Windows 10/11 x64](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](https://microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](#license)
[![Zero Dependencies](https://img.shields.io/badge/Dependencies-Zero-brightgreen.svg)](#-key-features)

A lightweight, zero-dependency, standalone Windows tool (`ffxiv-mitigator.exe`) that eliminates animation lock clipping in **Final Fantasy XIV** (64-bit DirectX 11 client `ffxiv_dx11.exe`).

It enables **seamless double weaving of off-Global Cooldowns (oGCDs)** on medium to high latency connections (50 ms – 300+ ms) without requiring Dalamud, XIVLauncher, kernel drivers, or network packet MITM.

---

### ✨ Key Features

- 🚀 **Zero Third-Party Dependencies**: No Dalamud plugins, XIVLauncher, Python runtimes, or kernel drivers (`WinDivert`) required. Runs entirely out of a single folder.
- 🎯 **Works Out of the Box**: Self-calibrating rolling RTT engine. No manual ping configuration or port forwarding needed.
- 🛡️ **Anti-Cheat Safe Guardrails**: Hardcoded safety floors (minimum 20–25 ms animation lock) ensure requests never drop to 0 ms or trip server-side anomaly detection.
- 📊 **Real-Time Terminal Dashboard**: Live 3-tab ANSI console displaying Action RTT gauges, jitter analysis, latency distribution histograms, and cumulative time saved.
- 🧹 **Clean In-Memory Detours**: Injects via standard Win32 APIs, hooks combat routines with MinHook, and unhooks completely with zero trace on exit (`[Q]`).

---

## Table of Contents

- [Quick Start](#quick-start)
- [Interactive Dashboard & Controls](#interactive-dashboard--controls)
- [Frequently Asked Questions (FAQ)](#frequently-asked-questions-faq)
- [Advanced Configuration](#advanced-configuration)
- [How It Works](#how-it-works)
- [Building from Source](#building-from-source)
- [License](#license)

---

<a id="quick-start"></a>
## 🚀 Quick Start

### 1. Download & Extract
Download the latest release (`ffxiv-mitigator-windows-x64.zip`) from the [Releases](../../releases) tab.
Extract the ZIP into any folder. Keep `ffxiv-mitigator.exe` and `mitigator_payload.dll` in the same directory:
```
ffxiv-mitigator/
├── ffxiv-mitigator.exe     # Standalone loader & dashboard
└── mitigator_payload.dll    # In-game detour engine
```

### 2. Launch Final Fantasy XIV
Start `ffxiv_dx11.exe` through your normal launcher and log into your character.

### 3. Run the Mitigator
Right-click `ffxiv-mitigator.exe` and select **Run as Administrator** (required for standard Win32 process injection permissions).

The console will automatically discover the game process, inject the payload, hook combat routines, and display the live dashboard:

```text
╭─────────────────────────────────────────────────────────────────────────────────────────╮
│ ⚡ FFXIV STANDALONE LATENCY MITIGATOR (C++20) — LIVE COMBAT DASHBOARD                   │
│ Target: ffxiv_dx11.exe (PID: 14088) │ Detours: 2/2 Active │ Mode: ACTIVE                │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ ▶ [1] Live Combat  │   [2] Latency Analytics  │   [3] Settings & Safety                 │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ NETWORK & LATENCY: Action RTT [████░░░░] 365.2ms [FAIR] (±6.4ms jitter) │ Target: 15ms  │
│ MITIGATION & THROUGHPUT: Mitigated [████████] 100% (42/42) │ Saved: 14.82s (Avg: 352.8ms│
│ SAFETY GUARDS & DIAGNOSTICS: APM: 48.5 │ Uptime: 00:08:24 │ Floor: 0 │ Spike: 0 │ Cast: │
├─────────────────────────────────────────────────────────────────────────────────────────┤
│ LIVE COMBAT ACTION STREAM                                                               │
│ TIME     │ #SEQ  │ ACTION   │ ANIMATION LOCK         │ SAVED     │ ACTION RTT     │ STATUS  │
│ 14:20:05 │ #0041 │ 0x1D8D   │ 600.0ms ➔  247.2ms     │ -352.8ms  │ 367ms (365ms)  │ MITIGAT │
│ 14:20:06 │ #0042 │ 0x1D8E   │ 600.0ms ➔  248.0ms     │ -352.0ms  │ 366ms (365ms)  │ MITIGAT │
╰─────────────────────────────────────────────────────────────────────────────────────────╯
```

> [!TIP]
> **Zero Configuration Required!**
> You do **not** need to set your ping or change any settings. The mitigator automatically measures your round-trip time and trims off excess latency dynamically so your abilities feel like you are playing on ~15 ms ping.

> [!NOTE]
> **Windows Defender / SmartScreen Notice**:
> Because this tool uses standard Win32 memory injection (`VirtualAllocEx` / `CreateRemoteThread`) without an expensive commercial code-signing certificate, Windows SmartScreen or antivirus software may show an alert. You can safely click **"More info" &rarr; "Run anyway"**. The entire codebase is 100% open source and can be inspected or compiled directly from source.

---

<a id="interactive-dashboard--controls"></a>
## 🎮 Interactive Dashboard & Controls

The terminal features a 3-tab live interface that can be navigated while playing:

- **Tab 1: Live Combat (`[1]`)**: Real-time Action RTT gauge, connection quality assessment, rolling action feed, and cumulative time saved.
- **Tab 2: Latency Analytics (`[2]`)**: Real-time ASCII RTT waveform (sparkline) and latency distribution histogram bins.
- **Tab 3: Settings & Safety (`[3]`)**: Interactive configuration overview showing active settings and hotkey legend.

### Hotkey Reference

| Key | Action | Description |
|---|---|---|
| **Tab Navigation** | | |
| `[1]` / `[2]` / `[3]` | **Switch Tab** | Jump directly to Tab 1 (Combat), Tab 2 (Analytics), or Tab 3 (Settings). |
| `[Tab]` / `[Shift+Tab]` | **Cycle Tabs** | Cycle through dashboard tabs (Arrow keys `[←]` / `[→]` also work). |
| **Real-Time Tuning** | | |
| `[P]` / `[Shift+P]` | **Target Ping ±5 ms** | Decrease / Increase simulated target baseline (range: 5 ms – 100 ms). |
| `[F]` / `[Shift+F]` | **Lock Floor ±5 ms** | Decrease / Increase minimum animation lock floor (range: 10 ms – 100 ms). |
| `[M]` / `[Shift+M]` | **Safety Margin ±1 ms** | Decrease / Increase jitter safety margin (range: 0 ms – 20 ms). |
| **General Controls** | | |
| `[S]` | **Save Configuration** | Saves all current settings directly to `mitigator_config.json`. |
| `[D]` | **Toggle Dry-Run** | Switch between active mitigation and telemetry-only monitoring without restarting. |
| `[L]` | **Toggle Verbose** | Show or hide non-mitigated actions in the live action feed. |
| `[C]` | **Clear Statistics** | Reset session counters (actions mitigated, total time saved). |
| `[Q]` | **Clean Exit & Unhook** | Safely unhooks all detours, unloads the DLL, and exits. The game continues running normally. |

---

<a id="frequently-asked-questions-faq"></a>
## ❓ Frequently Asked Questions (FAQ)

<details>
<summary><b>Why does the dashboard show ~380 ms RTT when my ping test says ~220 ms?</b></summary>
<br>

This is completely normal and expected!
- **Network Ping (ICMP / TCP)**: Measures only the raw transit time of a network packet over physical cables between your PC and Square Enix's servers.
- **Action RTT (Round-Trip Time)**: Measures the full elapsed time from the moment your client presses an ability (`UseActionLocation`) until the FFXIV game server evaluates combat logic, checks buffs/cooldowns, calculates RNG, and dispatches the effect back to your client (`ReceiveActionEffect`). Server-side combat processing typically adds **+100 ms to +150 ms** on top of your physical ping.

The mitigator tracks this true Action RTT and automatically trims off the excess delay so double weaving feels crisp and responsive.
</details>

<details>
<summary><b>Do I need to change Target Ping if I have 200 ms latency?</b></summary>
<br>

**No!** The default `target_ping_ms: 15.0` is already optimal.
- `target_ping_ms` does **not** represent your current internet speed. It represents the **ideal low-ping target** you want the game to mimic.
- Leaving it at `15.0 ms` tells the mitigator to subtract all latency above 15 ms, effectively giving you the responsiveness of a player living right next to the data center.
</details>

<details>
<summary><b>Is this safe to use? Will I get banned?</b></summary>
<br>

The mitigator is designed with conservative anti-cheat guardrails:
1. **Hard Floor Clamping**: Animation locks can never drop below `min_animation_lock_ms` (default 25.0 ms), matching standard low-ping client behavior and avoiding server frequency anomaly flags.
2. **Client-Side Only**: It does not modify network packets sent to the server or alter global cooldowns (GCDs). It only adjusts the client's local animation lock timer after the server confirms an action.
3. **Preserves Cast Locks**: Spell casting locks and slide-cast states are strictly preserved to maintain animation synchronization.
</details>

<details>
<summary><b>Why does Windows Defender or SmartScreen flag the executable?</b></summary>
<br>

`ffxiv-mitigator.exe` uses standard Windows debugging and memory injection APIs (`VirtualAllocEx` and `CreateRemoteThread`) to load `mitigator_payload.dll` into `ffxiv_dx11.exe`. Antivirus heuristics flag unknown binaries using these APIs unless they are signed with an expensive commercial Extended Validation (EV) certificate. You can safely add an exclusion or allow it through SmartScreen.
</details>

<details>
<summary><b>How do I cleanly close or stop using it?</b></summary>
<br>

Simply press **`[Q]`** in the console. The loader sends an IPC command to the payload, which disables all MinHook detours, calls `FreeLibraryAndExitThread` to unload itself from game memory, and closes the console. Your game continues running completely unaffected.
</details>

<details>
<summary><b>What happens when Final Fantasy XIV releases a patch?</b></summary>
<br>

When a major patch is released, memory offsets and function signatures in `ffxiv_dx11.exe` may change. If the mitigator fails to find the signatures after a game update, check the [Releases](../../releases) page for an updated binary. (Developers can update `include/mitigator/game_definitions.hpp` by following the 2-minute guide in [AGENTS.md](AGENTS.md)).
</details>

---

<a id="advanced-configuration"></a>
## ⚙️ Advanced Configuration

`ffxiv-mitigator` works out of the box with zero configuration. For advanced users, settings can be customized through three layers with the following precedence:

> **Precedence**: **CLI Flags** &nbsp;&gt;&nbsp; **`mitigator_config.json`** &nbsp;&gt;&nbsp; **Built-in Defaults**

### Configuration File (`mitigator_config.json`)

On startup, `ffxiv-mitigator.exe` automatically looks for `mitigator_config.json` next to the executable.

> [!TIP]
> You do **not** need to write this file manually! Adjust your desired settings in real time using the live hotkeys and press **`[S]`** to automatically save your active configuration.

Example `mitigator_config.json`:
```json
{
  "target_ping_ms": 15.0,
  "min_animation_lock_ms": 25.0,
  "max_animation_lock_ms": 2500.0,
  "rtt_sample_window": 10,
  "safety_margin_ms": 0.0,
  "dry_run": false,
  "verbose": false
}
```

### Parameter Reference

| Setting | JSON Key | CLI Flag | Default | Allowed Range | Description |
|---|---|---|---|---|---|
| **Target Ping** | `"target_ping_ms"` | `--target-ping <ms>` | `15.0` ms | `0.0` – `250.0` ms | Simulated low-latency baseline (what ping animation locks mimic). |
| **Min Lock Floor** | `"min_animation_lock_ms"` | `--min-lock <ms>` | `25.0` ms | `20.0` – `150.0` ms | Hard floor safety guardrail preventing locks from dropping to 0 ms. |
| **Max Lock Ceiling** | `"max_animation_lock_ms"` | — | `2500.0` ms | `500.0` – `5000.0` ms | Prevents corrupt or malformed server packets from causing runaway locks. |
| **Safety Margin** | `"safety_margin_ms"` | — | `0.0` ms | `0.0` – `100.0` ms | Conservative buffer subtracted from mitigation to absorb heavy packet jitter. |
| **Sample Window** | `"rtt_sample_window"` | — | `10` | `3` – `100` | Size of sliding window used for moving-median spike rejection. |
| **Dry-Run Mode** | `"dry_run"` | `--dry-run` | `false` | `true` / `false` | Calculates mitigations without modifying game memory (telemetry only). |
| **Verbose Logging** | `"verbose"` | `--verbose` | `false` | `true` / `false` | Logs all actions (including non-mitigated GCDs) in the combat stream. |

### CLI Options

CLI flags override both `mitigator_config.json` and default values:

```
Usage: ffxiv-mitigator.exe [options]

Options:
  --target-ping <ms>    Simulated low-ping target RTT in ms (default: 15.0)
  --min-lock <ms>       Hard floor animation lock in ms (default: 25.0)
  --dry-run             Monitor and display calculations without modifying game memory
  --verbose             Log all actions including non-mitigated ones
  --watch               Auto-recover and re-inject if game process restarts
  --help, -h            Show this help text
```

---

<a id="how-it-works"></a>
## 🔬 How It Works

### The Animation Lock Problem

In Final Fantasy XIV:
1. When you press an ability, your client dispatches an action request to the server.
2. The server processes the action and returns an `ActionEffect` packet with an **animation lock duration** (typically 500 ms – 600 ms).
3. The unmodded client applies the full animation lock duration **at the exact moment the server packet is received**.
4. Higher network ping shifts the animation lock expiration further into the future:
   - At **10 ms ping**: animation lock ends ~610 ms after pressing the button.
   - At **160 ms ping**: animation lock ends ~760 ms after pressing the button.
5. This extra 150 ms delay prevents weaving two off-Global Cooldowns (oGCDs) between GCDs without clipping, lowering overall DPS.

`ffxiv-mitigator` calculates the exact round-trip transit time and adjusts the client's local animation lock timer to eliminate this artificial delay.

<details>
<summary><b>Architecture & In-Memory Detours (Click to expand)</b></summary>
<br>

```
[ ffxiv-mitigator.exe (Loader & Telemetry Console) ]
  ├── 1. Discovers ffxiv_dx11.exe via Toolhelp32 snapshot
  ├── 2. Locates adjacent mitigator_payload.dll in the same directory
  ├── 3. Injects payload via Win32 VirtualAllocEx + CreateRemoteThread
  ├── 4. Establishes Windows Named Pipe (\\.\pipe\ffxiv_mitigator_ipc)
  └── 5. Displays Live Telemetry Dashboard & handles hotkeys [1-3], [P], [F], [M], [S], [D], [L], [C], [Q]
          │
          ▼ IPC Stream
[ ffxiv_dx11.exe (Game Process) ]
  └── [ mitigator_payload.dll (In-Game Detour Engine) ]
        ├── AOB Signature Scanner (dynamically locates game routines)
        ├── MinHook Detours:
        │     ├── UseActionLocation (captures ActionManager*, tracks action dispatch & cast initiation)
        │     ├── ReceiveActionEffect (calculates elapsed RTT, preserves cast locks, rewrites animationLock)
        │     └── ActionManager->animation_lock adjustment
        └── Adaptive Latency Math (simulates ~10-20ms ping with anti-cheat safety floors)
```

</details>

<details>
<summary><b>Adaptive Ping Smoothing Formulas (Click to expand)</b></summary>
<br>

When a server action effect arrives:
1. **Measured Action RTT**:
   $$\text{RTT} = t_{\text{effect}} - t_{\text{request}}$$
2. **Smoothing & Spike Filter**:
   Update exponential moving average (EMA) and sliding-window median. If a spike exceeds 2.5× the median, it is rejected from the smoothed estimate to prevent over-mitigation.
3. **Mitigation Delay**:
   $$\Delta = \max(0,\ \text{RTT} - \tau_{\text{target}} - \text{margin})$$
4. **Adjusted Animation Lock**:
   $$L_{\text{adjusted}} = \max\Big(L_{\text{original}} - \Delta,\ L_{\text{floor}}\Big)$$
   where $L_{\text{floor}}$ defaults to `25.0 ms` to enforce an absolute anti-cheat safety floor.

</details>

---

<a id="building-from-source"></a>
## 🛠️ Building from Source

### Prerequisites
- **Windows 10 / 11 (64-bit)**
- **Visual Studio 2022** (Desktop development with C++, C++20 MSVC v143 toolset)
- **CMake 3.20+**

### Compile Commands

```cmd
:: 1. Generate build files for Visual Studio 2022 x64
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

:: 2. Compile standalone executable and payload DLL
cmake --build build --config Release

:: 3. Run unit tests
ctest --test-dir build -C Release --output-on-failure
```

The compiled binaries will be output side-by-side to:
```
build\bin\Release\ffxiv-mitigator.exe
build\bin\Release\mitigator_payload.dll
```

### Cross-Platform Unit Testing (macOS / Linux / Windows)

The core calculation engine, rolling RTT tracker, sequence tracker, and IPC binary protocols are **100% platform-independent C++20** and can be compiled and tested on macOS, Linux, or Windows without Windows SDK headers:

```bash
make test
```

For codebase architecture, memory layouts, and patch day update workflows, see [AGENTS.md](AGENTS.md).

---

<a id="license"></a>
## 📄 License

This project is licensed under the MIT License.
Included third-party dependencies:
- [MinHook](src/third_party/minhook/README.md) is licensed under the 2-Clause BSD License.

