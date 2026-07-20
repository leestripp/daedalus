/**
 * @file daedalus/types/HostTool.h
 * @brief Deprecated alias for daedalus::BaseTool.
 *
 * @deprecated Use `daedalus::BaseTool` instead. This header and name are
 *             kept for backward compatibility and will be removed in a
 *             future release.
 */
#pragma once

#include "daedalus/types/BaseTool.h"

namespace daedalus {

using HostTool [[deprecated("Use daedalus::BaseTool instead")]] = BaseTool;

}  // namespace daedalus
