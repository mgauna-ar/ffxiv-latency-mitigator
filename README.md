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

---

## How It Works: In-Memory Detours with Adaptive Ping Smoothing

This project implements an **In-Memory Detour Architecture** packaged inside a single standalone executable:

```
[ ffxiv-mitigator.exe (Loader & Telemetry Console) ]
  ├── 1. Discovers ffxiv_dx11.exe via Toolhelp32 snapshot
  ├── 2. Locates adjacent mitigator_payload.dll in the same directory
  ├── 3. Injects payload via Win32 VirtualAllocEx + CreateRemoteThread
  ├── 4. Establishes Windows Named Pipe (\\.\pipe\ffxiv_mitigator_ipc)
  └── 5. Displays Live Telemetry Dashboard & handles hotkeys [Q], [D], [L], [C], [S]
          │
          ▼ IPC Stream
[ ffxiv_dx11.exe (Game Process) ]
  └── [ mitigator_payload.dll (In-Game Detour Engine) ]
        ├── AOB Signature Scanner (dynamically locates game routines)
        ├── MinHook Detours:
        │     ├── UseActionLocation (captures ActionManager* and records timestamps)
        │     ├── ReceiveActionEffect (calculates elapsed RTT, rewrites animationLock)
        │     ├── CastBegin & CastInterrupt (preserves hard-cast timings)
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
1. **Download the latest release binaries** (`ffxiv-mitigator.exe` and `mitigator_payload.dll`) from the GitHub Releases tab, or [build from source](#building-from-source).
2. Ensure `ffxiv-mitigator.exe` and `mitigator_payload.dll` are placed in the **same directory**.
3. Launch Final Fantasy XIV (`ffxiv_dx11.exe`).
4. Run `ffxiv-mitigator.exe` as **Administrator** (required for Win32 process injection permissions).
5. The console will detect the game, inject the adjacent payload DLL, hook the detours, and begin streaming live telemetry.

### CLI Options

```
Usage: ffxiv-mitigator.exe [options]

Options:
  --target-ping <ms>    Simulated low-ping target RTT in ms (default: 15.0)
  --min-lock <ms>       Hard floor animation lock in ms (default: 25.0)
  --dry-run             Monitor and display calculations without modifying game memory
  --verbose             Log all actions including non-mitigated ones
  --help, -h            Show this help text
```

### Interactive Hotkeys

| Hotkey | Action | Description |
|---|---|---|
| `[Q]` | **Clean Exit & Unhook** | Sends unhook command via IPC, disables all MinHook detours, calls `FreeLibraryAndExitThread`, and closes the console cleanly. The game continues running without interruption. |
| `[D]` | **Toggle Dry-Run** | Switches between Live mitigation and passive telemetry monitoring without restarting. |
| `[L]` | **Toggle Verbose** | Shows/hides non-mitigated actions in the console output. |
| `[C]` | **Clear Stats** | Resets the cumulative session counters. |
| `[S]` | **Stats Summary** | Prints current telemetry metrics (total actions, cumulative time saved, average reduction, estimated ping and jitter). |

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
