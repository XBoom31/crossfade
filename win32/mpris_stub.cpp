// Windows stub for the MPRIS D-Bus integration. MPRIS is a Linux desktop
// protocol; a future Windows version would map these calls onto the Media
// Session API (winrt::SystemMediaTransportControls). For now the surface is
// a no-op so the rest of the app compiles and runs unchanged.
#include "../src/playback/mpris.h"

struct MprisPlayer::Impl
{
};

MprisPlayer::MprisPlayer(AppState &, MpvPlayer *&)
    : pImpl(std::make_unique<Impl>())
{
}

MprisPlayer::~MprisPlayer() = default;

void MprisPlayer::update_metadata(const Track &, double) {}
void MprisPlayer::update_playback_status(bool) {}
void MprisPlayer::update_position(double) {}
void MprisPlayer::update_volume(double) {}
