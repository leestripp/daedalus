/**
 * @file daedalus/types/DiscoveredModel.h
 * @brief Read-only description of a model returned by a provider's
 *        model-discovery endpoint.
 */
#pragma once

#include <cstdint>
#include <string>

namespace daedalus {

struct DiscoveredModel {
    std::string id;        ///< Model identifier sent in chat requests.
    std::string owned_by;  ///< Provider or organization that owns the model.
    int64_t created = 0;   ///< Unix timestamp (seconds) when the model was created.
};

}  // namespace daedalus
