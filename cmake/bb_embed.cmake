# bb_embed.cmake — FORWARDING SHIM (Commit D).
#
# The embed functions (bb_embed_assets, bb_embed_site) now live in
# cmake/bbtool.cmake. This file is kept so that consumers still pinned to a
# breadboard version that used include(bb_embed.cmake) — most notably
# TaipanMiner's components/webui/CMakeLists.txt — continue to work unchanged.
#
# DO NOT add new logic here. Migrate callers to include bbtool.cmake directly.
# This shim will be removed in PR-E once consumers migrate.

include("${CMAKE_CURRENT_LIST_DIR}/bbtool.cmake")
