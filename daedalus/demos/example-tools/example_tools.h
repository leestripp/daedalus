/**
 * @file example-tools/example_tools.h
 * @brief Umbrella header for the non-core example tools.
 *
 * These tools were moved out of libdaedalus so the default tool surface
 * stays minimal. Demos that need file / git / search / system-info /
 * background-process / network tools include this header, then register
 * the individual tools they want through ToolHandler::get_tool_factory().
 *
 * There is no bulk "register everything" helper — hosts should opt in to
 * exactly the tools they need. See docs/custom-tools.md for the public
 * daedalus::BaseTool API used to write brand-new custom tools.
 */
#pragma once

#include "BackgroundTool.h"
#include "GitTool.h"
#include "NetworkTool.h"
#include "SearchTool.h"
#include "SystemInfoTool.h"
