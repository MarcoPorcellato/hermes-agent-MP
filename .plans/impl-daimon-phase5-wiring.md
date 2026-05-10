# Phase 5: Gateway Wiring — Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Wire the `gateway/daimon/` modules into the live gateway — making `DaimonSessionManager` the actual decision point for Discord message processing, agent construction, tool gating, and response delivery.

**Architecture:** `DaimonSessionManager` is the single integration point. The Discord adapter instantiates it once, calls it at each lifecycle event. The gateway's `_run_agent()` uses `AgentOverrides` to configure the AIAgent. The tool gate hooks into the existing `pre_tool_call` plugin mechanism.

**Risk:** This phase modifies two 15K+ line files (`gateway/run.py`, `gateway/platforms/discord.py`). Changes must be surgical — add hooks, don't restructure.

---

## Decisions Summary

| Decision | Choice |
|---|---|
| Integration pattern | Minimal hooks into existing code — delegate all logic to DaimonSessionManager |
| System prompt delivery | Via `ephemeral_system_prompt` parameter on AIAgent (already supported) |
| Tool gating mechanism | Register a `pre_tool_call` plugin hook, OR use the `tool_gate_callback` pattern |
| Config loading | Load DaimonConfig once at gateway startup, reload on `/reload` command |
| Error handling | If DaimonSessionManager raises, fall back to normal (non-Daimon) behavior |

---

## Task 18: Discord Adapter — DaimonSessionManager Lifecycle

**Objective:** Instantiate `DaimonSessionManager` in the Discord adapter and hook thread lifecycle events.

**Files:**
- Modify: `gateway/platforms/discord.py` (~4997 lines)

**Changes:**

1. **In `DiscordAdapter.__init__()` (around line 530-540):**
```python
# After self._allowed_role_ids initialization
from gateway.daimon.session_manager import DaimonSessionManager
self._daimon: DaimonSessionManager | None = None
self._daimon_banned: set[str] = set()
```

2. **In `connect()` (around line 590-620, after config loading):**
```python
# Initialize Daimon session manager if configured
try:
    from gateway.daimon.config import load_daimon_config
    _daimon_cfg = load_daimon_config(_load_gateway_config())
    if _daimon_cfg.admin_users:
        from gateway.daimon.session_manager import DaimonSessionManager
        self._daimon = DaimonSessionManager(_load_gateway_config())
        logger.info("[Discord] Daimon active: %d admin(s)", len(_daimon_cfg.admin_users))
except Exception as e:
    logger.warning("[Discord] Daimon init failed: %s", e)
    self._daimon = None
```

3. **In `_handle_message()` (around line 4038-4055, after thread detection):**
```python
# Daimon thread-creator filter
if self._daimon and is_thread and thread_id:
    if not self._daimon.should_process_message(str(message.author.id), thread_id):
        return  # Silently ignore non-creator messages in Daimon threads
```

4. **In `_auto_create_thread()` return path (after thread is created):**
```python
# Register thread ownership for Daimon
if self._daimon and thread:
    # Start a Daimon session for this thread
    from gateway.daimon.session_manager import SessionStartResult
    result = self._daimon.start_session(
        str(thread.id), str(message.author.id), _load_gateway_config()
    )
    if not result.allowed and result.denial_reason:
        await thread.send(result.denial_reason)
        return None  # Don't process further
    if not result.allowed and result.queue_position > 0:
        await thread.send(
            f"⏳ You're #{result.queue_position} in queue. "
            f"I'll notify you when it's your turn."
        )
        # Store for later promotion notification
        self._daimon_queued[str(thread.id)] = thread
        return None
```

5. **Thread close/archive handler (new event listener):**
```python
@self._client.event
async def on_thread_update(before, after):
    if self._daimon and after.archived and not before.archived:
        thread_id = str(after.id)
        promoted = self._daimon.end_session(thread_id)
        if promoted and promoted in getattr(self, '_daimon_queued', {}):
            queued_thread = self._daimon_queued.pop(promoted)
            await queued_thread.send("✅ Your turn! Starting session now...")
```

6. **Admin command dispatch (in slash command handler):**
```python
# In the slash command dispatcher, when canonical == "daimon":
if self._daimon:
    from gateway.daimon.admin_commands import handle_daimon_command
    parts = cmd_args.split(None, 1)
    subcommand = parts[0] if parts else ""
    args = parts[1] if len(parts) > 1 else ""
    result = handle_daimon_command(subcommand, args, self._daimon, self._daimon_banned)
    await interaction.response.send_message(result.message, ephemeral=not result.success)
```

---

## Task 19: Gateway Run — Tier-Based Agent Construction

**Objective:** Apply `AgentOverrides` when constructing AIAgent for Discord sessions.

**Files:**
- Modify: `gateway/run.py` (around line 13146-13880)

**Changes:**

In `_run_agent()`, after config loading (~line 13154) and before agent construction (~line 13849):

```python
# --- Daimon tier-based overrides (Discord only) ---
_daimon_overrides = None
if source.platform == "discord":
    from gateway.daimon.agent_overrides import compute_overrides
    _daimon_overrides = compute_overrides(user_config, source.user_id, "discord")

if _daimon_overrides:
    # Override model
    if _daimon_overrides.model:
        model = _daimon_overrides.model
    # Override max_iterations
    if _daimon_overrides.max_iterations is not None:
        max_iterations = _daimon_overrides.max_iterations
    # Merge disabled toolsets
    if _daimon_overrides.disabled_toolsets:
        if disabled_toolsets:
            disabled_toolsets = list(set(disabled_toolsets + _daimon_overrides.disabled_toolsets))
        else:
            disabled_toolsets = _daimon_overrides.disabled_toolsets
    # Load Daimon system prompt
    if not _daimon_overrides.tier.is_admin:
        _daimon_prompt_path = Path(__file__).parent / "daimon" / "daimon-system-prompt.md"
        if _daimon_prompt_path.exists():
            combined_ephemeral = _daimon_prompt_path.read_text(encoding="utf-8")
```

This must go BEFORE `_resolve_turn_agent_config(message, model, runtime_kwargs)` since that function uses the `model` variable.

---

## Task 20: Tool Gate — Pre-Tool-Call Hook Registration

**Objective:** Wire `tool_gate.check_tool_call()` into the agent's tool execution path.

**Files:**
- Modify: `gateway/run.py` (in `_run_agent()`, register the gate on session start)
- OR: Create a lightweight plugin that the gateway registers

**Approach A (plugin hook — preferred if plugin system supports session context):**

The existing `get_pre_tool_call_block_message()` in `hermes_cli/plugins.py` fires for every tool call. If it has access to `session_id`, we can look up the limiter.

**Approach B (callback injection — simpler):**

In `_run_agent()`, after constructing the AIAgent, set a tool gate callback:

```python
# Register tool limiter for Daimon user sessions
if _daimon_overrides and not _daimon_overrides.tier.is_admin:
    from gateway.daimon.tool_gate import register_limiter, unregister_limiter, check_tool_call
    from gateway.daimon.tool_limiter import ToolLimiter
    from gateway.daimon.config import load_daimon_config

    _dcfg = load_daimon_config(user_config)
    _limiter = ToolLimiter(_dcfg.tool_limits)
    register_limiter(session_id, _limiter)

    # The tool gate check happens via the pre_tool_call plugin hook
    # which calls check_tool_call(session_id, tool_name)
```

Then ensure cleanup:
```python
# In the finally block after agent.run_conversation():
if _daimon_overrides and not _daimon_overrides.tier.is_admin:
    from gateway.daimon.tool_gate import unregister_limiter
    unregister_limiter(session_id)
```

---

## Task 21: Response Redaction in Send Path

**Objective:** Apply `redact_response()` to agent output before sending to Discord.

**Files:**
- Modify: `gateway/run.py` (around line 13391-13400, after getting response)

**Changes:**

```python
response = result.get("final_response", "") if result else ""

# --- Daimon output redaction (user sessions only) ---
if _daimon_overrides and not _daimon_overrides.tier.is_admin and response:
    from gateway.daimon.redaction import redact_response
    response = redact_response(response)
```

This is 3 lines. Apply to both the main response path and the streaming delta path if streaming is active.

---

## Task 22: System Prompt File Placement

**Objective:** Place the Daimon system prompt where the gateway can find it.

**Files:**
- Move: `docker/daimon-sandbox/daimon-system-prompt.md` → `gateway/daimon/daimon-system-prompt.md`

The system prompt should live alongside the gateway code (not in the Docker dir) since it's loaded by the gateway process on the host, not inside the container.

---

## Task 23: Banned User Check

**Objective:** Block banned users before they can start sessions.

**Files:**
- Modify: `gateway/platforms/discord.py` (in `_handle_message()`, early in the path)

**Changes:**

```python
# Early ban check (before any processing)
if self._daimon and str(message.author.id) in self._daimon_banned:
    return  # Silently ignore banned users
```

---

## Implementation Order

```
Task 22 (move system prompt) — no deps, trivial
    ↓
Task 18 (Discord adapter lifecycle) — core integration
    ↓
Task 19 (gateway/run.py overrides) — depends on Task 18 interface
    ↓
Task 20 (tool gate hook) — depends on Task 19 session setup
    ↓
Task 21 (redaction) — depends on Task 19 response path
    ↓
Task 23 (ban check) — depends on Task 18 daimon instance
```

---

## Verification

After all tasks:
1. `python -m pytest tests/gateway/daimon/ -q` — all 153+ tests pass
2. `python -c "from gateway.daimon.session_manager import DaimonSessionManager; print('✅')"` — imports clean
3. Full gateway test suite: `scripts/run_tests.sh tests/gateway/ -q` — no regressions
4. Manual smoke test: start gateway with Daimon config, verify bot responds in Discord

---

## Adversarial Review Gates

- After Task 18+19: Review the diff of discord.py and run.py changes
- After Task 20+21: Review the complete tool gating + redaction path
- Final: Full coherence review of the wiring against spec §6 (tool routing matrix) and §9 (session lifecycle)
