# CLAUDE.md — eden-xbox (repo pointer)

> Slim by design. Coordination lives OUTSIDE this repo so it does not duplicate across every agent's
> clone. This file points there and records the canonical layout, build/run, and architecture map.

## Coordination (shared, NOT in this repo)
All planning and roster docs live in the shared folder one level up from the clones:
```
C:\Users\juanr\Desktop\Claude\Projects\Eden-Xbox\Agent Coordination Board\
  TEAM_GUIDE.md     # stack, build/test/deploy, roster, full laws, house rules
  AGENT-BOARD.md    # live board: LAWS · Roll call · Active claims · Log
  ROADMAP.md        # phased plan with gates
  Onboarding/       # README (roster + first-run prompts) + per-role briefs
```
Read those before changing anything here. Claim work on the board, not in GitHub issues.

## What this repo is
A fork of **Eden** (Nintendo Switch emulator, C++20, GPLv3) being ported to **UWP/WinRT for Xbox
Series X|S Dev Mode**, target renderer **Direct3D 12**. Upstream: `git.eden-emu.dev/eden-emu/eden`.

## Canonical workspace layout
```
C:\Users\juanr\Desktop\Claude\Projects\Eden-Xbox\
  Agent Coordination Board/     # shared, not cloned
  Repo/eden-xbox/               # LEAD's canonical clone (this CLAUDE.md)
  Agents/<ROLE>/eden-xbox/      # each specialist's own clone (git -C-pinned)
  screenshots/<ROLE>/           # per-agent proof output
```

## Build / test / run
- `development` is the working branch; `main` is protected. LEAD is the only merger.
- Desktop sanity build (Phase 0):
  `cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release`
- UWP target build + appx packaging: added by AGENT DEVOPS in Phase 2 (toolchain file + manifest).
- Deploy: Xbox Device Portal at `https://<xbox-ip>:11443` → add appx → set app to **Game** mode.
- _LEAD updates this section with exact, verified commands once the UWP toolchain is proven._

## Architecture map (where the porting work lands)
- `src/video_core/` — renderer. **The D3D12 work lives here** (Phase 1/3). Vulkan/GL only upstream.
- `src/dynarmic/` (+ Xbyak) — ARM64→x64 JIT. **W^X adaptation lives here** (Phase 2).
- `src/core/` — HLE, memory, fastmem. **Game-mode memory + fastmem rework** (Phase 2/4).
- platform/frontend layer — **UWP2Win32 shims + AppContainer file access** (Phase 2).
