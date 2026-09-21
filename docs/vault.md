# Secrets Vault (password manager)

§TASK-2026-09-21-01. daedalus ships a user-global, encrypted-at-rest
secrets store for logins, secure notes, API keys, and other credentials.

## What it is

- **Global**: one vault per user. `SecretsVault::global()` always uses
  the process-wide `SettingsStore` DB
  (`$XDG_DATA_HOME/daedalus/daedalus.db`, table `vault_entries`,
  schema v6) — independent of any agent's per-project `daedalus_home`,
  so every project sees the same vault.
- **Encrypted at rest**: AES-256-GCM (OpenSSL EVP). Random 12-byte
  nonce per entry, 16-byte tag, AAD = entry name (a ciphertext cannot
  be transplanted onto another row). Only the secret fields blob is
  encrypted; name/kind/description/timestamps stay plaintext so
  `list()` works without decryption.
- **Master key**: 32 random bytes in
  `$XDG_CONFIG_HOME/daedalus/vault.key` (default
  `~/.config/daedalus/vault.key`), mode 0600, generated on first use.
  Unlock is non-interactive — the security boundary is filesystem
  permissions, exactly like `~/.ssh` keys. A passphrase-wrapped key is
  future work.

## Agent usage — the `vault` tool

| Call | Result |
|------|--------|
| `vault(action="list")` | Metadata for all entries — **never** secret values. |
| `vault(action="get", name="...")` | Decrypted fields for one entry. |
| `vault(action="store", name="...", kind="api_key", description="...", fields={"key": "..."})` | Create or **replace** a whole entry. Confirms without echoing values. |
| `vault(action="edit", name="...", fields={"password": "new"})` | **Merge** fields into an existing entry — the other secrets are never read into context. `kind`/`description` can also be updated. Errors if the entry doesn't exist (use `store` to create). |
| `vault(action="delete", name="...")` | Remove an entry. |

Kinds: `login` (`username`/`password`/`url`), `note` (`text`),
`api_key` (`key`/`service`), `custom` (any string fields).

> **Note**: `get` results enter the conversation history. Hosts that
> don't want secrets in chat context can disable the tool via
> `AgentConfig::tools_disabled = {"vault"}` and drive the C++ API
> directly.

## Host usage — C++ API

```cpp
#include <daedalus/vault.h>

auto vault = daedalus::SecretsVault::global();   // user-global

daedalus::VaultEntry e;
e.name   = "github/api-token";
e.kind   = "api_key";
e.fields = {{"key", "ghp_..."}, {"service", "github.com"}};
vault.store(e);

for (const auto& m : vault.list()) { /* metadata only */ }
auto entry = vault.get("github/api-token");      // optional<VaultEntry>
vault.edit("github/api-token", {{"key", "ghp_rotated"}});  // merge, false if absent
vault.remove("github/api-token");
```

Errors (tamper detected, wrong/missing key, IO failure) throw
`std::runtime_error`; the agent tool converts them to tool errors.

Thread-safe; tests inject `SecretsVault(SettingsStore::for_home(tmp),
tmp/"vault.key")`.
