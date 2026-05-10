# Spec: Role-Based Tier Resolution for Daimon

> **Status:** Approved  
> **Date:** 2026-05-10  
> **Scope:** gateway/daimon/ + gateway/session.py + gateway/platforms/discord.py + gateway/run.py  
> **Branch:** hermes/hermes-4fa48a27 (worktree)

---

## 1. Goal

Replace user-ID-only tier detection with Discord role-based resolution. Admins and users are identified by Discord roles (with user_id lists as fallback/override). A debug flag allows forcing a specific tier for testing.

---

## 2. Tier Resolution Logic

```
resolve_tier(user_id, role_ids, cfg) -> Tier | None

1. If debug_force_tier is set → return that tier (all users forced)
2. If user_id in admin_users → ADMIN
3. If any(role_id in admin_roles for role_id in user_role_ids) → ADMIN
4. If user_roles is empty (not configured) → USER (open access)
5. If user_id in user_users → USER  
6. If any(role_id in user_roles for role_id in user_role_ids) → USER
7. Otherwise → None (silent ignore — bot does not respond)
```

**Highest privilege wins.** Admin from ANY source = admin. No demotion possible.

**None = silent ignore.** When user_roles IS configured and user matches nothing, the message is dropped silently (same behavior as current DISCORD_ALLOWED_USERS filtering).

---

## 3. Config Schema

```yaml
# ~/.hermes/profiles/daimon/config.yaml
gateway:
  discord:
    daimon:
      # --- Role-based access (NEW) ---
      admin_roles: ["1214801236323467284"]   # Moderator role → admin tier
      user_roles: ["1149869525567799297"]    # Developer role → user tier (testing gate)
      # Empty user_roles = open access (anyone can use bot)
      
      # --- User-ID overrides (existing, kept for individual grants) ---
      admin_users: ["690519146701783042"]    # teknium — always admin regardless of roles
      user_users: []                          # Individual user-tier grants (no role needed)
      
      # --- Debug (NEW) ---
      debug_force_tier: null                 # Set to "user" or "admin" to force ALL users to that tier
      
      # --- Everything else unchanged ---
      admin_model: "anthropic/claude-sonnet-4.6"
      user_model: "anthropic/claude-sonnet-4-20250514"
      max_iterations: 30
      tool_limits: { ... }
```

**Extensibility note:** The two-tier system (admin/user) is kept for now, but the config structure supports adding more tiers later as a list-of-tier-definitions without breaking existing configs.

---

## 4. Implementation Plan

### 4.1 `gateway/session.py` — Add role_ids to SessionSource

```python
@dataclass
class SessionSource:
    ...
    role_ids: Optional[list[str]] = None  # Platform role IDs (Discord roles, Slack roles, etc.)
```

### 4.2 `gateway/platforms/discord.py` — Populate role_ids

At the `build_source()` call (~line 4249), add:

```python
source = self.build_source(
    ...
    role_ids=[str(r.id) for r in message.author.roles] if hasattr(message.author, 'roles') else None,
)
```

Note: `message.author.roles` includes `@everyone` (role ID = guild ID). We don't filter it — the config simply won't include that ID.

Also: **remove `DISCORD_ALLOWED_USERS` and `DISCORD_ALLOWED_ROLES` from daimon profile .env.** The `require_mention: true` config + `DISCORD_ALLOWED_CHANNELS` remain as the outer gate. Daimon tier resolution is the sole access control.

### 4.3 `gateway/daimon/config.py` — New fields

```python
@dataclass
class DaimonConfig:
    admin_users: list[str] = field(default_factory=list)
    admin_roles: list[str] = field(default_factory=list)      # NEW
    user_users: list[str] = field(default_factory=list)       # NEW (renamed concept)
    user_roles: list[str] = field(default_factory=list)       # NEW
    debug_force_tier: Optional[str] = None                     # NEW
    ...
```

### 4.4 `gateway/daimon/tier.py` — Role-aware resolution

```python
def resolve_tier(
    user_id: str,
    cfg: DaimonConfig,
    role_ids: Optional[list[str]] = None,
) -> Optional[Tier]:
    """Determine tier. Returns None if user should be silently ignored."""
    
    # Debug override
    if cfg.debug_force_tier:
        return Tier(cfg.debug_force_tier)
    
    # Admin checks (highest privilege wins)
    if user_id in cfg.admin_users:
        return Tier.ADMIN
    if role_ids and cfg.admin_roles:
        if set(role_ids) & set(cfg.admin_roles):
            return Tier.ADMIN
    
    # User checks
    if not cfg.user_roles:
        # No user_roles configured = open access
        return Tier.USER
    if user_id in cfg.user_users:
        return Tier.USER
    if role_ids and set(role_ids) & set(cfg.user_roles):
        return Tier.USER
    
    # No match + user_roles configured = silent ignore
    return None
```

### 4.5 `gateway/daimon/agent_overrides.py` — Pass role_ids through

```python
def compute_overrides(
    raw_config: dict,
    user_id: str,
    platform: str,
    role_ids: Optional[list[str]] = None,  # NEW param
) -> Optional[AgentOverrides]:
    ...
    tier = resolve_tier(user_id, cfg, role_ids=role_ids)
    
    if tier is None:
        # Return a sentinel that tells gateway to drop the message
        return AgentOverrides(tier=None)  # or a new DENIED state
    ...
```

### 4.6 `gateway/run.py` ~13207 — Pass source.role_ids

```python
_daimon_overrides = get_agent_overrides(user_config, source.user_id, platform_key, role_ids=source.role_ids)
```

And handle the "ignored" case (tier=None → don't process, return silently).

### 4.7 `gateway/daimon/gateway_hooks.py` — Signature update

```python
def get_agent_overrides(raw_config, user_id, platform, role_ids=None):
    return compute_overrides(raw_config, user_id, platform, role_ids=role_ids)
```

---

## 5. Daimon Profile .env Changes

```bash
# REMOVE these (Daimon roles replace them):
# DISCORD_ALLOWED_USERS=466492386873311235
# DISCORD_ALLOWED_ROLES=...

# KEEP these:
DISCORD_BOT_TOKEN=<bot token>
DISCORD_ALLOWED_CHANNELS=1485307775444844625,1502698924786450632
```

---

## 6. Testing Config (Initial Deployment)

```yaml
gateway:
  discord:
    daimon:
      admin_roles: ["1214801236323467284"]        # Moderator
      admin_users: ["690519146701783042"]          # teknium (individual)
      user_roles: ["1149869525567799297"]          # Developer (testing whitelist)
      user_users: []
      debug_force_tier: null                       # Set to "user" to test user-tier as admin
```

---

## 7. Test Plan (Discord CLI Tool)

**Tool:** `/tmp/discord_daimon_test.py` — uv inline-deps script (requests, rich).  
**Token:** `~/.hermes/profiles/daimon/credentials/discord_user_token` (600 perms).  
**Usage:**
```bash
uv run /tmp/discord_daimon_test.py post "title" "msg" --mention
uv run /tmp/discord_daimon_test.py reply <thread_id> "msg" --mention
uv run /tmp/discord_daimon_test.py threads --limit 10
uv run /tmp/discord_daimon_test.py read <thread_id> --limit 20
```

### 7.1 Test Cases (Post-Implementation)

| # | Scenario | Config State | Expected | How to Verify |
|---|----------|-------------|----------|---------------|
| 1 | Admin via role | admin_roles has Moderator, sidbin has Moderator | Admin tier: claude-sonnet-4.6, no tool limits | Post → check model in response |
| 2 | Admin via user_id | admin_users has teknium's ID | Admin tier regardless of roles | Have teknium post, or add own ID |
| 3 | User via role | debug_force_tier: null, user_roles has Developer, sidbin has Developer | User tier: mimo model, 30 iter cap | Set debug_force_tier=user OR remove Moderator from admin_roles temporarily |
| 4 | debug_force_tier=user | debug_force_tier: "user" | ALL users (including admins) get user tier | Post as admin → should get mimo model |
| 5 | debug_force_tier=admin | debug_force_tier: "admin" | ALL users get admin tier | Useful for one-off full-access grants |
| 6 | Silent ignore | user_roles configured, user has no matching role | No response from bot | Need alt account or temporarily set user_roles to a role you don't have |
| 7 | Open access | user_roles: [] (empty) | Everyone gets user tier | Remove user_roles list, post as anyone |
| 8 | Highest privilege wins | User has BOTH admin_role AND user_role | Admin tier (not user) | sidbin has both Moderator + Developer |
| 9 | @mention in initial post | -- | Bot responds in new thread | `post "test" "msg" --mention` |
| 10 | @mention in follow-up | -- | Bot responds in existing thread | `reply <id> "msg" --mention` |
| 11 | Rapid-fire (3 msgs <2s) | -- | Bot handles gracefully (text batching) | Script sends burst |
| 12 | No @mention | -- | Bot ignores (require_mention: true) | `post "test" "msg"` without --mention |

### 7.2 Verification Method

For each test: post via CLI tool → wait 30s → `read` the thread → confirm:
- Model used (bot often mentions it or we check gateway logs)
- Response vs silence
- `tail -20 ~/.hermes/profiles/daimon/logs/gateway.log` for tier resolution logs

---

## 8. Discord Intent Requirement

Bot MUST have **Server Members Intent** enabled in Discord Developer Portal. The existing adapter code already requests `members=True` intent when role-based features are active (line ~658). Document as hard requirement — if not enabled, `message.author` won't have `.roles` populated in guild contexts.

---

## 9. Execution Plan (Implementation TODOs)

Each task gets an adversarial review via Claude Code (`/review` in tmux) after completion.

| # | Task | Files | Review Focus |
|---|------|-------|--------------|
| 1 | Add `role_ids: Optional[list[str]] = None` to `SessionSource` | `gateway/session.py` | Backward compat — ensure no downstream code breaks with None default |
| 2 | Populate `role_ids` in Discord adapter `build_source()` | `gateway/platforms/discord.py` | Edge cases: DM context (no Member), webhook messages, bot messages |
| 3 | Add `admin_roles`, `user_roles`, `user_users`, `debug_force_tier` to `DaimonConfig` + `load_daimon_config()` | `gateway/daimon/config.py` | Null safety, YAML type coercion (string vs int role IDs), empty list vs None |
| 4 | Rewrite `resolve_tier()` with role-aware logic + None return | `gateway/daimon/tier.py` | Priority ordering correctness, set intersection perf, None propagation |
| 5 | Update `compute_overrides()` to pass role_ids + handle None tier | `gateway/daimon/agent_overrides.py` | Sentinel handling — gateway must distinguish "no overrides" from "ignore user" |
| 6 | Update `get_agent_overrides()` signature | `gateway/daimon/gateway_hooks.py` | Backward compat if called without role_ids kwarg |
| 7 | Wire `source.role_ids` through in `gateway/run.py` + handle ignored tier | `gateway/run.py` | Silent return path — no error, no response, no session creation. Must not leak into other code paths. |
| 8 | Update daimon profile: .env (remove DISCORD_ALLOWED_USERS) + config.yaml (add roles) | config files | Verify gateway still connects, no regression on channel filtering |
| 9 | Restart service + E2E test all scenarios from §7.1 | systemd + CLI tool | Full integration pass |

### Review Protocol

After each task (1–7), run adversarial review in tmux:

```bash
tmux send-keys -t codex-review "cd ~/github/hermes-agent/.worktrees/hermes-4fa48a27 && claude -p 'review the git diff of the last commit. focus on: backward compatibility, null safety, edge cases where role_ids could be None or empty, and whether the change could break any existing non-Daimon gateway deployments. be adversarial — find bugs, not praise.'" Enter
```

Wait for review → address findings → commit → next task.

---

## 10. Migration Path

1. Implement tasks 1–7 (code changes)
2. Adversarial review per task
3. Update daimon profile .env (remove DISCORD_ALLOWED_USERS)
4. Update daimon profile config.yaml with role IDs
5. Restart `hermes-gateway-daimon.service`
6. Run test cases from §7.1 (admin path first, then debug_force_tier, then user path)
7. Remove debug flag when satisfied
8. Eventually: remove user_roles whitelist for open access (production)
