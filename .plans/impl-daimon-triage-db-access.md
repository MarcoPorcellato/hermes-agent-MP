# Daimon Triage DB Access — Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Give the Daimon Discord bot's sandboxed agent access to the triage DB (120MB SQLite with 22K+ triaged issues/PRs) so user support sessions can search existing issues, find duplicates, and reference prior resolutions.

**Architecture:** Read-only bind-mounts from host into the Docker sandbox container. The triage DB, query scripts, and bare git repo are mounted at `/opt/triage/`. The `TRIAGE_HOME` env var tells the scripts where to find the DB. The system prompt is updated to teach the agent the search commands.

**Tech Stack:** Docker bind-mounts, SQLite (read-only), existing Python triage scripts, Daimon profile config.yaml.

---

## Decisions Summary

| Decision | Choice |
|---|---|
| Mount mechanism | Docker bind-mount via `docker_volumes` in daimon profile config.yaml |
| DB access mode | Read-only (`:ro` flag) — container can never corrupt the DB |
| Container path | `/opt/triage/{db,scripts,hermes-agent.git}` |
| Env var | `TRIAGE_HOME=/opt/triage` in `docker_env` |
| Source repo mount | Bind-mount host checkout at `/opt/hermes-agent:ro` (fixes empty volume bug) |
| Bare repo mount | `/opt/triage/hermes-agent.git:ro` for `git show HEAD:<file>` |
| System prompt changes | Add "Triage DB" section with search command examples |
| Agent invocation | `cd /opt/triage && python3 scripts/search_db.py "query"` (no `uv run` needed — scripts have zero deps) |

---

## File Inventory

### Modified Files

| Path | Change | ~LOC delta |
|------|--------|-----------|
| `~/.hermes/profiles/daimon/config.yaml` | Add `docker_volumes` entries + `docker_env.TRIAGE_HOME` | +8 |
| `docker/daimon-sandbox/daimon-system-prompt.md` (in worktree) | Add triage DB section | +25 |
| `docker/daimon-sandbox/docker-compose.yml` (in worktree) | Replace named volume with bind-mount, add triage mounts | +6, -2 |

### New Files

| Path | Purpose | ~LOC |
|------|---------|------|
| `/tmp/test_triage_e2e.py` | E2E test script — posts to Discord, waits for Daimon response containing triage results | ~120 |

---

## Implementation Order (Dependency Chain)

```
Task 1: Update daimon profile config.yaml (docker_volumes + docker_env)
    ↓
Task 2: Update docker-compose.yml (fix empty volume + add triage mounts)
    ↓
Task 3: Update system prompt (teach agent about triage DB)
    ↓
Task 4: Verify — exec into container, confirm mounts work
    ↓
Task 5: Restart Daimon gateway to pick up config changes
    ↓
Task 6: E2E test — post to Discord, verify agent uses triage DB
```

---

## Task 1: Update Daimon Profile Config

**Objective:** Configure the Hermes Docker terminal backend to mount triage assets and hermes-agent source into every spawned container.

**Files:**
- Modify: `~/.hermes/profiles/daimon/config.yaml` (lines 62, 71)

**Step 1: Set docker_env with TRIAGE_HOME**

Change line 62 from:
```yaml
  docker_env: {}
```
to:
```yaml
  docker_env:
    TRIAGE_HOME: /opt/triage
```

**Step 2: Set docker_volumes with all bind-mounts**

Change line 71 from:
```yaml
  docker_volumes: []
```
to:
```yaml
  docker_volumes:
    - "/home/daimon/github/hermes-agent:/opt/hermes-agent:ro"
    - "/home/daimon/projects/triage/db:/opt/triage/db:ro"
    - "/home/daimon/projects/triage/scripts:/opt/triage/scripts:ro"
    - "/home/daimon/projects/triage/hermes-agent.git:/opt/triage/hermes-agent.git:ro"
```

**Step 3: Verify config syntax**

Run: `python3 -c "import yaml; yaml.safe_load(open('/home/daimon/.hermes/profiles/daimon/config.yaml'))"`
Expected: No error (valid YAML)

---

## Task 2: Update docker-compose.yml

**Objective:** Fix the empty named volume problem and add triage bind-mounts to the sandbox container definition (for when docker-compose is used directly, e.g., container rebuild).

**Files:**
- Modify: `~/github/hermes-agent/.worktrees/hermes-4fa48a27/docker/daimon-sandbox/docker-compose.yml`

**Step 1: Replace the daimon-sandbox volumes section**

Change:
```yaml
    volumes:
      - hermes-repo:/opt/hermes-agent:ro
```
to:
```yaml
    volumes:
      - /home/daimon/github/hermes-agent:/opt/hermes-agent:ro
      - /home/daimon/projects/triage/db:/opt/triage/db:ro
      - /home/daimon/projects/triage/scripts:/opt/triage/scripts:ro
      - /home/daimon/projects/triage/hermes-agent.git:/opt/triage/hermes-agent.git:ro
    environment:
      TRIAGE_HOME: /opt/triage
```

**Step 2: Remove the now-unused named volume declaration**

Remove from the bottom:
```yaml
volumes:
  hermes-repo:
```

**Note:** The broker container's volumes remain unchanged (it only needs the GH token).

---

## Task 3: Update System Prompt

**Objective:** Teach the Daimon agent how to search the triage DB when helping users with support questions.

**Files:**
- Modify: `~/github/hermes-agent/.worktrees/hermes-4fa48a27/docker/daimon-sandbox/daimon-system-prompt.md`

**Step 1: Add triage DB section after "## Environment"**

Insert after the environment section:

```markdown
## Triage Database

You have read-only access to a triage DB containing 22K+ issues and PRs from NousResearch/hermes-agent, with labels, priorities, duplicate links, and triage notes.

**Search by keywords:**
```bash
cd /opt/triage && python3 scripts/search_db.py "gateway crash telegram"
```

**Search by issue number (find similar):**
```bash
cd /opt/triage && python3 scripts/search_db.py --number 22500
```

**Search specific field:**
```bash
cd /opt/triage && python3 scripts/search_db.py --field triage_note "CWD resolution"
```

**FTS5 boolean queries (OR, AND, phrases):**
```bash
cd /opt/triage && python3 scripts/query_db.py --match '"memory capture" OR auto_capture'
```

**Raw SQL (read-only):**
```bash
cd /opt/triage && python3 scripts/query_db.py --sql "SELECT number, title, triage_note FROM items WHERE duplicate_of = 19242"
```

**Inspect source code (bare repo):**
```bash
git --git-dir=/opt/triage/hermes-agent.git show HEAD:gateway/run.py | head -50
git --git-dir=/opt/triage/hermes-agent.git log --oneline -10 -- tools/browser_tool.py
```

Use the triage DB when:
- User reports a bug → search for existing issues first
- User asks "is this a known issue?" → search by keywords
- Reproducing a bug → find related issues for context
- Filing a new issue → check for duplicates first
```

**Step 2: Update the Skills section to mention triage**

Add to the skills list:
```markdown
- `github-issue-triage` — searching the triage DB, duplicate detection
```

---

## Task 4: Verify Mounts Inside Container

**Objective:** Confirm the bind-mounts are accessible from inside the running container after recreating it.

**Step 1: Recreate the sandbox container with new compose**

```bash
cd ~/github/hermes-agent/.worktrees/hermes-4fa48a27/docker/daimon-sandbox
docker compose down daimon-sandbox
docker compose up -d daimon-sandbox
```

**Step 2: Verify mounts exist**

```bash
docker exec daimon-sandbox ls /opt/hermes-agent/run_agent.py
docker exec daimon-sandbox ls /opt/triage/db/triage.db
docker exec daimon-sandbox ls /opt/triage/scripts/search_db.py
docker exec daimon-sandbox ls /opt/triage/hermes-agent.git/HEAD
```

Expected: All four files exist (no "No such file" errors).

**Step 3: Verify read-only**

```bash
docker exec daimon-sandbox touch /opt/triage/db/test_write 2>&1
```

Expected: `touch: cannot touch '/opt/triage/db/test_write': Read-only file system`

**Step 4: Verify search works from inside container**

```bash
docker exec -u 1000:1000 daimon-sandbox bash -c "cd /opt/triage && python3 scripts/search_db.py 'gateway crash' --limit 3"
```

Expected: JSON array with 3 results containing `number`, `title`, `state`, `rank` fields.

**Step 5: Verify git bare repo works**

```bash
docker exec -u 1000:1000 daimon-sandbox git --git-dir=/opt/triage/hermes-agent.git log --oneline -3
```

Expected: 3 recent commit lines.

---

## Task 5: Restart Daimon Gateway

**Objective:** The gateway must be restarted to pick up the new `docker_volumes` and `docker_env` from config.yaml.

**Step 1: Restart the gateway service**

```bash
sudo systemctl restart hermes-gateway-daimon.service
```

**Step 2: Verify it's running**

```bash
sudo systemctl status hermes-gateway-daimon.service | head -15
```

Expected: "active (running)"

**Step 3: Check logs for errors**

```bash
journalctl -u hermes-gateway-daimon.service --since "1 min ago" --no-pager | tail -20
```

Expected: No errors, "Connected to Discord" or similar.

---

## Task 6: E2E Test — Verify Daimon Uses Triage DB

**Objective:** Post a support question to Discord that should trigger triage DB usage, verify the bot responds with triage context.

**Test script:** `/tmp/test_triage_e2e.py`

```python
#!/usr/bin/env python3
# /// script
# dependencies = [
#   "requests<3",
#   "rich",
# ]
# ///
"""E2E test: Daimon bot uses triage DB when answering support questions."""

import sys
import time
from pathlib import Path

import requests
from rich.console import Console

# Constants
GUILD_ID = "1053877538025386074"
FORUM_CHANNEL_ID = "1502698924786450632"
DAIMON_BOT_ID = "1498667414873968700"
API_BASE = "https://discord.com/api/v9"
TOKEN_PATH = Path("/home/daimon/.hermes/profiles/daimon/credentials/discord_user_token")

console = Console()


def load_token() -> str:
    return TOKEN_PATH.read_text().strip()


def headers(token: str) -> dict:
    return {
        "Authorization": token,
        "Content-Type": "application/json",
        "User-Agent": "DiscordBot (daimon-e2e-test, 1.0)",
    }


def create_thread(token: str, title: str, message: str) -> str:
    """Create a forum post mentioning Daimon, return thread_id."""
    payload = {
        "name": title,
        "message": {"content": f"<@{DAIMON_BOT_ID}> {message}"},
        "auto_archive_duration": 1440,
    }
    resp = requests.post(
        f"{API_BASE}/channels/{FORUM_CHANNEL_ID}/threads",
        headers=headers(token),
        json=payload,
    )
    resp.raise_for_status()
    thread_id = resp.json()["id"]
    console.print(f"[green]✓[/green] Created thread: {thread_id}")
    return thread_id


def wait_for_bot_reply(token: str, thread_id: str, timeout: int = 180) -> str | None:
    """Poll thread until Daimon replies. Returns bot message content or None."""
    console.print(f"[dim]Waiting up to {timeout}s for Daimon response...[/dim]")
    start = time.time()
    seen_ids = set()

    while time.time() - start < timeout:
        resp = requests.get(
            f"{API_BASE}/channels/{thread_id}/messages?limit=20",
            headers=headers(token),
        )
        if resp.ok:
            for msg in resp.json():
                msg_id = msg["id"]
                author_id = msg.get("author", {}).get("id", "")
                if author_id == DAIMON_BOT_ID and msg_id not in seen_ids:
                    content = msg.get("content", "")
                    if content:
                        elapsed = int(time.time() - start)
                        console.print(f"[green]✓[/green] Bot replied after {elapsed}s ({len(content)} chars)")
                        return content
                    seen_ids.add(msg_id)
        time.sleep(5)

    console.print("[red]✗[/red] Timeout — no bot reply")
    return None


def archive_thread(token: str, thread_id: str):
    """Archive the test thread to clean up."""
    requests.patch(
        f"{API_BASE}/channels/{thread_id}",
        headers=headers(token),
        json={"archived": True},
    )
    console.print(f"[dim]Archived thread {thread_id}[/dim]")


def test_triage_db_search():
    """Test: Ask about a known issue pattern and verify triage DB was consulted."""
    token = load_token()

    title = "[E2E Test] Triage DB Access Check"
    message = (
        "Is there a known issue about the gateway crashing when Telegram "
        "messages contain unicode? I want to know if this was already triaged "
        "and what the resolution was. Search the triage database."
    )

    thread_id = create_thread(token, title, message)

    try:
        reply = wait_for_bot_reply(token, thread_id, timeout=180)

        if reply is None:
            console.print("[red]FAIL:[/red] No response from Daimon")
            return False

        # Check for evidence of triage DB usage
        indicators = [
            "#",           # Issue number references like #12345
            "triage",      # Mentions triage
            "search_db",   # Shows the command it ran
            "duplicate",   # Triage terminology
            "P0", "P1", "P2", "P3",  # Priority labels
            "comp/",       # Component labels
        ]

        found = [i for i in indicators if i.lower() in reply.lower()]

        if len(found) >= 2:
            console.print(f"[green]PASS:[/green] Bot used triage DB (indicators: {found})")
            return True
        else:
            console.print(f"[yellow]WEAK:[/yellow] Bot replied but few triage indicators: {found}")
            console.print(f"[dim]Reply preview: {reply[:300]}...[/dim]")
            return False

    finally:
        archive_thread(token, thread_id)


def test_hermes_source_access():
    """Test: Ask agent to look at source code via the bare repo."""
    token = load_token()

    title = "[E2E Test] Source Code Access Check"
    message = (
        "Can you show me the first 10 lines of gateway/run.py from the hermes-agent "
        "source? Use git show on the bare repo at /opt/triage/hermes-agent.git"
    )

    thread_id = create_thread(token, title, message)

    try:
        reply = wait_for_bot_reply(token, thread_id, timeout=120)

        if reply is None:
            console.print("[red]FAIL:[/red] No response from Daimon")
            return False

        # Check for evidence of source code access
        if "import" in reply or "def " in reply or "class " in reply:
            console.print("[green]PASS:[/green] Bot showed source code from bare repo")
            return True
        else:
            console.print(f"[yellow]WEAK:[/yellow] Reply doesn't contain obvious code")
            console.print(f"[dim]Reply preview: {reply[:300]}...[/dim]")
            return False

    finally:
        archive_thread(token, thread_id)


if __name__ == "__main__":
    console.print("[bold]Daimon E2E Test: Triage DB Access[/bold]\n")

    results = []

    console.print("[bold cyan]Test 1: Triage DB Search[/bold cyan]")
    results.append(("triage_db_search", test_triage_db_search()))

    console.print("\n[bold cyan]Test 2: Source Code Access[/bold cyan]")
    results.append(("source_access", test_hermes_source_access()))

    console.print("\n[bold]Results:[/bold]")
    all_pass = True
    for name, passed in results:
        status = "[green]PASS[/green]" if passed else "[red]FAIL[/red]"
        console.print(f"  {status} {name}")
        if not passed:
            all_pass = False

    sys.exit(0 if all_pass else 1)
```

**Step 1: Run the E2E test**

```bash
uv run /tmp/test_triage_e2e.py
```

Expected output:
```
Daimon E2E Test: Triage DB Access

Test 1: Triage DB Search
✓ Created thread: <id>
Waiting up to 180s for Daimon response...
✓ Bot replied after ~30s (N chars)
PASS: Bot used triage DB (indicators: ['#', 'P2', ...])
Archived thread <id>

Test 2: Source Code Access
✓ Created thread: <id>
Waiting up to 120s for Daimon response...
✓ Bot replied after ~20s (N chars)
PASS: Bot showed source code from bare repo
Archived thread <id>

Results:
  PASS triage_db_search
  PASS source_access
```

**Step 2: If tests fail, debug**

- Check `docker exec daimon-sandbox env | grep TRIAGE` — is env var set?
- Check `docker exec daimon-sandbox ls /opt/triage/db/triage.db` — is mount visible?
- Check gateway logs: `journalctl -u hermes-gateway-daimon.service --since "5 min ago" | grep -i "volume\|mount\|triage"`
- Manually test the agent's terminal: start a new thread and ask it to run `ls /opt/triage/`

---

## Pitfalls

1. **SQLite WAL mode + read-only mount** — SQLite needs to read the `-wal` and `-shm` files alongside the main DB. Since we mount the entire `db/` directory (not just `triage.db`), all three files are visible. Read-only connections (`?mode=ro`) work fine with WAL as long as `-shm` is accessible.

2. **Stale `-shm` file** — If the writer (cron agent on host) crashes without closing the DB, the `-shm` file may have stale data. SQLite handles this gracefully for read-only connections — it reconstructs from the WAL.

3. **Container user permissions** — The container runs as `1000:1000` (agent user). The triage DB on host is owned by `daimon` (also uid 1000). Read-only mount means even if permissions match, writes are blocked at the filesystem level.

4. **`uv run` vs `python3` inside container** — The triage scripts have zero Python dependencies (stdlib only: `sqlite3`, `json`, `argparse`, `pathlib`). They use `# /// script` headers for `uv run` on the host, but inside the container `python3 scripts/search_db.py` works directly. The system prompt should use `python3` to avoid needing `uv` in the container.

5. **`migrate.py` import** — `search_db.py` does `from migrate import get_connection` (relative import from same dir). Running with `cd /opt/triage && python3 scripts/search_db.py` ensures the import works because Python adds the script's directory to `sys.path`.

6. **Large query results** — FTS5 searches can return verbose JSON. The default `--limit 10` is fine. The system prompt recommends `--limit 3` or `--limit 5` to keep agent context lean.

7. **Docker compose vs runtime config** — The `docker-compose.yml` mounts are for when you `docker compose up` the sandbox directly. The per-session containers spawned by the Hermes Docker terminal backend use `docker_volumes` from config.yaml. **Both** need to be updated for full coverage.

8. **hermes-agent source at `/opt/hermes-agent`** — The named volume `hermes-repo` was creating an empty directory. Replacing it with a host bind-mount fixes this AND gives the agent access to browse source. The repo-sync timer (`daimon-repo-sync.timer`) should be updated or disabled since a live bind-mount doesn't need periodic `git pull` inside the container.
