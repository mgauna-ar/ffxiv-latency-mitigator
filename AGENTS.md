# Agent Guidelines & Architecture Reference

This document outlines the architectural patterns, engineering principles, memory layouts, and verification workflows for autonomous AI agents and developers working on `ffxiv-latency-mitigator`.

---

## 🏛️ Engineering Principles & Guidelines

1. **Strict Separation of Concerns (No God Files)**:
   - Every file must have a single, clearly defined responsibility.
   - Keep translation units focused, modular, and under reasonable line counts.
   - Never combine loader orchestration, IPC networking, and game memory hooks into a single monolithic unit.

2. **Clean Interfaces & Portability**:
   - The algorithmic core (`src/core/`) and IPC serialization protocol (`ipc_protocol.hpp`) must remain **100% platform-independent C++20**, free of `<windows.h>` dependencies.
   - This ensures the entire core calculation and protocol engine can be compiled and unit tested on any development host (macOS, Linux, or Windows) without emulator overhead.

3. **Memory Safety & Modern C++ Standards**:
   - Standard: C++20 (`-std=c++20` / `/std:c++20`).
   - Use RAII for all resource lifecycles (handles, synchronization primitives, file descriptors).
   - Use `std::span` and typed structs for binary buffer parsing rather than raw unbounded pointer arithmetic.
   - No undefined behavior or calling-convention mismatches (e.g., ensure `CreateThread` callback signatures strictly match `DWORD WINAPI (*)(LPVOID)`).

4. **Zero-Dependency Runtime Constraint**:
   - The project must produce a single, self-contained executable (`ffxiv-mitigator.exe`) that runs on Windows 10/11 without requiring:
     - External plugin injectors (Dalamud, XIVLauncher).
     - Kernel filter drivers (WinDivert).
     - Microsoft Visual C++ Redistributable packages (statically linked `/MT` runtime).
   - In-game payload DLL (`mitigator_payload.dll`) is embedded into the loader executable as a `constexpr` byte array during build via `tools/embed_dll.py`.

---

## 📂 Component Responsibility Matrix

| Component | Header / Source | Primary Responsibility |
|---|---|---|
| **Core Types** | `include/mitigator/types.hpp` | Common domain types, `MitigationConfig`, `MitigationResult`, action IDs |
| **Rolling RTT** | `include/mitigator/rolling_rtt.hpp`<br>`src/core/rolling_rtt.cpp` | EMA RTT tracker, sliding-window median spike filter, jitter estimation |
| **Sequence Tracker** | `include/mitigator/sequence_tracker.hpp`<br>`src/core/sequence_tracker.cpp` | Correlates action dispatches with server responses via sequence IDs or FIFO fallback with TTL pruning |
| **Cast Tracker** | `include/mitigator/cast_tracker.hpp`<br>`src/core/cast_tracker.cpp` | Tracks active spell casting states to preserve cast-lock durations (slide-casting) |
| **Animation Lock** | `include/mitigator/animation_lock.hpp`<br>`src/core/animation_lock.cpp` | Core mitigation formula, anti-cheat safety floors, ceiling clamping, dry-run mode |
| **IPC Protocol** | `include/mitigator/ipc_protocol.hpp`<br>`src/core/ipc_protocol.cpp` | Fixed-size binary packet framing, serialization, and deserialization |
| **Signature Scanner** | `include/mitigator/sigscan.hpp`<br>`src/payload/sigscan.cpp` | IDA-style AOB pattern scanning, PE section matching, and RIP-relative address resolution |
| **Game Hooks** | `src/payload/game_hooks.hpp`<br>`src/payload/game_hooks.cpp` | MinHook detours for `UseActionLocation`, `ReceiveActionEffect`, `CastBegin`, `CastInterrupt` |
| **Payload IPC Client** | `src/payload/payload_ipc.hpp`<br>`src/payload/payload_ipc.cpp` | In-game Named Pipe client thread streaming telemetry to the loader |
| **Payload Entry** | `src/payload/dllmain.cpp` | Injected DLL lifecycle, background orchestration, and clean unhooking (`FreeLibraryAndExitThread`) |
| **Process Finder** | `src/loader/process_finder.hpp`<br>`src/loader/process_finder.cpp` | Win32 Toolhelp32 process snapshot scanning and 64-bit architecture validation |
| **DLL Injector** | `src/loader/injector.hpp`<br>`src/loader/injector.cpp` | Writes embedded DLL to `%TEMP%` and executes `CreateRemoteThread` + `LoadLibraryW` with cleanup retry |
| **Loader IPC Server** | `src/loader/loader_ipc.hpp`<br>`src/loader/loader_ipc.cpp` | Named Pipe server, thread-safe command dispatch, and graceful wake-up on shutdown |
| **UI Renderer** | `src/loader/ui_renderer.hpp`<br>`src/loader/ui_renderer.cpp` | Formatted ANSI console UI, rolling telemetry logs, and summary statistics |
| **Loader Entry** | `src/loader/main.cpp` | CLI parsing, signal handling, and interactive non-blocking hotkey event loop |

---

## 🔒 Critical Invariants & Edge Cases

When modifying detours, hooks, or timing math, the following invariants **must** be preserved:

1. **Zone-Wide Action Effect Isolation**:
   - In FFXIV, `ReceiveActionEffect` is invoked for **every** action in the zone (party members, enemies, ground effects, damage-over-time ticks).
   - Only actions that actually change the local player's animation lock (`old_lock != new_lock && new_lock > 0.0f`) should be processed and mitigate animation lock.

2. **Failed Action Request Suppression**:
   - If `UseActionLocation` returns `0` (action rejected client-side due to cooldown, range, or status), **no packet is dispatched to the server**.
   - Do not register pending requests on failure; doing so creates ghost entries that desynchronize RTT tracking on subsequent actions.

3. **Cast Lock & Slide-Cast Preservation**:
   - If `CastTracker::is_casting` is active when an effect arrives, do **not** reduce animation lock. Cast locks represent caster tax and slide-cast timing; reducing them causes cast animation clipping and server-side desynchronization.

4. **Hard Clamping Floors (Anti-Cheat Guardrail)**:
   - Never allow `adjusted_lock` to drop below `min_animation_lock_ms` (default 25.0ms).
   - Zero or near-zero animation locks can trigger server-side action frequency anomaly detection.

5. **Thread-Safe Pipe Teardown**:
   - Always lock `m_send_mutex` when closing pipe handles.
   - When stopping `LoaderIpcServer`, issue a dummy connect (`CreateFileA`) to unblock any pending `ConnectNamedPipe` calls before joining worker threads.

---

## 🎮 Game Structures & Memory Layouts

Located in [`include/mitigator/game_structures.hpp`](include/mitigator/game_structures.hpp):

- **`ActionManager` Offsets (FFXIV dx11 x64)**:
  - `0x08`: `float animation_lock` (active lock timer in seconds)
  - `0x28`: `bool is_casting` (active cast state)
  - `0x30`: `float elapsed_cast_time`
  - `0x34`: `float cast_time`
  - `0x60`: `float remaining_combo_time`
  - `0x68`: `bool is_queued`
  - `0x120`: `uint16_t current_sequence` (rolling action sequence ID)

---

## 🧪 Verification & Test Commands

### Running Unit Tests (Any Host OS: macOS / Linux / Windows)
```bash
make test
```
Or directly with Clang:
```bash
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Isrc -Itests \
  src/core/*.cpp src/payload/sigscan.cpp tests/*.cpp \
  -o test_runner && ./test_runner
```

### Windows MSVC Build
```cmd
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
