# FFXIV Standalone Latency Mitigator

A lightweight, zero-dependency, standalone Windows executable (`ffxiv-mitigator.exe`) that eliminates action animation lock clipping for **Final Fantasy XIV** (64-bit DirectX 11 client `ffxiv_dx11.exe`).

It enables seamless double weaving of off-Global Cooldowns (oGCDs) on medium to high latency connections without requiring Dalamud, XIVLauncher, kernel drivers, or network packet MITM.

---

## The Problem: Latency Clipping in FFXIV

In Final Fantasy XIV:
1. When a player presses an ability, the client dispatches an action request to the server.
2. The server processes the action and replies with an `ActionEffect` packet containing an **animation lock duration** (typically 500ms to 600ms).
3. In the unmodded game, the client applies the full animation lock duration **at the exact moment the server packet is received**.
4. Consequently, higher network Round-Trip Time (RTT) shifts the animation lock expiration further into the future:
   $$\text{Total Lock Out} = \text{RTT} + \text{AnimationLock}_{\text{original}}$$
   - At **10 ms ping**: the player is locked for $10\text{ms} + 600\text{ms} = 610\text{ms}$.
   - At **160 ms ping**: the player is locked for $160\text{ms} + 600\text{ms} = 760\text{ms}$.
5. This extra 150ms delay makes double weaving oGCDs impossible without clipping into the Global Cooldown (GCD), severely penalizing higher ping players.

### Understanding Action RTT vs. Network Ping

When checking latency via an ICMP ping command or router test, you may see e.g. **200 ms – 250 ms**, yet `ffxiv-mitigator`'s dashboard will report an **Action RTT of ~350 ms – 400 ms**. This is completely normal and expected:
- **Network Ping (ICMP / TCP)**: Measures only raw network packet transit across physical cables.
- **Action RTT (Round-Trip Time)**: Measures the full elapsed time from the client dispatching `UseActionLocation` until the FFXIV server processes combat calculations, evaluates buffs/cooldowns, and dispatches the `ReceiveActionEffect` response back to your client. Server-side combat turnaround typically adds **+100 ms to +150 ms** on top of physical network ping.
- The mitigation engine automatically tracks this total round-trip time and trims off the excess latency so abilities trigger cleanly as if you were playing on ~15 ms ping.

---

## How It Works: In-Memory Detours with Adaptive Ping Smoothing

This project implements an **In-Memory Detour Architecture** packaged inside a single standalone executable:

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

### Adaptive Ping Smoothing Formula

When a server action effect arrives:
1. Calculate measured RTT: $\text{RTT} = t_{\text{effect}} - t_{\text{request}}$
2. Update rolling exponential moving average (EMA) and moving median.
3. Compute mitigation delay:
   $$\Delta = \max(0,\ \text{RTT} - \tau_{\text{target}} - \text{margin})$$
4. Calculate adjusted animation lock:
   $$L_{\text{adjusted}} = \max\Big(L_{\text{original}} - \Delta,\ L_{\text{floor}}\Big)$$
5. **Anti-Cheat Guardrail**: $L_{\text{floor}}$ enforces a hard floor (default 25ms - 40ms) to prevent setting animation lock to 0ms or negative values, protecting against server-side frequency anomaly detection.

---

## Usage

### Quick Start
1. **Download the latest release** (`ffxiv-mitigator-windows-x64.zip`) from the GitHub Releases tab (or [build from source](#building-from-source)).
2. Extract the ZIP into a folder of your choice (ensuring `ffxiv-mitigator.exe` and `mitigator_payload.dll` remain side-by-side).
3. Launch Final Fantasy XIV (`ffxiv_dx11.exe`).
4. Run `ffxiv-mitigator.exe` as **Administrator** (required for Win32 process injection permissions).
5. The console will detect the game, inject the adjacent payload DLL, hook the detours, and begin streaming live telemetry.

---

## Configuration & Tuning

`ffxiv-mitigator` works seamlessly out of the box with zero manual setup required. For advanced customization, settings can be configured through three methods evaluated with the following precedence:

$$\textbf{CLI Arguments} \;\;>\;\; \textbf{mitigator\_config.json} \;\;>\;\; \textbf{Built-in Defaults}$$

### Configuration Parameters & Defaults

| Setting | JSON Key | CLI Flag | Default | Allowed Range | Description |
|---|---|---|---|---|---|
| **Target Ping** | `"target_ping_ms"` | `--target-ping <ms>` | `15.0` ms | `0.0` – `250.0` ms | Simulated low-latency baseline (what ping your animation locks mimic). |
| **Min Lock Floor** | `"min_animation_lock_ms"` | `--min-lock <ms>` | `25.0` ms | `20.0` – `150.0` ms | Anti-cheat safety guardrail preventing animation lock from ever dropping to 0 or negative values. |
| **Max Lock Ceiling** | `"max_animation_lock_ms"` | — | `2500.0` ms | `500.0` – `5000.0` ms | Clamping ceiling preventing corrupt or malformed server packets from causing unbounded locks. |
| **Safety Margin** | `"safety_margin_ms"` | — | `0.0` ms | `0.0` – `100.0` ms | Conservative buffer subtracted from mitigation. Absorbs latency jitter on unstable connections. |
| **Sample Window** | `"rtt_sample_window"` | — | `10` | `3` – `100` | Size of the sliding window used for moving median spike rejection. |
| **Dry-Run Mode** | `"dry_run"` | `--dry-run` | `false` | `true` / `false` | Telemetry-only mode: calculates mitigations without modifying game memory. |
| **Verbose Logging** | `"verbose"` | `--verbose` | `false` | `true` / `false` | When true, logs all actions (including non-mitigated GCDs). When false, logs only mitigated actions. |

---

### Recommended Settings by Connection Type

#### High / Medium Latency (e.g., 200 ms – 250 ms Ping)
- **The default settings are already optimal!** You do **not** need to set `target_ping_ms` to 200 ms or 250 ms.
- `target_ping_ms` represents the **ideal low-ping baseline** you want the game to simulate (15 ms).
- The mitigator automatically measures your actual Action RTT (~350 ms – 400 ms), subtracts the 15 ms target, and eliminates the extra delay so double weaving oGCDs feels instantaneous.

#### Unstable Connections (Wi-Fi, Cellular Hotspot, Packet Jitter)
- If your ping fluctuates frequently or you experience occasional rubberbanding, add a small safety buffer:
  - Set `safety_margin_ms` to `5.0` or `10.0` ms (press `[Shift+M]` in the console or edit JSON).
  - This leaves a small safety margin to absorb sudden latency spikes without clipping.

#### Extra-Conservative Anti-Cheat Preference
- The default floor of `25.0` ms is safe and matches standard low-ping client behavior.
- If you prefer an extra safety margin, increase `min_animation_lock_ms` to `35.0` or `40.0` ms (press `[Shift+F]` in the console).

---

### Configuration File (`mitigator_config.json`)

On startup, `ffxiv-mitigator.exe` automatically looks for `mitigator_config.json` in the same directory as the executable. If found, your preferences are loaded automatically.

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

> [!TIP]
> You do **not** need to create this JSON file manually! Simply adjust settings in real time using the dashboard hotkeys and press **`[S]`** to save your active configuration to `mitigator_config.json`.

---

### Interactive Dashboard & Live Hotkeys

The terminal interface features a 3-tab live dashboard that can be operated while playing:
- **Tab 1: Overview (`[1]`)**: Real-time Action RTT gauge, connection quality assessment (`[EXCELLENT]`, `[GOOD]`, `[FAIR]`, `[POOR]`), cumulative time saved, and rolling action feed.
- **Tab 2: Analytics (`[2]`)**: Rolling Action RTT sparkline waveform and latency distribution histogram bins.
- **Tab 3: Settings (`[3]`)**: Interactive configuration overview showing active settings and hotkey legend.

| Hotkey | Action | Description |
|---|---|---|
| `[1]` / `[2]` / `[3]` | **Switch Tab** | Switch directly to Tab 1 (Overview), Tab 2 (Analytics), or Tab 3 (Settings). |
| `[Tab]` / `[Shift+Tab]` | **Cycle Tabs** | Cycle through tabs (Arrow keys `[←]`/`[→]` also work). |
| `[P]` / `[Shift+P]` | **Adjust Target Ping** | Decrease / Increase simulated target ping by 5 ms (range: 5 ms – 100 ms). |
| `[F]` / `[Shift+F]` | **Adjust Lock Floor** | Decrease / Increase min animation lock floor by 5 ms (range: 10 ms – 100 ms). |
| `[M]` / `[Shift+M]` | **Adjust Safety Margin** | Decrease / Increase mitigation safety margin by 1 ms (range: 0 ms – 20 ms). |
| `[S]` | **Save Configuration** | Saves all current in-memory settings directly to `mitigator_config.json`. |
| `[D]` | **Toggle Dry-Run** | Toggles between Live mitigation and passive telemetry monitoring without restarting. |
| `[L]` | **Toggle Verbose** | Toggles display of non-mitigated actions in the telemetry log. |
| `[C]` | **Clear Stats** | Resets cumulative session counters (actions mitigated, total time saved). |
| `[Q]` | **Clean Exit & Unhook** | Sends unhook command via IPC, disables all MinHook detours, unloads payload DLL, and exits cleanly. The game continues running without interruption. |

---

### CLI Options

CLI arguments take precedence over both `mitigator_config.json` and default values:

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

## Building from Source

### Requirements
- **Windows 10 / 11 (64-bit)**
- **Visual Studio 2022** (with C++20 MSVC v143 toolset)
- **CMake 3.20+**

### Build Commands

```cmd
:: 1. Generate build files for Visual Studio 2022 x64
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

:: 2. Compile standalone executable and payload
cmake --build build --config Release

:: 3. Run unit tests
ctest --test-dir build -C Release --output-on-failure
```

The resulting binaries will be located side-by-side at:
```
build\bin\Release\ffxiv-mitigator.exe
build\bin\Release\mitigator_payload.dll
```

### Testing Core Logic (macOS / Linux / Windows)

The core simulation math, rolling RTT tracker, sequence tracker, and IPC serialization protocols are 100% platform-independent C++20 and can be built and tested directly on any operating system:

```bash
make test
```

See [AGENTS.md](AGENTS.md) for direct compiler invocation commands and cross-platform verification details.

---

## Architecture & Development

For technical details on codebase structure, single-responsibility file boundaries, game memory offsets, and patch day update workflows, see [AGENTS.md](AGENTS.md).

---

## License

This project is licensed under the MIT License. MinHook is licensed under the 2-Clause BSD License.
