// SPDX-License-Identifier: BSL-1.0
// include/daedalus/vault.h
//
// §TASK-2026-09-21-01: SecretsVault — daedalus's password manager.
//
// Encrypted-at-rest storage for logins, secure notes, API keys and other
// secrets. GLOBAL by design: SecretsVault::global() always targets the
// process-wide SettingsStore DB + the user-global key file, regardless of
// any agent's per-project daedalus_home, so secrets are reachable from
// every project.
//
// Security model
// --------------
//   - AES-256-GCM (OpenSSL EVP — already a public dependency, no new deps).
//   - Random 12-byte nonce per entry; 16-byte tag; AAD = entry name, so a
//     ciphertext cannot be transplanted onto another row.
//   - Master key: 32 random bytes in `<user_config_root>/vault.key`,
//     mode 0600, generated on first use. Unlock is non-interactive (the
//     agent must be able to read secrets unattended); the security
//     boundary is filesystem permissions, exactly like ~/.ssh keys.
//     Passphrase-wrapped keys are deliberately future work.
//   - Metadata (name/kind/description/timestamps) is plaintext so
//     list() is useful without decryption; ONLY the secret fields blob
//     is encrypted.
//
// Thread-safety: all methods are safe to call from multiple threads
// (storage is SettingsStore-serialised; key load is internally locked).

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "daedalus/settings.h"

namespace daedalus {

/// Decrypted vault entry as hosts/agents see it.
struct VaultEntry
{
    std::string name;                    // unique key, e.g. "github/api-token"
    std::string kind = "custom";         // login | note | api_key | custom
    std::string description;             // plaintext, shown by list()
    /// Secret fields (encrypted at rest). Conventional keys per kind:
    ///   login:   username, password, url
    ///   note:    text
    ///   api_key: key, service
    ///   custom:  anything.
    std::map<std::string, std::string> fields;
    std::string created_at;              // ISO-8601 UTC (filled by store())
    std::string updated_at;              // ISO-8601 UTC (filled by store())
};

/// Metadata-only view returned by list() — never carries secret fields.
struct VaultEntryMeta
{
    std::string name;
    std::string kind;
    std::string description;
    std::string created_at;
    std::string updated_at;
};

class SecretsVault
{
public:
    /// The user-global vault: global SettingsStore DB +
    /// `Paths::global().user_config_root() / "vault.key"`. This is the
    /// instance the agent's `vault` tool uses — per-project agents all
    /// share it.
    static SecretsVault global();

    /// Explicit store + key file (tests, per-DB tooling).
    SecretsVault(SettingsStore store, std::filesystem::path key_file);

    /// Insert or replace an entry. `created_at` is preserved on update;
    /// `updated_at` is always refreshed. Throws std::runtime_error on
    /// crypto/IO failure.
    void store(const VaultEntry& entry);

    /// Fetch and decrypt one entry. std::nullopt when the name is
    /// unknown. Throws std::runtime_error on tamper/wrong key (GCM tag
    /// mismatch) or IO failure.
    [[nodiscard]] std::optional<VaultEntry> get(std::string_view name) const;

    /// Metadata for every entry, ordered by name. Never decrypts —
    /// the result contains no secret material.
    [[nodiscard]] std::vector<VaultEntryMeta> list() const;

    /// Partially edit an existing entry — merge `fields` into the stored
    /// field set (unspecified fields are kept, so rotating one field does
    /// NOT require reading the rest into context first). `kind` /
    /// `description` are updated only when non-empty. Returns false when
    /// no such entry exists (call store() to create). Throws
    /// std::runtime_error on crypto/IO failure.
    bool edit(std::string_view name,
              const std::map<std::string, std::string>& fields,
              std::string_view kind = {},
              std::string_view description = {});

    /// Delete an entry (unknown name = no-op).
    void remove(std::string_view name);

    [[nodiscard]] bool exists(std::string_view name) const;

    /// Path of the key file in use (diagnostics).
    [[nodiscard]] const std::filesystem::path& key_file() const {
        return key_file_;
    }

private:
    /// Load (or generate on first use) the 32-byte master key.
    /// Cached; guarded by key_mutex_.
    const std::string& master_key() const;

    SettingsStore         store_;
    std::filesystem::path key_file_;
    mutable std::mutex    key_mutex_;
    mutable std::string   key_cache_;   // 32 raw bytes once loaded
};

} // namespace daedalus
