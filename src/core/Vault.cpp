// SPDX-License-Identifier: BSL-1.0
// src/core/Vault.cpp — §TASK-2026-09-21-01: SecretsVault implementation.
//
// AES-256-GCM via OpenSSL EVP. Layout of the stored blob:
//   nonce (12 bytes, random per entry) ‖ ciphertext ‖ GCM tag (16 bytes)
// base64'd into the vault_entries.nonce_b64 / payload_b64 TEXT columns
// (nonce kept as its own column for debuggability; it is also the prefix
// of no other field). AAD = entry name.

#include "daedalus/vault.h"
#include "daedalus/paths.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

namespace daedalus {

namespace {

constexpr size_t kKeyBytes   = 32;  // AES-256
constexpr size_t kNonceBytes = 12;  // GCM standard
constexpr size_t kTagBytes   = 16;

// ---------------------------------------------------------------------------
// base64 helpers (OpenSSL EVP; no padding-of-error tolerance — our own data)
// ---------------------------------------------------------------------------

std::string b64_encode(const unsigned char* data, size_t len) {
    std::string out(4 * ((len + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  data, static_cast<int>(len));
    if (n < 0) throw std::runtime_error("vault: base64 encode failed");
    out.resize(static_cast<size_t>(n));
    return out;
}

std::string b64_decode(const std::string& in) {
    std::string out(3 * (in.size() / 4) + 3, '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(in.data()),
                                  static_cast<int>(in.size()));
    if (n < 0) throw std::runtime_error("vault: base64 decode failed");
    size_t len = static_cast<size_t>(n);
    // EVP_DecodeBlock counts padding bytes; strip them.
    if (!in.empty() && in.back() == '=') --len;
    if (in.size() > 1 && in[in.size() - 2] == '=') --len;
    out.resize(len);
    return out;
}

// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

void openssl_check(int ok, const char* what) {
    if (ok != 1) throw std::runtime_error(std::string("vault: OpenSSL ") + what + " failed");
}

/// Encrypt `plaintext` under `key` with a fresh random nonce.
/// Returns nonce ‖ ciphertext ‖ tag. AAD binds the blob to `aad`.
std::string gcm_encrypt(const std::string& key, const std::string& aad,
                        const std::string& plaintext) {
    std::string nonce(kNonceBytes, '\0');
    openssl_check(RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()),
                             static_cast<int>(nonce.size())), "RAND_bytes");

    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) throw std::runtime_error("vault: EVP_CIPHER_CTX_new failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(raw, EVP_CIPHER_CTX_free);

    openssl_check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EncryptInit");
    openssl_check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceBytes), nullptr), "SET_IVLEN");
    openssl_check(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
        reinterpret_cast<const unsigned char*>(key.data()),
        reinterpret_cast<const unsigned char*>(nonce.data())), "EncryptInit key");
    int len = 0;
    openssl_check(EVP_EncryptUpdate(ctx.get(), nullptr, &len,
        reinterpret_cast<const unsigned char*>(aad.data()),
        static_cast<int>(aad.size())), "AAD");

    std::string ciphertext(plaintext.size(), '\0');
    openssl_check(EVP_EncryptUpdate(ctx.get(),
        reinterpret_cast<unsigned char*>(ciphertext.data()), &len,
        reinterpret_cast<const unsigned char*>(plaintext.data()),
        static_cast<int>(plaintext.size())), "EncryptUpdate");
    size_t total = static_cast<size_t>(len);
    openssl_check(EVP_EncryptFinal_ex(ctx.get(),
        reinterpret_cast<unsigned char*>(ciphertext.data()) + total, &len), "EncryptFinal");
    total += static_cast<size_t>(len);
    ciphertext.resize(total);

    std::string tag(kTagBytes, '\0');
    openssl_check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagBytes), tag.data()), "GET_TAG");

    return nonce + ciphertext + tag;
}

/// Inverse of gcm_encrypt. Throws on tag mismatch (tamper / wrong key).
std::string gcm_decrypt(const std::string& key, const std::string& aad,
                        const std::string& blob) {
    if (blob.size() < kNonceBytes + kTagBytes) {
        throw std::runtime_error("vault: blob too short — corrupt entry");
    }
    const std::string nonce      = blob.substr(0, kNonceBytes);
    const std::string ciphertext = blob.substr(kNonceBytes, blob.size() - kNonceBytes - kTagBytes);
    const std::string tag        = blob.substr(blob.size() - kTagBytes);

    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) throw std::runtime_error("vault: EVP_CIPHER_CTX_new failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(raw, EVP_CIPHER_CTX_free);

    openssl_check(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "DecryptInit");
    openssl_check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceBytes), nullptr), "SET_IVLEN");
    openssl_check(EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
        reinterpret_cast<const unsigned char*>(key.data()),
        reinterpret_cast<const unsigned char*>(nonce.data())), "DecryptInit key");
    int len = 0;
    openssl_check(EVP_DecryptUpdate(ctx.get(), nullptr, &len,
        reinterpret_cast<const unsigned char*>(aad.data()),
        static_cast<int>(aad.size())), "AAD");

    std::string plaintext(ciphertext.size(), '\0');
    openssl_check(EVP_DecryptUpdate(ctx.get(),
        reinterpret_cast<unsigned char*>(plaintext.data()), &len,
        reinterpret_cast<const unsigned char*>(ciphertext.data()),
        static_cast<int>(ciphertext.size())), "DecryptUpdate");
    size_t total = static_cast<size_t>(len);

    openssl_check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
        static_cast<int>(kTagBytes), const_cast<char*>(tag.data())), "SET_TAG");
    if (EVP_DecryptFinal_ex(ctx.get(),
            reinterpret_cast<unsigned char*>(plaintext.data()) + total, &len) != 1) {
        throw std::runtime_error(
            "vault: GCM tag mismatch — entry is corrupt, tampered with, "
            "or the vault.key does not match");
    }
    total += static_cast<size_t>(len);
    plaintext.resize(total);
    return plaintext;
}

std::string now_iso8601_utc() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// SecretsVault
// ---------------------------------------------------------------------------

SecretsVault SecretsVault::global() {
    return SecretsVault(SettingsStore::instance(),
                        Paths::global().user_config_root() / "vault.key");
}

SecretsVault::SecretsVault(SettingsStore store, std::filesystem::path key_file)
    : store_(std::move(store)), key_file_(std::move(key_file)) {}

const std::string& SecretsVault::master_key() const {
    std::lock_guard<std::mutex> lk(key_mutex_);
    if (!key_cache_.empty()) return key_cache_;

    // Try to read an existing key.
    {
        std::ifstream f(key_file_, std::ios::binary);
        if (f.is_open()) {
            std::string key((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
            if (key.size() != kKeyBytes) {
                throw std::runtime_error(
                    "vault: " + key_file_.string() + " has " +
                    std::to_string(key.size()) + " bytes, expected 32 — " \
                    "refusing to use a corrupt key file");
            }
            key_cache_ = std::move(key);
            return key_cache_;
        }
    }

    // First use: generate. Parent dir is the owner-only config root in
    // production; create it (owner-only) for explicit-path callers.
    std::error_code ec;
    std::filesystem::create_directories(key_file_.parent_path(), ec);
    std::string key(kKeyBytes, '\0');
    openssl_check(RAND_bytes(reinterpret_cast<unsigned char*>(key.data()),
                             static_cast<int>(key.size())), "RAND_bytes");
    {
        std::ofstream f(key_file_, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            throw std::runtime_error("vault: cannot write key file " +
                                     key_file_.string());
        }
        f.write(key.data(), static_cast<std::streamsize>(key.size()));
    }
    // 0600 — owner read/write only. Best-effort on non-POSIX.
    ::chmod(key_file_.c_str(), S_IRUSR | S_IWUSR);

    key_cache_ = std::move(key);
    return key_cache_;
}

void SecretsVault::store(const VaultEntry& entry) {
    if (entry.name.empty()) {
        throw std::runtime_error("vault: entry name must not be empty");
    }
    const nlohmann::json payload = entry.fields;
    const std::string blob = gcm_encrypt(master_key(), entry.name, payload.dump());

    SettingsStore::VaultRow row;
    row.name        = entry.name;
    row.kind        = entry.kind.empty() ? "custom" : entry.kind;
    row.description = entry.description;
    row.nonce_b64   = b64_encode(reinterpret_cast<const unsigned char*>(blob.data()), kNonceBytes);
    row.payload_b64 = b64_encode(reinterpret_cast<const unsigned char*>(blob.data()) + kNonceBytes,
                                 blob.size() - kNonceBytes);

    const auto now = now_iso8601_utc();
    const auto existing = store_.get_vault_row(entry.name);
    row.created_at = existing ? existing->created_at : now;
    row.updated_at = now;

    store_.upsert_vault_row(row);
}

std::optional<VaultEntry> SecretsVault::get(std::string_view name) const {
    const auto row = store_.get_vault_row(name);
    if (!row) return std::nullopt;

    const std::string nonce = b64_decode(row->nonce_b64);
    const std::string rest  = b64_decode(row->payload_b64);
    const std::string plain = gcm_decrypt(master_key(), row->name, nonce + rest);

    VaultEntry entry;
    entry.name        = row->name;
    entry.kind        = row->kind;
    entry.description = row->description;
    entry.created_at  = row->created_at;
    entry.updated_at  = row->updated_at;
    const auto parsed = nlohmann::json::parse(plain);
    if (!parsed.is_object()) {
        throw std::runtime_error("vault: decrypted payload is not a JSON object");
    }
    for (const auto& [k, v] : parsed.items()) {
        entry.fields[k] = v.is_string() ? v.get<std::string>() : v.dump();
    }
    return entry;
}

std::vector<VaultEntryMeta> SecretsVault::list() const {
    std::vector<VaultEntryMeta> out;
    for (const auto& r : store_.list_vault_rows()) {
        out.push_back(VaultEntryMeta{r.name, r.kind, r.description,
                                     r.created_at, r.updated_at});
    }
    return out;
}

bool SecretsVault::edit(std::string_view name,
                        const std::map<std::string, std::string>& fields,
                        std::string_view kind,
                        std::string_view description) {
    auto entry = get(name);   // decrypts (verifies tag) — nullopt if absent
    if (!entry) return false;
    for (const auto& [k, v] : fields) entry->fields[k] = v;
    if (!kind.empty())        entry->kind = std::string(kind);
    if (!description.empty()) entry->description = std::string(description);
    store(*entry);            // re-encrypt; preserves created_at
    return true;
}

void SecretsVault::remove(std::string_view name) {
    store_.delete_vault_row(name);
}

bool SecretsVault::exists(std::string_view name) const {
    return store_.get_vault_row(name).has_value();
}

} // namespace daedalus
