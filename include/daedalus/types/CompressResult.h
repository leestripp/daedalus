/**
 * @file daedalus/types/CompressResult.h
 * @brief Outcome of Agent::compress_context() (§COMPRESS-RESULT,
 *        2026-09-10, MonkeyByte upstream report REQ-3).
 *
 * Previously compress_context() was void and hosts had to duplicate
 * internal thresholds to tell the user whether anything happened.
 */
#pragma once

namespace daedalus {

enum class CompressResult {
    Compressed,        ///< Context was reduced (summary, or fallback delete).
    Deferred,          ///< A turn is running; compression will happen at the
                       ///< current/next cycle checkpoint.
    NothingToCompress, ///< History too small — nothing happened. The request
                       ///< is carried over to the next turn's first cycle.
    NoModel,           ///< No model configured (no LLMClient yet); no-op.
    Failed,            ///< Summary failed AND history could not be safely
                       ///< reduced. Also carried over to the next turn.
};

/// @brief Human-readable name (e.g. "Compressed"). Never returns nullptr.
inline const char* to_string(CompressResult r) noexcept {
    switch (r) {
        case CompressResult::Compressed:        return "Compressed";
        case CompressResult::Deferred:          return "Deferred";
        case CompressResult::NothingToCompress: return "NothingToCompress";
        case CompressResult::NoModel:           return "NoModel";
        case CompressResult::Failed:            return "Failed";
    }
    return "Unknown";
}

}  // namespace daedalus
