# OmniNet Bugs

Bugs found in OmniNet (vendored under `build/_deps/omninet-src/`,
fetched from https://gitlab.com/leestripp/omninet). Each entry is filed
with the upstream URL once a fix lands. Until then, this file is the
working list so the daedalus team knows what to work around.

Only **open** bugs live here. Fixed bugs are not re-listed — see git
history or the OmniNet release notes.

---

## OMNINET-2: `WebSocket::Connection` has virtual methods but non-virtual destructor

**Severity:** Med (latent UB — no production subclass today, but undefined
behaviour the moment anyone subclasses `Connection`)

**Discovered:** 2026-07-13 by clang++'s `-Wdelete-non-abstract-non-virtual-dtor`
during Phase 13 build audit.

### Symptom

clang++ emits the following warning on every daedalus translation unit that
includes `NetworkTool.cpp`:

```
delete called on non-final
  'OmniNet::WebSocket::Connection' that has virtual functions but
  non-virtual destructor [-Wdelete-non-abstract-non-virtual-dtor]
```

Code location: `src/tools/NetworkTool.cpp:1032` — `Connection` is held in a
`std::unique_ptr<OmniNet::WebSocket::Connection>` and `delete`d through the
base pointer when the holder goes out of scope. If any subclass adds
non-trivial members, only `~Connection()` runs; the derived destructor is
skipped → undefined behaviour (member leaks, partial destruction, etc.).

### Root cause

`build/_deps/omninet-src/src/websocket/WebSocket.h:44` — `~Connection()`
is declared as a non-virtual destructor, but the class declares 6 virtual
methods (`sendText`, `sendBinary`, `sendPing`, `sendPong`, `close`,
`isOpen`) added in OmniNet 0.7.0 so tests can substitute `MockConnection`.

The comment at line 52 even acknowledges the design intent, but the
actual declaration has no `virtual` keyword, so polymorphic delete
through `Connection*` is UB.

### Reproduction

```cpp
#include "websocket/WebSocket.h"

struct MockConnection : public OmniNet::WebSocket::Connection {
    std::unique_ptr<int> payload_;  // wants cleanup on destruction
    bool sendText(std::string_view) override { return true; }
};

std::unique_ptr<OmniNet::WebSocket::Connection> p =
    std::make_unique<MockConnection>();
// p.reset() — UB: ~Connection() runs, ~MockConnection() skipped,
// payload_'s destructor is never called.
```

### Expected

`~Connection()` should be `virtual` (and ideally `= default;`). Once
virtual, `MockConnection`'s destructor is invoked through the base
pointer as expected.

### Suggested upstream fix

One-line change in `WebSocket.h:44`:

```cpp
-    ~Connection() = default;
+    virtual ~Connection() = default;
```

Plus `final` on the class if no further subclassing is intended (it would
make clang's `-Wdelete-non-virtual-dtor` warning go away entirely).

### Workaround in daedalus

Three options documented in Agent.md §13:

| Option | Effort | Pros | Cons |
|---|---|---|---|
| **A** Upstream a fix to OmniNet | 1 d | Cleanest; all consumers benefit | Requires a PR to the OmniNet repo; review time unknown |
| **B** Patch OmniNet in-tree | 1 h | Local; fast | Breaks on `cmake` re-download |
| **C** Wrapper class in `NetworkTool.cpp` | 1 h | Local; no upstream dep | Cumbersome; only fixes our call site |

**Recommendation:** A if we control OmniNet (which we do — same author),
otherwise C. Option B is fragile.

### Status

- [x] Documented here
- [ ] Filed upstream (PR pending — Lee)
- [ ] Fix released
- [ ] Worked around in daedalus (none needed yet — production code
      doesn't subclass `Connection`; only `MockConnection` does,
      in OmniNet's own tests, where it's stack-allocated)

---

## Template for new entries

```
## OMNINET-N: <title>

**Severity:** Low / Med / High / Critical
**Discovered:** YYYY-MM-DD

### Symptom
<what the user sees>

### Root cause
<file:line — the buggy code, with context>

### Reproduction
<minimal repro>

### Expected
<correct behaviour>

### Suggested fix
<patch or approach>

### Workaround in daedalus
<what we do to keep going>

### Status
- [ ] Filed upstream
- [x] Documented here
- [ ] Worked around
```