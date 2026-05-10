# Spec: Punctuation-Based Message Windowing for Daimon

> **Status:** Approved  
> **Date:** 2026-05-10  
> **Scope:** gateway/platforms/discord.py + gateway/daimon/ modules  
> **Branch:** hermes/hermes-4fa48a27 (worktree)  
> **Depends on:** Role-based tier resolution (completed)

---

## 1. Goal

Change Daimon's message handling from "free-response after first @mention" to **@mention-only triggering with punctuation-based windowing**. Messages accumulate silently between @mentions. Each @mention flushes the buffer and triggers a response with full context of what was said since the last response.

---

## 2. User Flow

```
Forum thread:
  [user A] hey what's the issue with X         ← no @mention, buffered silently
  [user B] I think it's related to Y           ← no @mention, buffered silently
  [user A] @daimon can you look into this?     ← PUNCTUATION: flush + trigger

  → daimon receives:
    [Messages since last response]
    user A: hey what's the issue with X
    user B: I think it's related to Y
    [Current request:]
    user A: @daimon can you look into this?
  → daimon responds (turn 1)

  [user A] oh wait, also check Z               ← buffered
  [user A] and W too                            ← buffered
  [user A] @daimon ^^                           ← PUNCTUATION: flush + trigger

  → daimon receives:
    [Messages since last response]
    user A: oh wait, also check Z
    user A: and W too
    [Current request:]
    user A: @daimon ^^
  → daimon responds (turn 2)

  [user C] random chatter                      ← buffered
  [user A] @daimon what was that error again?   ← PUNCTUATION: flush + trigger (empty buffer + trigger msg)

  → daimon receives:
    [Messages since last response]
    user C: random chatter
    [Current request:]
    user A: @daimon what was that error again?
  → daimon responds (turn 3)
```

---

## 3. Key Decisions

- **@mention required on EVERY interaction** — no free-response mode, even for thread creators
- **Trigger message included in the flush** — it's the user's current request
- **ALL users' messages buffered** — not just thread creator (supports multi-user discussions)
- **Buffer cap: 50 messages** (ring buffer, oldest evicted) — no TTL
- **Gateway restart fallback:** If buffer is empty (fresh start), fetch via Discord `channel.history()` API (same primitive as current first-mention history fetch)
- **Turn counter counts agent RESPONSES** — one @mention = one turn regardless of buffer size
- **Admins exempt from turn cap** (unchanged)

---

## 4. Architecture

```
on_message(msg):
  if @daimon in msg.mentions:
    # PUNCTUATION EVENT — flush + trigger
    buffered = window_buffer.flush(thread_id)  # returns list, clears buffer
    context = format_window(buffered, trigger_msg=msg)
    dispatch_to_agent(context)
    increment_turn_counter(thread_id)
  else:
    # ACCUMULATE — silent buffering
    window_buffer.append(thread_id, msg)
```

### 4.1 WindowBuffer class

```python
@dataclass
class BufferedMessage:
    author_name: str
    author_id: str
    content: str
    timestamp: datetime

class WindowBuffer:
    """Per-thread ring buffer accumulating messages between @mentions."""
    
    MAX_PER_THREAD = 50
    MAX_THREADS = 5000  # memory bound
    
    def __init__(self):
        self._buffers: dict[str, deque[BufferedMessage]] = {}
    
    def append(self, thread_id: str, msg: BufferedMessage) -> None:
        """Add a message to the buffer. Evicts oldest if at cap."""
        buf = self._buffers.setdefault(thread_id, deque(maxlen=self.MAX_PER_THREAD))
        buf.append(msg)
        # Evict oldest threads if too many tracked
        if len(self._buffers) > self.MAX_THREADS:
            self._evict_oldest_thread()
    
    def flush(self, thread_id: str) -> list[BufferedMessage]:
        """Return all buffered messages and clear the buffer."""
        buf = self._buffers.pop(thread_id, deque())
        return list(buf)
    
    def clear(self, thread_id: str) -> None:
        """Remove buffer for a thread (cleanup on close)."""
        self._buffers.pop(thread_id, None)
```

### 4.2 Context formatting

```python
def format_window_context(buffered: list[BufferedMessage], trigger_msg) -> str:
    """Format buffered messages + trigger into agent context string."""
    if not buffered:
        # No buffer — just the trigger message (first mention or empty window)
        return ""  # trigger message is already the event text
    
    parts = ["[Messages since last response]"]
    for msg in buffered:
        parts.append(f"{msg.author_name}: {msg.content}")
    parts.append("[Current request:]")
    return "\n".join(parts) + "\n\n"
    # Prepended to the trigger message's event_text
```

### 4.3 Gateway restart / empty buffer fallback

When `flush()` returns empty AND `_threads` doesn't have this thread_id marked (first interaction after restart):
- Fall back to `channel.history(limit=50, before=trigger_message)` 
- Filter to messages after daimon's last response in that thread (or all if never responded)
- This reuses the SAME formatting as the window flush

This replaces the existing hardcoded 20-message history fetch with the unified windowing primitive.

---

## 5. Changes to Existing Behavior

| Before | After |
|--------|-------|
| First @mention → fetch 20 prior messages | First @mention → flush window buffer (or API fallback if empty) |
| Subsequent messages → free-response (no @mention needed) | Subsequent messages → buffered silently until next @mention |
| `_threads.mark()` enables free-response | `_threads.mark()` only used for tracking "has daimon responded here before" (no free-response) |
| Thread history fetch: 20 messages, first mention only | Window context: up to 50 messages, every mention |
| Turn counter: counted on `should_process_message` | Turn counter: counted on agent response delivery |

---

## 6. Implementation Plan

| # | Task | Files | Notes |
|---|------|-------|-------|
| 1 | Create `WindowBuffer` class + `BufferedMessage` dataclass | `gateway/daimon/window_buffer.py` (new file) | Ring buffer with deque(maxlen=50), thread eviction |
| 2 | Add `WindowBuffer` to `DaimonDiscordHooks` | `gateway/daimon/discord_hooks.py` | Instantiate on init, expose `buffer_message()` and `flush_window()` |
| 3 | Disable free-response: enforce @mention on EVERY message in Daimon threads | `gateway/platforms/discord.py` ~line 4145 | When Daimon active, don't bypass require_mention for tracked threads |
| 4 | Buffer non-mention messages | `gateway/platforms/discord.py` in `_handle_message()` | Before the early return on "no mention", buffer the message into WindowBuffer |
| 5 | On @mention: flush buffer + format context | `gateway/platforms/discord.py` ~line 4370 | Replace existing history fetch with: flush window → format → prepend to event_text. API fallback if buffer empty + first interaction |
| 6 | Move turn counter to count responses (not triggers) | `gateway/daimon/session_manager.py` + `gateway/platforms/discord.py` | Increment AFTER agent response is delivered, not in should_process_message |
| 7 | Add `max_buffer_per_thread` config field | `gateway/daimon/config.py` | Default 50, configurable |
| 8 | Cleanup: buffer.clear() on thread close/archive | `gateway/daimon/session_manager.py` | In end_session() |

### Review Protocol

Same as role-based tier: adversarial review via Claude Code after each task.

---

## 7. Config Addition

```yaml
gateway:
  discord:
    daimon:
      max_buffer_per_thread: 50    # messages accumulated between @mentions
      max_turns_per_thread: 20     # now counts agent RESPONSES, not triggers
```

---

## 8. Edge Cases

| Case | Handling |
|------|----------|
| @mention with empty buffer (first msg or rapid re-mentions) | Just the trigger message, no context prefix |
| Gateway restart mid-thread | Buffer lost → API fallback fetches from channel.history() |
| Thread with 500 messages between mentions | Only last 50 kept (ring buffer eviction) |
| Multiple users @mention simultaneously | Each flush is independent — first one gets the buffer, second gets empty buffer |
| Bot message in buffer | Skip bot's own messages during buffering (don't buffer self) |
| Message with attachments | Buffer the text content; attachment URLs included as-is |
| Edited messages | Not tracked — buffer captures original content at time of receipt |
| Deleted messages | Stay in buffer (we don't get delete events retroactively) |
