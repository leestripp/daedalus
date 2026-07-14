/**
 * @file daedalus/ModelInfo.h
 * @brief Read-only description of a model configured in settings.json.
 */
#pragma once

#include <string>

namespace daedalus {

struct ModelInfo {
    std::string name;             ///< Key in settings.json (e.g. "minimax")
    std::string model_id;         ///< Model id sent to the API
    std::string host;             ///< API host
    std::string chat_endpoint;    ///< e.g. "/v1/chat/completions"
    int max_context_size = 0;     ///< Effective max (resolved with fallback)
    double temperature = 0.0;     ///< Sampling temperature (from model or session override)
};

}  // namespace daedalus