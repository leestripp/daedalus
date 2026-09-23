# ============================================================================
# FetchOmniNet.cmake — single source of truth for the OmniNet declaration.
#
# §Monorepo (2026-09-24): OmniNet is shared by every component in the
# monorepo (daedalus links it PUBLIC; icarus/monkeybyte reach it
# transitively). This fragment is included from BOTH:
#   - the superbuild root CMakeLists.txt (declared before any
#     add_subdirectory, so the whole tree shares ONE declaration/fetch), and
#   - daedalus/CMakeLists.txt (so `cmake -S daedalus` standalone builds and
#     FetchContent consumers of the library still work with no root file).
# FetchContent "first declaration wins" semantics make the second include a
# no-op; FetchContent_MakeAvailable(OmniNet) then populates from whichever
# declaration was recorded first. Any change here applies to both paths.
#
# NOTE: this file only DECLARES. Population (and the OMNINET_LOCAL override
# that bypasses fetching entirely) stays in daedalus/CMakeLists.txt, which
# also owns the post-fetch source patches (§TASK-006).
# ============================================================================
include(FetchContent)

FetchContent_Declare(
    OmniNet
    GIT_REPOSITORY https://gitlab.com/leestripp/omninet.git
    GIT_TAG        main
    # NOT shallow: OmniNet's CMakeLists detects its version via
    # `git describe --tags --abbrev=0`. A shallow clone only carries
    # the single branch tip and has no tag history, so the lookup
    # fails and falls back to FALLBACK_VERSION ("0.1.0"). A full
    # clone keeps all tags, so e.g. HEAD just past v0.8.2 reports
    # `0.8.2` correctly instead of the misleading `0.1.0`.
)
