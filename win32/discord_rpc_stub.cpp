// Windows stub for Discord Rich Presence. The Linux implementation speaks
// the Discord IPC protocol over a Unix socket; on Windows Discord exposes
// the same protocol over named pipes (\\.\pipe\discord-ipc-N). Until that
// transport is ported, the integration is a silent no-op.
#include "../discord_rpc.h"

void discord_rpc_start(const std::string &) {}
void discord_rpc_stop() {}
void discord_rpc_update(const DiscordPresence &) {}
