# RFC: Multi-User Discord Bot — Simple Access Control

> **Status:** Draft  
> **Date:** 2026-05-05  
> **Authors:** glitch + teknium  
> **Context:** Ship a Discord support bot (@daimon) for the Nous Research server (~80K users). Must be safe enough to run live with minimal gatekeeping.

---

## Summary

Two-tier access control for the Discord gateway: **admins** get full Hermes, **users** get a capable but budget-capped agent in a Docker sandbox. No RBAC engine, no policy store, no Casbin — just config lists and a container.

---

## Threat Model

| Threat                   | Mitigation                                                                 |
| ------------------------ | -------------------------------------------------------------------------- |
| Steal secrets / API keys | Docker: no host filesystem, no `.env` access                               |
| Break the host system    | Docker: isolated container, wipe and restart                               |
| Abuse API credits        | Turn cap: `max_iterations` per-user, per-session                           |
| DDoS from our IP         | Docker network policy (egress allowlist)                                   |
| Run admin commands       | Slash command gating (application-level check)                             |
| Monopolize the agent     | Turn cap + gateway_timeout (short session lifetime for users)              |
| Persistent damage        | Docker: ephemeral container, reset on restart. No permanent writes escape. |

**Accepted risks (for now):**

- Users with terminal can do anything INSIDE the container (install packages, run servers, etc.)
- No per-user filesystem isolation within the container (all users share the same container fs)
- No per-user credential separation (everyone uses the same model key, billed to Nous)
- Agent could be tricked into making outbound requests (mitigated by network policy, not eliminated)

---

## Architecture

```
Discord message
  → Is user in admin_users?
    → YES: full Hermes (all tools, all slash commands, no limits)
    → NO: capped Hermes (all tools but turn-limited, no admin slash commands, Docker backend)
```

That's it. No enforcer, no policy store, no contextvar chain.

---

## Configuration

```yaml
# config.yaml — gateway.discord section
gateway:
    discord:
        enabled: true
        require_mention: true

        # ── Access Control ──────────────────────────────
        admin_users:
            - ""
            - ""

        # Everyone in DISCORD_ALLOWED_USERS who isn't in admin_users
        # gets the "user" tier automatically.

        # ── User Limits ─────────────────────────────────
        user_limits:
            max_iterations: 15 # max tool-calling loops per turn
            gateway_timeout: 600 # 10 min inactivity (vs 30 min for admins)
            # max_tokens: 4096        # optional: cap response length

        # ── Slash Command Gating ────────────────────────
        admin_only_commands:
            - update
            - config
            - model
            - fallback
            - setup
            - cron
            - webhook
            - kanban
            - hooks
            - plugins
            - memory
            - tools
            - mcp
            - backup
            - import
            - profile
            - rbac
            - credentials
```

### What Users GET (capable agent):

- All tools (terminal, file read/write, web, browser, vision, delegation, etc.)
- Full agentic capability within the Docker container
- Up to 15 iterations per turn (enough for real tasks, not enough to burn $50)
- 10 min inactivity timeout

### What Users DON'T GET:

- Admin slash commands (can't `/update`, `/config`, `/model`, `/cron`, etc.)
- Unlimited turns
- Unlimited session time
- Access to anything outside the Docker container

---

## Docker Backend

The terminal backend for user sessions is Docker. This gives:

```
┌─────────────────────────────────────────────┐
│ Host (runs gateway)                          │
│                                              │
│  ~/.hermes/.env (secrets) ── NOT mounted ──┐│
│  ~/github/ (source code) ── NOT mounted ──┐││
│                                            │││
│  ┌─────────────────────────────────────┐   │││
│  │ Docker container (user agent)        │   │││
│  │                                      │   │││
│  │  /workspace/ (ephemeral, user CWD)   │   │││
│  │  network: egress allowlist only      │   │││
│  │  no secrets, no host mounts          │   │││
│  │  wipe on restart                     │   │││
│  └─────────────────────────────────────┘   │││
└─────────────────────────────────────────────┘
```

Terminal config for users:

```yaml
terminal:
    backend: docker
    docker:
        image: "hermes-agent-sandbox:latest" # minimal image with python, git, common tools
        network_mode: "hermes-egress" # Docker network with egress allowlist
        memory: "2g"
        cpu: "1.0"
        workspace: "/workspace" # ephemeral, dies with container
        # No volume mounts to host filesystem
        # No env passthrough of secrets
```

Admin terminal uses the default local backend (full host access).

### Docker Network Policy (egress allowlist)

```bash
# Create isolated network
docker network create --driver bridge \
  --opt com.docker.network.bridge.enable_ip_masquerade=true \
  hermes-egress

# iptables rules (or Docker Compose network config):
# ALLOW: api.openai.com, api.anthropic.com, openrouter.ai (LLM APIs)
# ALLOW: github.com, pypi.org, npmjs.com (package managers)
# ALLOW: DNS (53/udp)
# DENY: everything else (private networks, arbitrary hosts)
```

This prevents the agent from being used as a DDoS tool or internal network scanner.

---

## Implementation

### Changes Required

| File                           | Change                                                                            | LOC |
| ------------------------------ | --------------------------------------------------------------------------------- | --- |
| `gateway/run.py`               | Check `admin_users` list, apply `user_limits` to non-admin `AIAgent` construction | ~20 |
| `gateway/platforms/discord.py` | Gate slash commands via `admin_only_commands` config                              | ~15 |
| `hermes_cli/config.py`         | Add `user_limits` and `admin_only_commands` config keys                           | ~10 |
| Docker setup                   | Dockerfile + compose for sandbox container                                        | ~30 |

**Total: ~75 lines of application code + Docker config.**

### Enforcement Points

**1. Turn cap (in `_run_agent()`):**

```python
# gateway/run.py — inside run_sync(), before AIAgent construction
is_admin = source.user_id in admin_user_ids

if not is_admin:
    user_limits = config.get("gateway", {}).get("discord", {}).get("user_limits", {})
    max_iterations = user_limits.get("max_iterations", 15)
    gateway_timeout = user_limits.get("gateway_timeout", 600)
else:
    max_iterations = int(os.getenv("HERMES_MAX_ITERATIONS", "90"))
    gateway_timeout = 1800

agent = AIAgent(
    max_iterations=max_iterations,
    ...
)
```

**2. Slash command gating (in Discord adapter):**

```python
# gateway/platforms/discord.py — in slash command handler
admin_only = config.get("admin_only_commands", [])
if command_name in admin_only and str(interaction.user.id) not in admin_user_ids:
    await interaction.response.send_message(
        "This command is admin-only.", ephemeral=True
    )
    return
```

**3. Docker backend for users:**

```python
# Already supported by Hermes terminal backend system
# Just set terminal.backend = "docker" for non-admin sessions
# Can be conditional: admin gets local, users get docker
```

---

## What This Doesn't Do (and that's fine for now)

| Not included                         | Why it's OK                                                    | When we'd need it                            |
| ------------------------------------ | -------------------------------------------------------------- | -------------------------------------------- |
| Per-user filesystem isolation        | Docker container is ephemeral, shared is fine                  | Multiple users doing persistent project work |
| Per-user credentials                 | Everyone uses Nous's model key, billing is centralized         | Users bring their own keys                   |
| Tool-level granularity               | All tools available inside container; container IS the sandbox | Different user tiers need different tools    |
| Privilege escalation / approval flow | Admin is always watching; can intervene manually               | 24/7 unattended operation                    |
| Skill gating                         | All skills available; worst case: agent follows a weird skill  | Skills that could cause real damage          |
| Persistent memory per-user           | Memory is shared; acceptable for support bot                   | Personal agent use cases                     |

---

## Migration Path to Full RBAC

This simple config IS the "RBAC disabled" default path. The full RBAC system (branch `sid/wip/user-permission-gating`) activates when `policies.db` exists. They're not in conflict:

```
No policies.db + simple config → this spec (admin/user split)
policies.db exists             → full RBAC (Casbin, per-tool, escalation, etc.)
```

When the bot outgrows simple admin/user:

1. Need more than 2 tiers → add RBAC roles
2. Need per-tool gating → enable RBAC policy
3. Need approval flows → enable escalation
4. Need per-user isolation inside Docker → Landlock or per-user containers

---

## Rollout Plan

1. **Day 1:** Deploy with admin_users + user_limits config. Docker for user terminal. Watch live.
2. **Week 1:** Observe what users try. Tighten network policy if needed. Adjust turn cap.
3. **Week 2+:** If abuse patterns emerge that config + Docker can't handle, enable specific RBAC policies for those cases.

---

## Open Questions

1. **Shared container or per-user container?** Shared container, but each thread spawns a new ephemeral workspace for the duration of the session. The container is long-lived and shared, but each session gets its own isolated workspace directory (e.g. `/workspace/<session-id>/`) that is torn down when the session ends. Hermes agent and docs are shared; tool access is shared.

2. **Should users be able to read each other's `/workspace/` files?** No — per-session subdirs mean each session's workspace is scoped to that thread. Shared tool and doc access is fine; workspace state is not shared across sessions.

3. **What happens when turn cap is hit?** Agent stops and says "I've reached my iteration limit for this request. Let me know if you need anything else." — clean termination, not an error.

4. **Network egress allowlist — what's allowed?** Proposal: LLM APIs + package managers + DNS. Block everything else. Too restrictive? Allow all outbound on ports 80/443 and only block RFC1918 (private networks)?

5. **How do we expose "admin watching" UX?** Admin gets notified of all user sessions? Or just check logs on demand? A dedicated `#agent-activity` channel with summaries?
