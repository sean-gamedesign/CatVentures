# Vibe Coding 2.0 — Lab Branch

Isolated sandbox for experimental MCP and Claude Code plugin integrations.
Branch: `feat/vibe-coding-lab` (do not merge to `main` without review).

## Status
- [x] Branch created
- [x] MCP servers configured
- [ ] Plugins evaluated
- [ ] Findings documented

## MCP Integrations

### VibeUE — Unreal Editor control — **KEEPER**
- [x] Plugin cloned to `Plugins/VibeUE/` (path ignored via `.gitignore`)
- [x] `BuildPlugin.bat` completed successfully (96/96 actions, ~62s, exit 0)
- [x] Enabled in `CatVentures.uproject`
- [x] Editor restarted, plugin loads cleanly (Output Log: `VibeUE Module has started`, `Exported 10 tools`)
- [x] `.mcp.json` registered at repo root with `Authorization: Bearer ${VIBEUE_API_KEY}` header
- [x] `mcp-remote` reachable
- [x] Claude Code lists the `VibeUE` server (`claude mcp list` → ✓ Connected)
- [x] Smoke test: list + deep read of `ABP_Cat_V2` succeeded (see below)
- [x] **Decision: KEEPER**

**Smoke test results (2026-04-29):**
- `manage_asset(action='list', path='/Game', asset_type='Blueprint')` returned 22 Blueprints — matches project layout (`PrimeCatBase`, `BP_CatGameState`, `BP_CatGameMode`, `BP_Destructible_Base`, `ABP_Cat_V2`, etc.)
- `execute_python_code` deep-read of `ABP_Cat_V2`: **31 animation graphs**, 1 state machine. The locomotion/jump structure is fully visible — `Jump_Launch`, `Jump_Apex`, `Jump_Fall`, `Jump_Land` state graphs match the `ECatJumpPhase` enum chain in `CatBase.h`; `A_Cat_Idle_Base`, `Move`, `Turn` cover the ground locomotion blendspace tree
- Read latency on tool calls: ~13–18 ms

**Notes:**
- Server auto-starts at `http://127.0.0.1:8088/mcp` while UE Editor is open. Server dies when Editor closes — `claude mcp list` will report `✗ Failed to connect` until Editor is reopened.
- VibeUE auto-enables dependencies on first load: `PythonScriptPlugin`, `EditorScriptingUtilities`, `EnhancedInput`, `AudioCapture`, `Niagara`, `MeshModelingToolset`, `ModelViewViewModel`, `StateTree`, `MetaSound`. The "no Python required" claim in VibeUE marketing is misleading — Python is used internally and is the most powerful tool surface.
- **Auth gotcha**: the startup `LogMCPServer: Error: SECURITY WARNING: ... starting with NO API key set. Any local process can connect and execute tools without authentication` is **misleading**. `initialize` and `tools/list` work without a key, but every `tools/call` returns `❌ A valid VibeUE API key is required`. The "any local process can connect" line refers to network reachability, not tool authorization. Always run with a key configured.
- API key lives in `Saved/Config/VibeUE.ini` (UE side) and is injected into MCP via `.mcp.json` env block on the Claude Code side. Both are gitignored.
- 10 tools exposed: `manage_asset`, `deep_research`, `read_logs`, `execute_python_code`, `discover_python_module`, `discover_python_class`, `discover_python_function`, `list_python_subsystems`, `manage_skills`, `terrain_data`. The Python tools are the deep-edit surface; `manage_asset` is the discovery/CRUD layer.
- VibeUE tools register into a Claude Code session **only if the Editor is up at session start** AND the API key is valid. If the handshake fails at startup, restart Claude Code after fixing the upstream cause.

## Findings / Tuning

**Workflow win — rapid PIE tuning via MCP.** With VibeUE wired in, edit-time CDO writes are a single Python round-trip from chat: read current value → propose change → apply → save → verify, all in ~200 ms. Iteration loop is fast enough to tune-and-feel without leaving Claude Code.

**Adopted tuning changes (this session):**
- `PrimeCatBase` → `CharacterMovement.MaxWalkSpeed`: **400.0 → 600.0** (new baseline). Confirmed in PIE: combined with the existing acceleration ease-in, the higher top speed enhances the weighty, momentum-based feel rather than making the cat float-y. Kept as the permanent default.

## Plugins
_(log plugins evaluated)_

## Notes
_(observations, gotchas, decisions)_
