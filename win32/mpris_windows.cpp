// Windows implementation of the media-controls surface that Linux provides
// through MPRIS. Every call is mapped onto the System Media Transport Controls
// session owned by WinMediaSession, so the Windows media flyout, hardware
// media keys, lock screen and "now playing" overlay control the app.
#include "../src/playback/mpris.h"
#include "media_session.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{

// SMTC asks for repeat/shuffle state; the app keeps those in Settings.
int smtc_repeat_mode(const Settings &settings)
{
    return settings.repeat_mode;
}

} // namespace

struct MprisPlayer::Impl
{
    Impl(AppState &state, MpvPlayer *&player, void *window)
        : m_state(state), m_player(player), m_session(window)
    {
        m_session.set_command_handler([this](MediaCommand command, double argument)
                                      { on_command(command, argument); });
    }

    void update_metadata(const Track &track, double duration_sec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current.track_id = track.id;
        m_current.title = track.title;
        m_current.artist = track.artist;
        m_current.album = track.album;
        m_current.art = track.thumbnail_url;
        if (duration_sec > 0.0)
            m_current.duration_sec = duration_sec;
        else if (track.duration_sec > 0)
            m_current.duration_sec = (double)track.duration_sec;
        else if (MpvPlayer *player = m_player)
        {
            // The "start" event can fire before mpv has parsed the duration;
            // pick it up from the cached observer instead of leaving the
            // timeline empty for the whole track.
            const double cached = player->cached_duration();
            if (cached > 0.0)
                m_current.duration_sec = cached;
        }
        m_session.update(m_current);
    }

    void update_playback_status(bool playing)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current.playing = playing;
        m_session.update(m_current);
    }

    void update_position(double position_sec)
    {
        if (position_sec < 0.0)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_current.position_sec = position_sec;
        if (MpvPlayer *player = m_player)
        {
            // mpv reports the new duration shortly after a track change; the
            // value captured at the "start" event can still be the previous
            // track's. Refresh it so the media flyout's timeline is correct.
            const double cached = player->cached_duration();
            if (cached > 0.0 && std::abs(cached - m_current.duration_sec) > 0.5)
                m_current.duration_sec = cached;
        }
        // Refresh the mode flags here as well: settings changes do not emit a
        // new track event, and the media flyout should mirror the app.
        m_current.shuffle = m_state.settings.shuffle_enabled;
        m_current.repeat = smtc_repeat_mode(m_state.settings);
        m_session.update(m_current);
    }

    void update_volume(double) {} // SMTC has no per-app volume surface

private:
    void push_status(bool playing)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_current.playing = playing;
            m_session.update(m_current);
        }
        m_state.playing = playing;
        m_state.repaint_requested = true;
    }

    // Called on the SMTC event thread. Mirrors the MPRIS method handlers:
    // player calls are thread-safe, AppState flags are atomics.
    void on_command(MediaCommand command, double argument)
    {
        MpvPlayer *player = m_player;
        if (!player)
            return;

        switch (command)
        {
        case MediaCommand::Play:
            player->set_paused(false);
            push_status(true);
            break;

        case MediaCommand::Pause:
            player->set_paused(true);
            push_status(false);
            break;

        case MediaCommand::TogglePlayPause:
        {
            player->toggle_pause();
            push_status(!m_state.playing.load());
            break;
        }

        case MediaCommand::Stop:
            player->stop();
            push_status(false);
            break;

        case MediaCommand::Next:
            m_state.next_requested = true;
            m_state.repaint_requested = true;
            break;

        case MediaCommand::Previous:
            m_state.prev_requested = true;
            m_state.repaint_requested = true;
            break;

        case MediaCommand::SeekTo:
        {
            double target = std::max(0.0, argument);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_current.duration_sec > 0.0)
                    target = std::min(target, m_current.duration_sec);
            }
            player->seek(target);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_current.position_sec = target;
                m_session.update(m_current);
            }
            m_state.repaint_requested = true;
            break;
        }

        case MediaCommand::SeekBy:
        {
            double position = player->cached_time_pos();
            if (position < 0.0)
                position = 0.0;
            double target = std::max(0.0, position + argument);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_current.duration_sec > 0.0)
                    target = std::min(target, m_current.duration_sec);
                m_current.position_sec = target;
                m_session.update(m_current);
            }
            player->seek(target);
            m_state.repaint_requested = true;
            break;
        }
        }
    }

    AppState &m_state;
    MpvPlayer *&m_player;
    WinMediaSession m_session;
    std::mutex m_mutex;
    MediaSessionState m_current;
};

MprisPlayer::MprisPlayer(AppState &state, MpvPlayer *&player, void *native_window)
    : pImpl(std::make_unique<Impl>(state, player, native_window))
{
}

MprisPlayer::~MprisPlayer() = default;

void MprisPlayer::update_metadata(const Track &track, double duration_sec)
{
    pImpl->update_metadata(track, duration_sec);
}

void MprisPlayer::update_playback_status(bool playing)
{
    pImpl->update_playback_status(playing);
}

void MprisPlayer::update_position(double position_sec)
{
    pImpl->update_position(position_sec);
}

void MprisPlayer::update_volume(double volume)
{
    pImpl->update_volume(volume);
}
