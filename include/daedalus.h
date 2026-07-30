/**
 * @file daedalus.h
 * @brief Umbrella include: pulls in the entire stable public API.
 *
 *   #include <daedalus.h>
 *
 * Equivalent to:
 *
 *   #include <daedalus/Agent.h>
 */
#pragma once

#include "daedalus/Agent.h"
#include "daedalus/ProviderPlugin.h"    // §ProviderPlugins
#include "daedalus/ProviderRegistry.h"  // §ProviderPlugins
#include "daedalus/model_discovery.h"
#include "daedalus/a2a/A2aClient.h"
