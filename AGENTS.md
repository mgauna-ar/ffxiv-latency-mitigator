# Agent Guidelines & Architecture Reference

This document outlines the architectural patterns, engineering principles, memory layouts, and verification workflows for autonomous AI agents and developers working on `ffxiv-latency-mitigator`.

---

## 🏛️ Engineering Principles & Guidelines

1. **Strict Separation of Concerns (No God Files)**:
   - Every file must have a single, clearly defined responsibility.
   - Keep translation units focused, modular, and under reasonable line counts.
   - Never combine loader orchestration, IPC networking, and game memory hooks into a single monolithic unit.

2. **Clean Interfaces & Portability**:
   - The algorithmic core (`src/core/`), IPC serialization protocol (`ipc_protocol.hpp`), configuration manager (`config_manager.hpp`), and console renderer (`ui_renderer.hpp`) must remain **100% platform-independent C++20**, free of `<windows.h>` dependencies.
   - This ensures the entire core calculation, configuration engine, and test runner can be compiled and unit tested on any development host (macOS, Linux, or Windows) without emulator overhead.

3. **Memory Safety & Modern C++ Standards**:
   - Standard: C++20 (`-std=c++20` / `/std:c++20`).
   - Use RAII for all resource lifecycles (handles, synchronization primitives, file descriptors).
   - Use `std::span` and typed structs for binary buffer parsing rather than raw unbounded pointer arithmetic.
   - No undefined behavior or calling-convention mismatches (e.g., ensure `CreateThread` callback signatures strictly match `DWORD WINAPI (*)(LPVOID)`).

4. **Zero-Dependency Runtime Constraint**:
   - The project produces a standalone executable (`ffxiv-mitigator.exe`) accompanied by its in-game payload DLL (`mitigator_payload.dll`) placed in the same directory, running on Windows 10/11 without requiring:
     - External plugin injectors (Dalamud, XIVLauncher).
     - Kernel filter drivers (WinDivert).
     - Microsoft Visual C++ Redistributable packages (statically linked `/MT` runtime).

5. **Mandatory Documentation Synchronization (`README.md` & `AGENTS.md`)**:
   - Whenever code is modified, the author or agent **must verify and update both `README.md` and `AGENTS.md`** if the change impacts:
     - Component architecture, translation units, or headers (`Component Responsibility Matrix`).
     - User-facing features, hotkeys, dashboard tabs, or CLI flags (`README.md`).
     - Configuration parameters, defaults, or clamping ranges (`MitigationConfig`).
     - Game memory structures, offsets, or signatures (`Game Structures & Memory Layouts`).
     - Build commands, dependencies, or test instructions.
   - **Never leave documentation out of sync with code**. Both files must co-evolve with every pull request and agent task.

---

## 📂 Component Responsibility Matrix

| Component | Header / Source | Primary Responsibility |
|---|---|---|
| **Game Definitions** | `include/mitigator/game_definitions.hpp` | Centralized FFXIV AOB signatures, instruction offsets, memory offsets, and process target (Dawntrail 7.x) |
| **Game Structures** | `include/mitigator/game_structures.hpp` | Memory layout definitions for `ActionManager`, `ActionEffectHeader`, and compile-time `static_assert` layout verification |
| **Core Types** | `include/mitigator/types.hpp` | Common domain types, `MitigationConfig`, `MitigationResult`, action IDs |
| **Config Manager** | `include/mitigator/config_manager.hpp`<br>`src/core/config_manager.cpp` | JSON configuration serialization/deserialization, anti-cheat clamping validation, disk persistence (`mitigator_config.json`), and default fallbacks |
| **Rolling RTT** | `include/mitigator/rolling_rtt.hpp`<br>`src/core/rolling_rtt.cpp` | EMA RTT tracker, sliding-window median spike filter, jitter estimation |
| **Sequence Tracker** | `include/mitigator/sequence_tracker.hpp`<br>`src/core/sequence_tracker.cpp` | Correlates action dispatches with server responses via sequence IDs or FIFO fallback with TTL pruning |
| **Cast Tracker** | `include/mitigator/cast_tracker.hpp`<br>`src/core/cast_tracker.cpp` | Tracks active spell casting states to preserve cast-lock durations (slide-casting) |
| **Animation Lock** | `include/mitigator/animation_lock.hpp`<br>`src/core/animation_lock.cpp` | Core mitigation formula, anti-cheat safety floors, ceiling clamping, dry-run mode |
| **IPC Protocol** | `include/mitigator/ipc_protocol.hpp`<br>`src/core/ipc_protocol.cpp` | Fixed-size binary packet framing, serialization, and deserialization |
| **Signature Scanner** | `include/mitigator/sigscan.hpp`<br>`src/core/sigscan.cpp` | IDA-style AOB pattern scanning, PE section matching, and RIP-relative address resolution |
| **PE Scanner** | `src/payload/pe_scanner.hpp`<br>`src/payload/pe_scanner.cpp` | SEH-protected Win32 PE section scanning (`scan_module_section`) for module scanning inside the game process |
| **Game Hooks** | `src/payload/game_hooks.hpp`<br>`src/payload/game_hooks.cpp` | MinHook detours for `UseActionLocation` (action dispatch & cast tracking) and `ReceiveActionEffect` (effect mitigation) |
| **MinHook Library** | `src/third_party/minhook/` | Lightweight x86/x64 in-memory detour hooking library embedded statically into the payload DLL |
| **Payload IPC Client** | `src/payload/payload_ipc.hpp`<br>`src/payload/payload_ipc.cpp` | In-game Named Pipe client thread streaming telemetry to the loader |
| **Payload Entry** | `src/payload/dllmain.cpp` | Injected DLL lifecycle, background orchestration, and clean unhooking (`FreeLibraryAndExitThread`) |
| **Process Finder** | `src/loader/process_finder.hpp`<br>`src/loader/process_finder.cpp` | Win32 Toolhelp32 process snapshot scanning and 64-bit architecture validation |
| **DLL Injector** | `src/loader/injector.hpp`<br>`src/loader/injector.cpp` | Injects adjacent payload DLL into the game process via `CreateRemoteThread` + `LoadLibraryW` |
| **Loader IPC Server** | `src/loader/loader_ipc.hpp`<br>`src/loader/loader_ipc.cpp` | Named Pipe server, thread-safe command dispatch, and graceful wake-up on shutdown |
| **UI Renderer** | `src/loader/ui_renderer.hpp`<br>`src/loader/ui_renderer.cpp` | Formatted ANSI console UI, 3-tab live dashboard, rolling action feed, and statistics |
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

5. **Moving-Median Spike Filtering**:
   - Outlier action RTT samples exceeding $2.5\times$ the moving median must be rejected from the smoothed EMA calculation to prevent transient packet spikes from causing erratic over-mitigation.

6. **Cold-Start Early Action Queue Guard**:
   - The initial actions in a session may reflect queue delay before a stable RTT baseline is established. The cold-start guard prevents premature floor clamping on early actions.

7. **Corrupt Packet Ceiling Clamping**:
   - Server-assigned animation locks must be clamped to `max_animation_lock_ms` (default 2500.0ms) to prevent malformed or corrupt packets from locking the player indefinitely.

8. **Thread-Safe Pipe Teardown**:
   - Always lock `m_send_mutex` when closing pipe handles.
   - When stopping `LoaderIpcServer`, issue a dummy connect (`CreateFileA`) to unblock any pending `ConnectNamedPipe` calls before joining worker threads.

---

## 🎮 Game Structures & Memory Layouts

Located in [`include/mitigator/game_structures.hpp`](include/mitigator/game_structures.hpp):

### `ActionManager` Offsets (FFXIV dx11 x64 Dawntrail 7.x)
- `0x08`: `float animation_lock` (active lock timer in seconds)
- `0x28`: `bool is_casting` (active cast state)
- `0x30`: `float elapsed_cast_time`
- `0x34`: `float cast_time`
- `0x60`: `float remaining_combo_time`
- `0x68`: `bool is_queued`
- `0x120`: `uint16_t current_sequence` (rolling action sequence ID)

### `ActionEffectHeader` Packet Layout (0x28 bytes)
- `0x00`: `uint64_t animation_target_id` (GameObjectId of primary target)
- `0x08`: `uint32_t action_id` (Action ID)
- `0x0C`: `uint32_t global_sequence` (Unique server sequence ID)
- `0x10`: `float animation_lock` (Server-assigned animation lock in seconds)
- `0x14`: `uint32_t ballista_entity_id` (Artillery / cannon entity ID)
- `0x18`: `uint16_t source_sequence` (Client-initiated sequence counter)
- `0x1A`: `uint16_t rotation` (Quantized rotation)
- `0x1C`: `uint16_t spell_id` (Spell ID)
- `0x1E`: `uint8_t animation_variation` (Animation variation)
- `0x1F`: `uint8_t action_type` (Action type)
- `0x20`: `uint8_t flags` (Bit 0: ShowInLog, Bit 1: ForceAnimationLock)
- `0x21`: `uint8_t num_targets` (Number of targets affected)

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
  src/core/*.cpp src/loader/ui_renderer.cpp tests/*.cpp \
  -o test_runner && ./test_runner
```

### Windows MSVC Build
```cmd
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

---

## 🔄 Patch Day Update Guide (Game Updates)

When Square Enix publishes an update to Final Fantasy XIV (`ffxiv_dx11.exe`), executable code and memory structures may shift. All game-specific signatures, displacements, and offsets are isolated in [`include/mitigator/game_definitions.hpp`](include/mitigator/game_definitions.hpp).

### 2-Minute Update Procedure:

1. **Check Community Mappings (or Open Disassembler)**:
   - Check [FFXIVClientStructs](https://github.com/aers/FFXIVClientStructs) for updated Dawntrail member offsets and signatures:
     - `Client::Game::ActionManager::UseActionLocation`
     - `Client::Game::ActionEffectHandler::ReceiveActionEffect`
     - `Client::Game::ActionManager::Instance`
   - Alternatively, open `ffxiv_dx11.exe` in IDA Pro, Ghidra, or x64dbg.

2. **Understand the Two-Tier Signature System**:
   - **PRIMARY Signatures**: Call-site patterns starting with `E8 ? ? ? ?`. The hook engine automatically extracts the 32-bit relative displacement via `memory::resolve_call_relative` to locate the target function.
   - **FALLBACK Signatures**: Direct function prologue opcodes (e.g. `48 89 5C 24 08 ...`).
   - If a call-site pattern breaks due to compiler instruction reordering, provide a verified unique function prologue as fallback.

3. **Update [`include/mitigator/game_definitions.hpp`](include/mitigator/game_definitions.hpp)**:
   - Update `game::signatures::...` if opcodes changed.
   - Update `game::offsets::...` if `ActionManager` members shifted.
   - Update `ACTION_MGR_RIP_DISP_OFFSET` if the displacement position in the static instance resolution instruction changed.
   - Update `SUPPORTED_GAME_VERSION` string.

4. **Synchronize Layout in [`include/mitigator/game_structures.hpp`](include/mitigator/game_structures.hpp)**:
   - If any `game::offsets::...` changed, adjust the padding fields (`pad_xx[...]`) in `struct ActionManager` so that all `static_assert(offsetof(...))` compile-time layout checks pass.

5. **Synchronize Documentation**:
   - Update any changed offsets, version strings, or signatures in both [`README.md`](README.md) and [`AGENTS.md`](AGENTS.md) per the Mandatory Documentation Synchronization rule.

6. **Compile & Verify**:
   - Run unit tests to verify signature parsing and layout asserts:
     ```bash
     make test
     ```
   - Build release binaries:
     ```cmd
     cmake --build build --config Release
     ```
   - **Live In-Game Verification**: Launch FFXIV and run the loader in dry-run mode to verify detours attach without modifying game memory:
     ```cmd
     ffxiv-mitigator.exe --dry-run
     ```
     Confirm the dashboard reports `Detours: 2/2 Active` and telemetry packets stream during combat.

