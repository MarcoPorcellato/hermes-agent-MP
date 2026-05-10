# Daimon Discord Bot — Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Deploy Daimon, a two-tier Discord support bot for the Nous Research server — admins get full Hermes on host, users get a sandboxed agent in Docker with iteration caps and per-tool limits.

**Architecture:** Gateway on host constructs AIAgent per session. User sessions route terminal/file/execute_code tools to a shared Docker container (`daimon-sandbox`). Admin sessions run tools locally on host. Per-tool call limits, concurrency cap with FIFO queue, output redaction.

**Tech Stack:** Python 3.12, Docker, discord.py, existing Hermes gateway + Docker terminal backend, `gh` CLI, systemd timers.

**Spec:** `.plans/rfc-daimon-discord-bot.md`

---

## Decisions Summary

| Decision | Choice |
|---|---|
| Agent topology | Agent on host, tools in Docker (Option A) |
| Container model | Single shared long-lived container, per-thread workspace dirs |
| Iteration budget | 30 total tool loops per thread lifetime |
| Network policy | Block private nets + host gateway + metadata; allow public internet |
| Credential protection | Socket-based credential helper (root daemon, Unix socket) |
| Per-tool limits | Hard per-session call counts (see spec §5) |
| Model routing | MiMo v2.5 for users, Sonnet 4.6 for admins, no runtime switching |
| Thread ownership | Only creator + admins trigger agent |
| Concurrency | 50 max active sessions, FIFO queue |
| Daily limit | 5 threads/user/day (rolling 24h) |
| Memory access | Enabled for users (prompt-guided) |
| Repo sync | systemd timer every 5min (`git pull --ff-only && uv sync`) |

---

## File Inventory

### New Files

| Path | Purpose | ~LOC |
|------|---------|------|
| `gateway/daimon/__init__.py` | Package marker | 1 |
| `gateway/daimon/tier.py` | Tier detection (admin vs user) + model routing | 60 |
| `gateway/daimon/tool_limiter.py` | Per-tool call counter + enforcement | 80 |
| `gateway/daimon/concurrency.py` | Session tracker, FIFO queue, daily limit | 120 |
| `gateway/daimon/redaction.py` | Post-response regex filter for API key patterns | 50 |
| `gateway/daimon/workspace.py` | Workspace lifecycle (create/nuke per thread) | 40 |
| `gateway/daimon/config.py` | Config schema + defaults for daimon section | 50 |
| `docker/daimon-sandbox/Dockerfile` | Container image build | 80 |
| `docker/daimon-sandbox/docker-compose.yml` | Compose for sandbox + network | 45 |
| `docker/daimon-sandbox/credential-server.c` | Root daemon for GH token socket | 80 |
| `docker/daimon-sandbox/git-credential-daimon` | Python credential helper client | 30 |
| `docker/daimon-sandbox/entrypoint.sh` | Start daemon, drop privs | 25 |
| `docker/daimon-sandbox/network-setup.sh` | iptables rules for private net blocking | 20 |
| `docker/daimon-sandbox/daimon-repo-sync.service` | systemd service unit | 10 |
| `docker/daimon-sandbox/daimon-repo-sync.timer` | systemd timer unit | 10 |
| `tests/gateway/daimon/__init__.py` | Test package | 1 |
| `tests/gateway/daimon/test_tier.py` | Tier detection tests | 40 |
| `tests/gateway/daimon/test_tool_limiter.py` | Tool limiter tests | 80 |
| `tests/gateway/daimon/test_concurrency.py` | Concurrency + queue tests | 90 |
| `tests/gateway/daimon/test_redaction.py` | Redaction pattern tests | 60 |
| `tests/gateway/daimon/test_workspace.py` | Workspace create/nuke tests | 40 |

### Modified Files

| Path | Change | ~LOC delta |
|------|--------|-----------|
| `gateway/run.py` ~L13146-13900 | Hook tier detection, model routing, tool limiter, concurrency gate before `AIAgent()` construction | +50 |
| `gateway/platforms/discord.py` ~L534-620 | Thread-creator-only filter, `/daimon` admin commands, workspace lifecycle on thread close | +80 |
| `hermes_cli/commands.py` | Add `CommandDef("daimon", ...)` for admin commands | +5 |

---

## Implementation Order (Dependency Chain)

```
Phase 1: Core modules (no gateway changes, fully testable in isolation)
  ├── Task 1: gateway/daimon/config.py
  ├── Task 2: gateway/daimon/tier.py
  ├── Task 3: gateway/daimon/tool_limiter.py
  ├── Task 4: gateway/daimon/concurrency.py
  ├── Task 5: gateway/daimon/redaction.py
  └── Task 6: gateway/daimon/workspace.py

Phase 2: Gateway integration (wires modules into agent construction)
  ├── Task 7: Discord thread-creator filter
  ├── Task 8: Tier-based agent construction in gateway/run.py
  ├── Task 9: Tool limiter hook into agent loop
  ├── Task 10: Concurrency gate in gateway
  └── Task 11: Redaction in Discord send path

Phase 3: Docker infrastructure
  ├── Task 12: Dockerfile + entrypoint
  ├── Task 13: Credential server + helper
  ├── Task 14: docker-compose.yml + network setup
  └── Task 15: systemd timer for repo sync

Phase 4: Admin commands
  ├── Task 16: /daimon slash command registration
  └── Task 17: /daimon handlers (restart, status, kill, ban, limits)
```

---

## Phase 1: Core Modules

### Task 1: Daimon Config Schema

**Objective:** Define the configuration structure and defaults for the daimon subsystem.

**Files:**
- Create: `gateway/daimon/__init__.py`
- Create: `gateway/daimon/config.py`
- Test: `tests/gateway/daimon/__init__.py`
- Test: `tests/gateway/daimon/test_config.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/__init__.py
# (empty)
```

```python
# tests/gateway/daimon/test_config.py
from gateway.daimon.config import DaimonConfig, load_daimon_config

def test_defaults():
    """Empty config produces sane defaults."""
    cfg = load_daimon_config({})
    assert cfg.admin_users == []
    assert cfg.user_model == "xiaomi/mimo-v2.5-pro"
    assert cfg.admin_model == "anthropic/claude-sonnet-4.6"
    assert cfg.max_iterations == 30
    assert cfg.max_threads_per_day == 5
    assert cfg.gateway_timeout == 600
    assert cfg.max_active_sessions == 50
    assert cfg.tool_limits["web_search"] == 15
    assert cfg.tool_limits["browser"] == 20
    assert cfg.tool_limits["send_message"] == 0
    assert cfg.tool_limits["cronjob"] == 0

def test_override():
    """User config overrides defaults."""
    raw = {
        "gateway": {
            "discord": {
                "daimon": {
                    "admin_users": ["111", "222"],
                    "models": {"user": "openai/gpt-4o"},
                    "user_limits": {
                        "max_iterations": 50,
                        "tool_limits": {"web_search": 25},
                    },
                }
            }
        }
    }
    cfg = load_daimon_config(raw)
    assert cfg.admin_users == ["111", "222"]
    assert cfg.user_model == "openai/gpt-4o"
    assert cfg.max_iterations == 50
    assert cfg.tool_limits["web_search"] == 25
    # Unchanged defaults still hold
    assert cfg.tool_limits["browser"] == 20
```

**Step 2: Run test to verify failure**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_config.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'gateway.daimon'`

**Step 3: Write implementation**

```python
# gateway/daimon/__init__.py
"""Daimon — multi-user Discord bot access control and sandboxing."""
```

```python
# gateway/daimon/config.py
"""Configuration schema and defaults for Daimon Discord bot."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


_DEFAULT_TOOL_LIMITS = {
    "web_search": 15,
    "web_extract": 10,
    "browser": 20,
    "image_generate": 3,
    "delegate_task": 2,
    "text_to_speech": 0,
    "video_analyze": 2,
    "vision_analyze": 5,
    "cronjob": 0,
    "send_message": 0,
    "execute_code": 10,
}

_DEFAULT_ADMIN_ONLY_COMMANDS = [
    "update", "config", "model", "fallback", "setup", "cron",
    "webhook", "kanban", "hooks", "plugins", "memory", "tools",
    "mcp", "backup", "import", "profile", "rbac", "credentials",
    "daimon",
]


@dataclass
class DaimonConfig:
    """Resolved configuration for the Daimon subsystem."""

    admin_users: list[str] = field(default_factory=list)
    user_model: str = "xiaomi/mimo-v2.5-pro"
    admin_model: str = "anthropic/claude-sonnet-4.6"
    max_iterations: int = 30
    max_threads_per_day: int = 5
    gateway_timeout: int = 600
    max_active_sessions: int = 50
    queue_enabled: bool = True
    per_user_concurrent: bool = True
    tool_limits: dict[str, int] = field(default_factory=lambda: dict(_DEFAULT_TOOL_LIMITS))
    admin_only_commands: list[str] = field(default_factory=lambda: list(_DEFAULT_ADMIN_ONLY_COMMANDS))
    responders: list[str] = field(default_factory=lambda: ["creator", "admins"])


def load_daimon_config(raw_config: dict[str, Any]) -> DaimonConfig:
    """Parse raw config.yaml dict into a DaimonConfig.

    Reads from gateway.discord.daimon namespace.
    """
    daimon = (
        (raw_config.get("gateway") or {})
        .get("discord", {})
        .get("daimon", {})
    ) or {}

    models = daimon.get("models") or {}
    user_limits = daimon.get("user_limits") or {}
    concurrency = daimon.get("concurrency") or {}

    # Merge tool limits: user overrides on top of defaults
    tool_limits = dict(_DEFAULT_TOOL_LIMITS)
    user_tool_limits = user_limits.get("tool_limits") or {}
    tool_limits.update(user_tool_limits)

    return DaimonConfig(
        admin_users=daimon.get("admin_users") or [],
        user_model=models.get("user", "xiaomi/mimo-v2.5-pro"),
        admin_model=models.get("admin", "anthropic/claude-sonnet-4.6"),
        max_iterations=user_limits.get("max_iterations", 30),
        max_threads_per_day=user_limits.get("max_threads_per_day", 5),
        gateway_timeout=user_limits.get("gateway_timeout", 600),
        max_active_sessions=concurrency.get("max_active_sessions", 50),
        queue_enabled=concurrency.get("queue_enabled", True),
        per_user_concurrent=concurrency.get("per_user_concurrent", True),
        tool_limits=tool_limits,
        admin_only_commands=daimon.get("admin_only_commands") or list(_DEFAULT_ADMIN_ONLY_COMMANDS),
        responders=daimon.get("responders") or ["creator", "admins"],
    )
```

**Step 4: Run tests**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_config.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add gateway/daimon/ tests/gateway/daimon/
git commit -m "feat(daimon): config schema with defaults and override parsing"
```

---

### Task 2: Tier Detection

**Objective:** Determine whether a Discord user is admin or user tier, and route to the correct model.

**Files:**
- Create: `gateway/daimon/tier.py`
- Test: `tests/gateway/daimon/test_tier.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/test_tier.py
from gateway.daimon.tier import resolve_tier, Tier
from gateway.daimon.config import DaimonConfig


def test_admin_detected():
    cfg = DaimonConfig(admin_users=["111", "222"])
    tier = resolve_tier("111", cfg)
    assert tier == Tier.ADMIN


def test_user_detected():
    cfg = DaimonConfig(admin_users=["111", "222"])
    tier = resolve_tier("999", cfg)
    assert tier == Tier.USER


def test_admin_gets_admin_model():
    cfg = DaimonConfig(admin_users=["111"], admin_model="claude-opus")
    tier = resolve_tier("111", cfg)
    assert tier == Tier.ADMIN
    assert tier.model(cfg) == "claude-opus"


def test_user_gets_user_model():
    cfg = DaimonConfig(admin_users=["111"], user_model="mimo")
    tier = resolve_tier("999", cfg)
    assert tier == Tier.USER
    assert tier.model(cfg) == "mimo"


def test_empty_admin_list_everyone_is_user():
    cfg = DaimonConfig(admin_users=[])
    assert resolve_tier("111", cfg) == Tier.USER
```

**Step 2: Run test to verify failure**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_tier.py -v`
Expected: FAIL — `ImportError`

**Step 3: Write implementation**

```python
# gateway/daimon/tier.py
"""Two-tier access control: admin vs user."""
from __future__ import annotations

from enum import Enum
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from gateway.daimon.config import DaimonConfig


class Tier(Enum):
    ADMIN = "admin"
    USER = "user"

    def model(self, cfg: "DaimonConfig") -> str:
        """Return the model string for this tier."""
        if self == Tier.ADMIN:
            return cfg.admin_model
        return cfg.user_model

    @property
    def is_admin(self) -> bool:
        return self == Tier.ADMIN


def resolve_tier(user_id: str, cfg: "DaimonConfig") -> Tier:
    """Determine the tier for a Discord user ID."""
    if user_id in cfg.admin_users:
        return Tier.ADMIN
    return Tier.USER
```

**Step 4: Run tests**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_tier.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add gateway/daimon/tier.py tests/gateway/daimon/test_tier.py
git commit -m "feat(daimon): tier detection — admin vs user routing"
```

---

### Task 3: Tool Limiter

**Objective:** Track per-tool call counts and enforce session limits.

**Files:**
- Create: `gateway/daimon/tool_limiter.py`
- Test: `tests/gateway/daimon/test_tool_limiter.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/test_tool_limiter.py
import pytest
from gateway.daimon.tool_limiter import ToolLimiter


def test_unlimited_tool_always_allowed():
    limiter = ToolLimiter({"web_search": 5})
    # terminal has no limit configured
    assert limiter.check("terminal") is True
    limiter.record("terminal")
    assert limiter.check("terminal") is True


def test_limited_tool_enforced():
    limiter = ToolLimiter({"web_search": 2})
    assert limiter.check("web_search") is True
    limiter.record("web_search")
    assert limiter.check("web_search") is True
    limiter.record("web_search")
    assert limiter.check("web_search") is False


def test_disabled_tool():
    limiter = ToolLimiter({"send_message": 0})
    assert limiter.check("send_message") is False


def test_browser_actions_normalized():
    """All browser_* actions count toward a single 'browser' bucket."""
    limiter = ToolLimiter({"browser": 3})
    limiter.record("browser_navigate")
    limiter.record("browser_click")
    limiter.record("browser_type")
    assert limiter.check("browser_snapshot") is False


def test_remaining():
    limiter = ToolLimiter({"web_search": 5})
    assert limiter.remaining("web_search") == 5
    limiter.record("web_search")
    assert limiter.remaining("web_search") == 4
    assert limiter.remaining("terminal") is None  # unlimited


def test_denial_message():
    limiter = ToolLimiter({"image_generate": 1})
    limiter.record("image_generate")
    msg = limiter.denial_message("image_generate")
    assert "image_generate" in msg
    assert "1" in msg
```

**Step 2: Run test to verify failure**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_tool_limiter.py -v`
Expected: FAIL — `ImportError`

**Step 3: Write implementation**

```python
# gateway/daimon/tool_limiter.py
"""Per-tool call counting and enforcement for user sessions."""
from __future__ import annotations

from collections import defaultdict


class ToolLimiter:
    """Tracks per-tool call counts against configured session limits.

    Tools not in the limits dict are unlimited.
    Tools with limit=0 are disabled entirely.
    """

    def __init__(self, limits: dict[str, int]):
        self._limits = dict(limits)
        self._counts: dict[str, int] = defaultdict(int)

    @staticmethod
    def _normalize(tool_name: str) -> str:
        """Normalize tool names — all browser_* actions share one bucket."""
        if tool_name.startswith("browser_"):
            return "browser"
        return tool_name

    def check(self, tool_name: str) -> bool:
        """Return True if the tool call is allowed."""
        key = self._normalize(tool_name)
        limit = self._limits.get(key)
        if limit is None:
            return True  # not rate-limited
        if limit == 0:
            return False  # disabled
        return self._counts[key] < limit

    def record(self, tool_name: str) -> None:
        """Record a successful tool call."""
        key = self._normalize(tool_name)
        self._counts[key] += 1

    def remaining(self, tool_name: str) -> int | None:
        """Return remaining calls, or None if unlimited."""
        key = self._normalize(tool_name)
        limit = self._limits.get(key)
        if limit is None:
            return None
        return max(0, limit - self._counts[key])

    def denial_message(self, tool_name: str) -> str:
        """Human-readable message when a tool is denied."""
        key = self._normalize(tool_name)
        limit = self._limits.get(key, 0)
        if limit == 0:
            return f"Tool '{tool_name}' is disabled for this session."
        return (
            f"Tool limit reached: you've used all {limit} allowed calls "
            f"to '{key}' for this session."
        )
```

**Step 4: Run tests**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_tool_limiter.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add gateway/daimon/tool_limiter.py tests/gateway/daimon/test_tool_limiter.py
git commit -m "feat(daimon): per-tool session limiter with browser normalization"
```

---

### Task 4: Concurrency Manager

**Objective:** Track active sessions, enforce caps, FIFO queue, daily per-user limits.

**Files:**
- Create: `gateway/daimon/concurrency.py`
- Test: `tests/gateway/daimon/test_concurrency.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/test_concurrency.py
import time
import pytest
from gateway.daimon.concurrency import ConcurrencyManager


def test_acquire_under_limit():
    mgr = ConcurrencyManager(max_active=3, max_threads_per_day=5)
    ok, pos = mgr.try_acquire("thread_1", "user_A")
    assert ok is True
    assert pos == 0


def test_acquire_at_limit_queues():
    mgr = ConcurrencyManager(max_active=2, max_threads_per_day=5)
    mgr.try_acquire("t1", "u1")
    mgr.try_acquire("t2", "u2")
    ok, pos = mgr.try_acquire("t3", "u3")
    assert ok is False
    assert pos == 1  # first in queue


def test_release_dequeues():
    mgr = ConcurrencyManager(max_active=1, max_threads_per_day=5)
    mgr.try_acquire("t1", "u1")
    ok, _ = mgr.try_acquire("t2", "u2")
    assert ok is False
    # Release t1 — t2 should be next
    next_thread = mgr.release("t1")
    assert next_thread == "t2"


def test_daily_limit_enforced():
    mgr = ConcurrencyManager(max_active=50, max_threads_per_day=2)
    mgr.try_acquire("t1", "user_X")
    mgr.release("t1")
    mgr.try_acquire("t2", "user_X")
    mgr.release("t2")
    # Third thread same day — should be rejected
    allowed, reason = mgr.check_daily_limit("user_X")
    assert allowed is False
    assert "2" in reason


def test_daily_limit_different_users_independent():
    mgr = ConcurrencyManager(max_active=50, max_threads_per_day=2)
    mgr.try_acquire("t1", "user_A")
    mgr.try_acquire("t2", "user_A")
    # user_B unaffected
    allowed, _ = mgr.check_daily_limit("user_B")
    assert allowed is True


def test_active_count():
    mgr = ConcurrencyManager(max_active=10, max_threads_per_day=5)
    mgr.try_acquire("t1", "u1")
    mgr.try_acquire("t2", "u2")
    assert mgr.active_count == 2
    mgr.release("t1")
    assert mgr.active_count == 1
```

**Step 2: Run test to verify failure**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_concurrency.py -v`
Expected: FAIL — `ImportError`

**Step 3: Write implementation**

```python
# gateway/daimon/concurrency.py
"""Session concurrency manager with FIFO queue and daily per-user limits."""
from __future__ import annotations

import threading
import time
from collections import deque, defaultdict
from typing import Optional


class ConcurrencyManager:
    """Thread-safe session concurrency tracking.

    Enforces:
    - Max active sessions (global cap)
    - FIFO queue for overflow
    - Per-user daily thread creation limit (rolling 24h)
    """

    def __init__(self, max_active: int = 50, max_threads_per_day: int = 5):
        self._max_active = max_active
        self._max_threads_per_day = max_threads_per_day
        self._lock = threading.Lock()
        self._active: dict[str, str] = {}  # thread_id → user_id
        self._queue: deque[tuple[str, str]] = deque()  # (thread_id, user_id)
        # Per-user thread creation timestamps (rolling 24h)
        self._daily_usage: dict[str, list[float]] = defaultdict(list)

    @property
    def active_count(self) -> int:
        with self._lock:
            return len(self._active)

    @property
    def queue_length(self) -> int:
        with self._lock:
            return len(self._queue)

    def check_daily_limit(self, user_id: str) -> tuple[bool, str]:
        """Check if user has remaining daily thread allowance.

        Returns (allowed, reason_if_denied).
        """
        with self._lock:
            self._prune_daily(user_id)
            count = len(self._daily_usage[user_id])
            if count >= self._max_threads_per_day:
                return (
                    False,
                    f"You've used your {self._max_threads_per_day} threads for today. "
                    f"Try again in a few hours.",
                )
            return (True, "")

    def try_acquire(self, thread_id: str, user_id: str) -> tuple[bool, int]:
        """Try to acquire an active session slot.

        Returns (acquired, queue_position).
        queue_position is 0 if acquired, or 1-indexed queue position if queued.
        """
        with self._lock:
            # Record daily usage
            self._daily_usage[user_id].append(time.time())

            if len(self._active) < self._max_active:
                self._active[thread_id] = user_id
                return (True, 0)
            else:
                self._queue.append((thread_id, user_id))
                return (False, len(self._queue))

    def release(self, thread_id: str) -> Optional[str]:
        """Release an active session slot.

        Returns the next queued thread_id if one was promoted, else None.
        """
        with self._lock:
            self._active.pop(thread_id, None)
            # Also remove from queue if somehow there
            self._queue = deque(
                (tid, uid) for tid, uid in self._queue if tid != thread_id
            )
            # Promote next from queue
            if self._queue and len(self._active) < self._max_active:
                next_tid, next_uid = self._queue.popleft()
                self._active[next_tid] = next_uid
                return next_tid
            return None

    def _prune_daily(self, user_id: str) -> None:
        """Remove timestamps older than 24h for a user."""
        cutoff = time.time() - 86400
        self._daily_usage[user_id] = [
            ts for ts in self._daily_usage[user_id] if ts > cutoff
        ]
```

**Step 4: Run tests**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_concurrency.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add gateway/daimon/concurrency.py tests/gateway/daimon/test_concurrency.py
git commit -m "feat(daimon): concurrency manager with FIFO queue and daily limits"
```

---

### Task 5: Output Redaction

**Objective:** Regex filter to scrub API key patterns from agent responses before they reach Discord.

**Files:**
- Create: `gateway/daimon/redaction.py`
- Test: `tests/gateway/daimon/test_redaction.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/test_redaction.py
from gateway.daimon.redaction import redact_response


def test_openai_key_redacted():
    text = "The key is sk-proj-abc123def456ghi789jkl012mno345"
    result = redact_response(text)
    assert "sk-proj-" not in result
    assert "[REDACTED" in result


def test_github_pat_redacted():
    text = "Use this token: ghp_aBcDeFgHiJkLmNoPqRsTuVwXyZ123456789012"
    result = redact_response(text)
    assert "ghp_" not in result
    assert "[REDACTED" in result


def test_anthropic_key_redacted():
    text = "sk-ant-api03-abcdefghijklmnopqrstuvwxyz"
    result = redact_response(text)
    assert "sk-ant-" not in result


def test_aws_key_redacted():
    text = "Access key: AKIAIOSFODNN7EXAMPLE"
    result = redact_response(text)
    assert "AKIA" not in result


def test_normal_text_unchanged():
    text = "Here's how to install: pip install hermes-agent"
    assert redact_response(text) == text


def test_code_block_with_placeholder_unchanged():
    text = "```\nexport OPENAI_API_KEY=your-key-here\n```"
    assert redact_response(text) == text


def test_multiple_keys_in_one_response():
    text = "Found sk-proj-abcdefghijklmnopqrstuv and ghp_aBcDeFgHiJkLmNoPqRsTuVwXyZ1234567890AB"
    result = redact_response(text)
    assert "sk-proj-" not in result
    assert "ghp_" not in result
    assert result.count("[REDACTED") == 2
```

**Step 2: Run test to verify failure**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_redaction.py -v`
Expected: FAIL — `ImportError`

**Step 3: Write implementation**

```python
# gateway/daimon/redaction.py
"""Post-response redaction filter for API keys and credentials.

Runs on every agent response before it's sent to Discord.
Pattern-based (no access to actual secrets needed).
"""
from __future__ import annotations

import re

_PATTERNS: list[tuple[re.Pattern, str]] = [
    # OpenAI (sk-proj-..., sk-...)
    (re.compile(r"sk-proj-[a-zA-Z0-9\-_]{20,}"), "[REDACTED_OPENAI_KEY]"),
    (re.compile(r"sk-[a-zA-Z0-9]{20,}"), "[REDACTED_OPENAI_KEY]"),
    # GitHub PAT (ghp_, gho_, github_pat_)
    (re.compile(r"ghp_[a-zA-Z0-9]{36,}"), "[REDACTED_GITHUB_TOKEN]"),
    (re.compile(r"gho_[a-zA-Z0-9]{36,}"), "[REDACTED_GITHUB_TOKEN]"),
    (re.compile(r"github_pat_[a-zA-Z0-9_]{20,}"), "[REDACTED_GITHUB_TOKEN]"),
    # Anthropic
    (re.compile(r"sk-ant-[a-zA-Z0-9\-]{20,}"), "[REDACTED_ANTHROPIC_KEY]"),
    # xAI
    (re.compile(r"xai-[a-zA-Z0-9]{20,}"), "[REDACTED_XAI_KEY]"),
    # Google AI
    (re.compile(r"AIza[a-zA-Z0-9\-_]{30,}"), "[REDACTED_GOOGLE_KEY]"),
    # AWS access key
    (re.compile(r"AKIA[A-Z0-9]{16}"), "[REDACTED_AWS_KEY]"),
    # Discord bot token (Bot prefix + long base64-ish)
    (re.compile(r"Bot\s+[A-Za-z0-9._\-]{50,}"), "[REDACTED_BOT_TOKEN]"),
    # Generic credential patterns (key=..., token=..., etc.)
    (
        re.compile(
            r"(?:api[_-]?key|token|secret|password)\s*[:=]\s*[\"']?"
            r"[A-Za-z0-9+/=_\-]{32,}"
        ),
        "[REDACTED_CREDENTIAL]",
    ),
]


def redact_response(text: str) -> str:
    """Scrub known API key patterns from text.

    Returns the text with any matches replaced by [REDACTED_*] placeholders.
    """
    for pattern, replacement in _PATTERNS:
        text = pattern.sub(replacement, text)
    return text
```

**Step 4: Run tests**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_redaction.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add gateway/daimon/redaction.py tests/gateway/daimon/test_redaction.py
git commit -m "feat(daimon): post-response API key redaction filter"
```

---

### Task 6: Workspace Manager

**Objective:** Create and destroy per-thread workspace directories inside the Docker container.

**Files:**
- Create: `gateway/daimon/workspace.py`
- Test: `tests/gateway/daimon/test_workspace.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/test_workspace.py
from unittest.mock import patch, MagicMock
from gateway.daimon.workspace import WorkspaceManager


def test_workspace_path():
    mgr = WorkspaceManager(container_name="daimon-sandbox")
    assert mgr.workspace_path("12345") == "/workspaces/12345"


@patch("subprocess.run")
def test_create_workspace(mock_run):
    mock_run.return_value = MagicMock(returncode=0)
    mgr = WorkspaceManager(container_name="daimon-sandbox")
    mgr.create("thread_123")
    mock_run.assert_called_once()
    cmd = mock_run.call_args[0][0]
    assert "docker" in cmd[0] or "exec" in cmd
    assert "mkdir" in " ".join(cmd)
    assert "thread_123" in " ".join(cmd)


@patch("subprocess.run")
def test_destroy_workspace(mock_run):
    mock_run.return_value = MagicMock(returncode=0)
    mgr = WorkspaceManager(container_name="daimon-sandbox")
    mgr.destroy("thread_123")
    mock_run.assert_called_once()
    cmd = mock_run.call_args[0][0]
    assert "rm" in " ".join(cmd)
    assert "thread_123" in " ".join(cmd)


@patch("subprocess.run")
def test_destroy_refuses_path_traversal(mock_run):
    mgr = WorkspaceManager(container_name="daimon-sandbox")
    # Should refuse thread IDs with path traversal
    mgr.destroy("../etc")
    mock_run.assert_not_called()
```

**Step 2: Run test to verify failure**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_workspace.py -v`
Expected: FAIL — `ImportError`

**Step 3: Write implementation**

```python
# gateway/daimon/workspace.py
"""Workspace lifecycle management for per-thread Docker directories."""
from __future__ import annotations

import logging
import re
import shutil
import subprocess

logger = logging.getLogger(__name__)

_SAFE_THREAD_ID = re.compile(r"^[a-zA-Z0-9_\-]+$")


class WorkspaceManager:
    """Manages /workspaces/<thread_id>/ directories inside the Docker container."""

    def __init__(self, container_name: str = "daimon-sandbox"):
        self._container = container_name
        self._docker = shutil.which("docker") or "docker"

    def workspace_path(self, thread_id: str) -> str:
        """Return the absolute path inside the container for a thread."""
        return f"/workspaces/{thread_id}"

    def create(self, thread_id: str) -> None:
        """Create workspace directory for a thread."""
        if not self._validate_thread_id(thread_id):
            return
        path = self.workspace_path(thread_id)
        try:
            subprocess.run(
                [self._docker, "exec", self._container, "mkdir", "-p", path],
                capture_output=True,
                timeout=10,
            )
            logger.info("Created workspace: %s", path)
        except Exception as e:
            logger.error("Failed to create workspace %s: %s", path, e)

    def destroy(self, thread_id: str) -> None:
        """Destroy workspace directory for a thread."""
        if not self._validate_thread_id(thread_id):
            return
        path = self.workspace_path(thread_id)
        try:
            subprocess.run(
                [self._docker, "exec", self._container, "rm", "-rf", path],
                capture_output=True,
                timeout=30,
            )
            logger.info("Destroyed workspace: %s", path)
        except Exception as e:
            logger.error("Failed to destroy workspace %s: %s", path, e)

    def _validate_thread_id(self, thread_id: str) -> bool:
        """Reject thread IDs that could cause path traversal."""
        if not _SAFE_THREAD_ID.match(thread_id):
            logger.warning("Refused unsafe thread_id: %r", thread_id)
            return False
        return True
```

**Step 4: Run tests**

Run: `scripts/run_tests.sh tests/gateway/daimon/test_workspace.py -v`
Expected: PASS

**Step 5: Commit**

```bash
git add gateway/daimon/workspace.py tests/gateway/daimon/test_workspace.py
git commit -m "feat(daimon): workspace lifecycle manager with path traversal guard"
```

---

## Phase 2: Gateway Integration

### Task 7: Thread-Creator-Only Filter

**Objective:** Only process messages from the thread creator and admins in Daimon-managed threads.

**Files:**
- Modify: `gateway/platforms/discord.py`

**Step 1: Write failing test**

```python
# tests/gateway/daimon/test_thread_filter.py
from gateway.daimon.tier import resolve_tier, Tier
from gateway.daimon.config import DaimonConfig


def test_creator_allowed():
    """Thread creator's messages should be processed."""
    cfg = DaimonConfig(admin_users=["admin_1"])
    # Simulate: thread creator = "user_A", message author = "user_A"
    assert _should_process("user_A", thread_creator="user_A", cfg=cfg) is True


def test_admin_allowed_in_any_thread():
    """Admin can trigger agent in any thread."""
    cfg = DaimonConfig(admin_users=["admin_1"])
    assert _should_process("admin_1", thread_creator="user_A", cfg=cfg) is True


def test_other_user_rejected():
    """Non-creator non-admin messages are ignored."""
    cfg = DaimonConfig(admin_users=["admin_1"])
    assert _should_process("user_B", thread_creator="user_A", cfg=cfg) is False


def _should_process(author_id: str, thread_creator: str, cfg: DaimonConfig) -> bool:
    """Helper mimicking the filter logic."""
    from gateway.daimon.tier import resolve_tier, Tier
    tier = resolve_tier(author_id, cfg)
    if tier.is_admin:
        return True
    return author_id == thread_creator
```

**Implementation approach:** In the Discord adapter's `on_message` handler, after identifying this is a Daimon-managed thread, look up the thread creator (stored when the thread was spawned) and check `author_id == creator_id or tier.is_admin`. If neither, silently ignore the message (don't send an error — that would be noisy in public threads).

**Key code change in `gateway/platforms/discord.py`:**

```python
# In the message processing path, after identifying a daimon thread:
from gateway.daimon.config import load_daimon_config
from gateway.daimon.tier import resolve_tier

daimon_cfg = load_daimon_config(user_config)
tier = resolve_tier(str(message.author.id), daimon_cfg)

# Check if this user can trigger the agent in this thread
thread_creator_id = self._daimon_thread_creators.get(str(message.channel.id))
if not tier.is_admin and str(message.author.id) != thread_creator_id:
    return  # silently ignore
```

The `_daimon_thread_creators` dict is populated when the bot creates a thread (maps thread_id → creator user_id).

**Step 2: Commit**

```bash
git add gateway/platforms/discord.py tests/gateway/daimon/test_thread_filter.py
git commit -m "feat(daimon): thread-creator-only message filter"
```

---

### Task 8: Tier-Based Agent Construction

**Objective:** Wire tier detection into `_run_agent()` to set model, max_iterations, terminal backend, and disabled toolsets based on admin vs user.

**Files:**
- Modify: `gateway/run.py` (around L13146-13880)

**Implementation approach:** In `_run_agent()`, after loading config and before constructing `AIAgent`:

```python
# --- Daimon tier routing (Discord only) ---
from gateway.daimon.config import load_daimon_config
from gateway.daimon.tier import resolve_tier

daimon_cfg = load_daimon_config(user_config)
tier = resolve_tier(source.user_id, daimon_cfg)

if source.platform == "discord" and daimon_cfg.admin_users:
    # Override model based on tier
    if not tier.is_admin:
        # User tier: override model, cap iterations, set Docker backend
        model_override = daimon_cfg.user_model
        max_iterations = daimon_cfg.max_iterations
        # Disable tools with limit=0
        disabled_for_user = [
            tool for tool, limit in daimon_cfg.tool_limits.items() if limit == 0
        ]
        if disabled_toolsets:
            disabled_toolsets = list(set(disabled_toolsets + disabled_for_user))
        else:
            disabled_toolsets = disabled_for_user
    else:
        model_override = daimon_cfg.admin_model
        # Admin keeps default max_iterations
```

The model override feeds into `turn_route` resolution. The Docker backend for user tier is configured via existing `terminal.backend: docker` config — set conditionally per-session.

**Key insight from codebase:** The `DockerEnvironment` class (`tools/environments/docker.py`) already supports everything we need — security hardening, resource limits, volume mounts, env forwarding. We configure it for user sessions by setting the appropriate config values before agent construction.

**Step: Commit**

```bash
git add gateway/run.py
git commit -m "feat(daimon): tier-based model routing and iteration cap in gateway"
```

---

### Task 9: Tool Limiter Hook Into Agent Loop

**Objective:** Intercept tool calls before execution to check the tool limiter and return denial messages when limits are hit.

**Files:**
- Modify: `gateway/run.py` or use the existing `pre_tool_call` plugin hook

**Implementation approach:** The cleanest integration point is the existing **`pre_tool_call` plugin hook** (see `model_tools.py:722` and `run_agent.py:9687`). The plugin system already supports blocking tool calls by returning a message.

Create a lightweight Daimon plugin that registers a `pre_tool_call` hook:

```python
# gateway/daimon/tool_limit_hook.py
"""Plugin hook for tool call limiting in Daimon sessions."""

_session_limiters: dict[str, "ToolLimiter"] = {}  # session_id → limiter

def register_limiter(session_id: str, limiter: "ToolLimiter") -> None:
    """Register a tool limiter for a session."""
    _session_limiters[session_id] = limiter

def unregister_limiter(session_id: str) -> None:
    """Clean up limiter on session end."""
    _session_limiters.pop(session_id, None)

def pre_tool_call_hook(function_name: str, function_args: dict, **kwargs) -> str | None:
    """Called before each tool execution. Return a string to block."""
    session_id = kwargs.get("session_id")
    if not session_id:
        return None
    limiter = _session_limiters.get(session_id)
    if limiter is None:
        return None
    if not limiter.check(function_name):
        return limiter.denial_message(function_name)
    limiter.record(function_name)
    return None
```

**Alternative:** If plugin hooks add too much indirection, inject the limiter check directly into `AIAgent._invoke_tool()` via a callback parameter. The agent already has `tool_progress_callback`, `step_callback`, etc. — add a `tool_gate_callback: Callable[[str, dict], str | None]` that returns a denial message or None.

**Step: Commit**

```bash
git add gateway/daimon/tool_limit_hook.py
git commit -m "feat(daimon): tool limiter hook via pre_tool_call"
```

---

### Task 10: Concurrency Gate in Gateway

**Objective:** Check concurrency limits before constructing an agent. Queue if at capacity.

**Files:**
- Modify: `gateway/platforms/discord.py` (message handler)
- Uses: `gateway/daimon/concurrency.py`

**Implementation approach:** In the Discord adapter, when a new thread is created (bot spawns thread on @mention):

```python
# Before spawning thread and starting agent session:
daily_ok, daily_reason = self._concurrency_mgr.check_daily_limit(str(user_id))
if not daily_ok:
    await message.reply(daily_reason)
    return

acquired, queue_pos = self._concurrency_mgr.try_acquire(str(thread_id), str(user_id))
if not acquired:
    await thread.send(
        f"⏳ You're #{queue_pos} in queue. Estimated wait: ~{queue_pos * 2}min. "
        f"I'll notify you when it's your turn."
    )
    # Store callback to resume when slot opens
    self._queued_sessions[str(thread_id)] = (message, thread)
    return
```

On session end (thread close, timeout, or iteration exhaustion):

```python
next_thread = self._concurrency_mgr.release(str(thread_id))
if next_thread and next_thread in self._queued_sessions:
    # Notify and start the queued session
    msg, thread = self._queued_sessions.pop(next_thread)
    await thread.send("✅ Your turn! Starting session now...")
    # Proceed with agent construction
```

The `ConcurrencyManager` is instantiated once on the Discord adapter's `__init__` with config values from `DaimonConfig`.

**Step: Commit**

```bash
git add gateway/platforms/discord.py
git commit -m "feat(daimon): concurrency gate with FIFO queue on thread creation"
```

---

### Task 11: Redaction in Discord Send Path

**Objective:** Apply `redact_response()` to every message the agent sends to Discord.

**Files:**
- Modify: `gateway/platforms/discord.py` (in the `send()` method or at the gateway response dispatch point)

**Implementation approach:** The cleanest point is in `gateway/run.py` right after `agent.run_conversation()` returns, before the response is sent via the adapter:

```python
# gateway/run.py — after getting response from agent
response = result.get("final_response", "") if result else ""

# --- Daimon redaction (Discord user sessions only) ---
if source.platform == "discord" and not tier.is_admin:
    from gateway.daimon.redaction import redact_response
    response = redact_response(response)
```

Apply to both the main response and any streaming deltas if streaming is enabled.

**Step: Commit**

```bash
git add gateway/run.py
git commit -m "feat(daimon): apply redaction filter on Discord user responses"
```

---

## Phase 3: Docker Infrastructure

### Task 12: Dockerfile + Entrypoint

**Objective:** Build the `daimon-sandbox` container image with Python, uv, Node, gh, git, non-root user.

**Files:**
- Create: `docker/daimon-sandbox/Dockerfile`
- Create: `docker/daimon-sandbox/entrypoint.sh`

**Full Dockerfile and entrypoint content in spec §7.1 and §7.2.** Copy verbatim.

**Verification:**

```bash
cd docker/daimon-sandbox
docker build -t daimon-sandbox:latest .
docker run --rm daimon-sandbox:latest id
# Expected: uid=1000(agent) gid=1000(agent) groups=1000(agent)
docker run --rm daimon-sandbox:latest python --version
# Expected: Python 3.12.x
docker run --rm daimon-sandbox:latest uv --version
docker run --rm daimon-sandbox:latest gh --version
```

**Step: Commit**

```bash
git add docker/daimon-sandbox/Dockerfile docker/daimon-sandbox/entrypoint.sh
git commit -m "infra(daimon): Dockerfile with Python, uv, Node, gh, non-root agent user"
```

---

### Task 13: Credential Server + Helper

**Objective:** Socket-based GH token protection.

**Files:**
- Create: `docker/daimon-sandbox/credential-server.c`
- Create: `docker/daimon-sandbox/git-credential-daimon`

**credential-server.c:** See PoC at `docker-credential-poc/` (from subagent validation). Root daemon that:
1. Reads PAT from `/run/secrets/gh_token`
2. Listens on Unix socket `/run/git-credentials.sock`
3. Only serves `host=github.com` requests
4. Returns git credential format

**git-credential-daimon:** Python script that connects to socket, sends request, outputs response.

**Verification:**

```bash
# Build and start container with a test token
echo "ghp_test123" > /tmp/test-token.txt
docker compose up -d
docker exec daimon-sandbox su agent -c "gh auth status"
# Expected: logged in as daimon
docker exec daimon-sandbox su agent -c "echo \$GH_TOKEN"
# Expected: empty
docker exec daimon-sandbox su agent -c "cat /run/secrets/gh_token"
# Expected: Permission denied
```

**Step: Commit**

```bash
git add docker/daimon-sandbox/credential-server.c docker/daimon-sandbox/git-credential-daimon
git commit -m "infra(daimon): socket-based credential helper for GH token"
```

---

### Task 14: Docker Compose + Network Setup

**Objective:** Production-ready compose file with security hardening and network isolation.

**Files:**
- Create: `docker/daimon-sandbox/docker-compose.yml`
- Create: `docker/daimon-sandbox/network-setup.sh`

**docker-compose.yml:** See spec §7.4.

**network-setup.sh:** iptables rules from spec §7.5. Blocks RFC1918 + link-local + metadata + host gateway.

**Verification:**

```bash
docker compose up -d
# Test private network blocking
docker exec daimon-sandbox curl -s --connect-timeout 3 http://169.254.169.254/
# Expected: timeout/connection refused
docker exec daimon-sandbox curl -s --connect-timeout 3 http://10.0.0.1/
# Expected: timeout/connection refused
# Test public internet works
docker exec daimon-sandbox curl -s -o /dev/null -w "%{http_code}" https://github.com
# Expected: 200
```

**Step: Commit**

```bash
git add docker/daimon-sandbox/docker-compose.yml docker/daimon-sandbox/network-setup.sh
git commit -m "infra(daimon): docker-compose with security hardening + network isolation"
```

---

### Task 15: Systemd Timer for Repo Sync

**Objective:** Auto-pull hermes-agent inside container every 5 minutes.

**Files:**
- Create: `docker/daimon-sandbox/daimon-repo-sync.service`
- Create: `docker/daimon-sandbox/daimon-repo-sync.timer`

**Content from spec §7.6.**

**Verification:**

```bash
sudo cp docker/daimon-sandbox/daimon-repo-sync.* /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now daimon-repo-sync.timer
systemctl list-timers | grep daimon
# Expected: timer listed, next trigger in <5min
sudo systemctl start daimon-repo-sync.service
journalctl -u daimon-repo-sync.service --no-pager -n 5
# Expected: successful git pull output
```

**Step: Commit**

```bash
git add docker/daimon-sandbox/daimon-repo-sync.*
git commit -m "infra(daimon): systemd timer for repo sync every 5min"
```

---

## Phase 4: Admin Commands

### Task 16: /daimon Slash Command Registration

**Objective:** Register the `/daimon` command in the slash command registry.

**Files:**
- Modify: `hermes_cli/commands.py`

**Change:**

```python
# Add to COMMAND_REGISTRY list:
CommandDef("daimon", "Admin controls for Daimon bot (restart, status, kill, ban)", "Tools & Skills",
           aliases=(), args_hint="<restart|status|kill|ban|limits> [args]"),
```

**Step: Commit**

```bash
git add hermes_cli/commands.py
git commit -m "feat(daimon): register /daimon admin slash command"
```

---

### Task 17: /daimon Command Handlers

**Objective:** Implement restart, status, kill, ban, limits subcommands.

**Files:**
- Modify: `gateway/platforms/discord.py`

**Handlers:**

```python
async def _handle_daimon_command(self, subcommand: str, args: str, source):
    if subcommand == "restart":
        subprocess.run(["docker", "restart", "daimon-sandbox"], timeout=30)
        # Notify all active sessions
        for tid in list(self._concurrency_mgr._active):
            # send termination message to thread
            ...
        await source.reply("✅ Container restarted. All active sessions terminated.")

    elif subcommand == "status":
        active = self._concurrency_mgr.active_count
        queued = self._concurrency_mgr.queue_length
        # Docker stats
        stats = subprocess.run(
            ["docker", "stats", "daimon-sandbox", "--no-stream", "--format",
             "CPU: {{.CPUPerc}}, Mem: {{.MemUsage}}"],
            capture_output=True, text=True, timeout=5
        )
        await source.reply(
            f"**Daimon Status**\n"
            f"Active sessions: {active}/50\n"
            f"Queue: {queued}\n"
            f"Container: {stats.stdout.strip()}"
        )

    elif subcommand == "kill":
        thread_id = args.strip()
        self._concurrency_mgr.release(thread_id)
        # Nuke workspace
        self._workspace_mgr.destroy(thread_id)
        await source.reply(f"Killed session {thread_id}")

    elif subcommand == "ban":
        user_id = args.strip()
        # Persist to config (or in-memory blocklist)
        self._banned_users.add(user_id)
        await source.reply(f"Banned user {user_id}")

    elif subcommand == "limits":
        cfg = self._daimon_cfg
        await source.reply(
            f"**Limits:**\n"
            f"Iterations/thread: {cfg.max_iterations}\n"
            f"Threads/day/user: {cfg.max_threads_per_day}\n"
            f"Concurrency: {cfg.max_active_sessions}\n"
            f"Tool limits: {cfg.tool_limits}"
        )
```

**Step: Commit**

```bash
git add gateway/platforms/discord.py
git commit -m "feat(daimon): /daimon admin command handlers (restart, status, kill, ban, limits)"
```

---

## ⚔ Adversarial Review Gate

After Phase 1 is complete (all core modules with tests), run adversarial review before proceeding to Phase 2:

```bash
# Claude Code review — no priors, just the code
claude -p "Review the code in gateway/daimon/. Look at the tests in tests/gateway/daimon/. What issues do you see? Security concerns? Race conditions? Missing edge cases? Design problems?" --model claude-sonnet-4-20250514

# After Phase 2 — full integration review
claude -p "Review the changes to gateway/run.py and gateway/platforms/discord.py related to Daimon bot access control. The spec is at .plans/rfc-daimon-discord-bot.md. What's missing or wrong?" --model claude-sonnet-4-20250514
```

Fix issues before proceeding to Docker infrastructure.

---

## Testing Strategy

### Unit Tests (Phase 1)

All core modules have pure unit tests — no Docker, no network, no filesystem. Run with:

```bash
scripts/run_tests.sh tests/gateway/daimon/ -v
```

### Integration Tests (Phase 2)

Test the gateway integration with mocked Discord adapter and agent:

```bash
scripts/run_tests.sh tests/gateway/daimon/ tests/gateway/test_run.py -v -k daimon
```

### E2E Tests (Phase 3-4)

Manual verification with a real Discord bot in a test server:
1. @mention bot → thread created → agent responds
2. Non-creator posts in thread → ignored
3. Hit iteration limit → graceful message
4. Hit tool limit → denial message in agent response
5. `/daimon status` → shows stats
6. `/daimon restart` → container restarts, sessions terminated

---

## System Prompt (delivered as `ephemeral_system_prompt` for user sessions)

Stored in config or as a file at `~/.hermes/daimon-persona.md`. Injected via the existing `agent.system_prompt` config mechanism or `ephemeral_system_prompt` parameter.

Full content in spec §10.

---

## Adversarial Review Findings + Responses

Claude Code reviewed the spec + plan cold. Below are the findings, validity assessment, and required plan amendments.

### ⚠️ Valid Issues — Plan Must Address

| # | Finding | Severity | Fix |
|---|---------|----------|-----|
| 1 | **Agent cache reuse across tiers** — if admin and user share a session key (impossible given Discord threading, but defensively...) | Low | Tier-based changes produce different `_agent_config_signature` (model, toolsets, ephemeral prompt all differ). Cache invalidates naturally. No fix needed but add comment. |
| 2 | **Workspace cross-thread snooping** — users can `ls /workspaces/` and `cd` into other workspaces | Medium | Accepted risk (documented in spec §4). Mitigated by: system prompt, agent runs commands (not human directly), thread IDs are opaque Discord snowflakes. Could add per-workspace Linux user in future. |
| 3 | **Credential helper socket is mode 666** — anyone can `socat` to get the token | Medium | Accepted risk (documented in spec §19 D3). Token is scoped (issues+PRs only). Alternative: restrict socket to a group the agent user is in, require proper git-credential-fill protocol. |
| 4 | **Thread ownership doesn't survive gateway restart** — in-memory dict lost | High | **FIX REQUIRED:** Persist thread→creator mapping. Options: (a) SQLite table in session DB, (b) write to a JSON file in HERMES_HOME, (c) query Discord API on restart to recover thread starter. Plan amended to use session DB. |
| 5 | **Network iptables rules don't survive Docker restart** | Medium | **FIX REQUIRED:** Use Docker Compose network configuration with `com.docker.network.bridge.enable_ip_masquerade` + a systemd service that re-applies iptables on container start. Or use `docker-compose.yml` `networks.ipam` with `--internal` + a routing container. Plan amended. |
| 6 | **Queue position estimate is fiction** | Low | Remove "~2min" estimate. Just show position number and "I'll notify you when it's your turn." |
| 7 | **Tool limiter state cleanup** — dead sessions leak limiters | Medium | **FIX REQUIRED:** Add `unregister_limiter()` call in session cleanup path. Use weak references or TTL-based expiry. |

### ❌ Invalid Critiques — No Changes Needed

| # | Claim | Why It's Wrong |
|---|-------|----------------|
| A | "File tools execute on host filesystem, not Docker" | **WRONG.** `tools/file_tools.py` uses `ShellFileOperations` which wraps the terminal environment's `execute()` method. When backend=Docker, all file ops (read/write/patch/search) execute inside the container via `docker exec`. Verified in source. |
| B | "Model routing is determined by _resolve_turn_agent_config and can't be overridden" | **WRONG.** `_resolve_turn_agent_config` takes `model` as a parameter from the caller. We override the model BEFORE calling it. The function just builds the runtime dict around whatever model it receives. |
| C | "Slash command registration in commands.py doesn't create Discord commands" | **WRONG.** Per AGENTS.md: "All slash commands are defined in a central COMMAND_REGISTRY list of CommandDef objects. Every downstream consumer derives from this registry automatically" — including the Discord adapter's `telegram_bot_commands()` equivalent. |
| D | "Session hijacking via cache poisoning" | **WRONG.** Discord threads produce distinct session keys (includes thread_id). Different tiers within same thread would need the admin user to post in a user's thread — which is intentional behavior. Cache signature includes model + toolsets which differ per tier. |
| E | "execute_code confused deputy via delegate_task" | **WRONG.** `execute_code` runs inside Docker. `delegate_task` spawns a child AIAgent which inherits the SAME Docker backend config. There's no "steal data from Docker, exfiltrate via host delegate_task" path because delegate_task children use the same environment. |
| F | "ConcurrencyManager race condition with single lock" | **WRONG.** Using a single lock for both active sessions and queue is correct — it serializes all mutations. The code acquires the lock before any check-then-act sequence. The example given in the review is impossible because both operations are inside `with self._lock`. |

### Amendments to Plan

**Task 4 (Concurrency):** Add `unregister()` / TTL-based cleanup. Add gateway restart recovery for thread ownership.

**Task 10 (Concurrency Gate):** Remove time estimate from queue message. Just position number.

**Task 14 (Docker Compose):** Replace manual iptables script with a systemd service that re-applies rules on container start, OR use Docker's `--internal` network + a NAT gateway container pattern.

**Task (NEW — insert after Task 6):** Thread ownership recovery on restart. **Primary approach (B):** Query Discord API for active threads on startup — `thread.owner_id` gives the creator natively. **Fallback (A):** If Discord API proves unreliable (rate limits, missing data, archived threads don't return owner), add SQLite table `daimon_threads(thread_id TEXT PRIMARY KEY, creator_id TEXT, created_at REAL)` in session DB. Must validate approach B during testing phase before committing to A.

---

## Post-Implementation Checklist

- [ ] All Phase 1 tests pass
- [ ] Adversarial review passed (Phase 1)
- [ ] Gateway integration works with mock Discord events
- [ ] Docker image builds and passes security checks
- [ ] Credential helper validated (agent can't extract raw token)
- [ ] Network policy blocks private nets, allows public
- [ ] systemd timer pulls every 5 min
- [ ] `/daimon` commands work for admins
- [ ] Full E2E test in staging Discord server
- [ ] **Thread ownership recovery (B):** Restart gateway while threads are active → verify bot still only responds to thread creator. If `thread.owner_id` is unreliable or rate-limited, switch to SQLite fallback (A).
- [ ] Redaction filter catches all common key patterns
- [ ] Concurrency queue works under load (simulate 50+ simultaneous)
