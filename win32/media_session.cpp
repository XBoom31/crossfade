#include "media_session.h"
#include "winrt_util.h"

#include <windows.media.h>
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <systemmediatransportcontrolsinterop.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

using ABI::Windows::Foundation::ITypedEventHandler;
using ABI::Windows::Foundation::IUriRuntimeClass;
using ABI::Windows::Foundation::IUriRuntimeClassFactory;
using ABI::Windows::Foundation::TimeSpan;
using ABI::Windows::Media::IMusicDisplayProperties;
using ABI::Windows::Media::IMusicDisplayProperties2;
using ABI::Windows::Media::IPlaybackPositionChangeRequestedEventArgs;
using ABI::Windows::Media::ISystemMediaTransportControls;
using ABI::Windows::Media::ISystemMediaTransportControls2;
using ABI::Windows::Media::ISystemMediaTransportControlsButtonPressedEventArgs;
using ABI::Windows::Media::ISystemMediaTransportControlsDisplayUpdater;
using ABI::Windows::Media::ISystemMediaTransportControlsTimelineProperties;
using ABI::Windows::Media::MediaPlaybackAutoRepeatMode;
using ABI::Windows::Media::MediaPlaybackAutoRepeatMode_List;
using ABI::Windows::Media::MediaPlaybackAutoRepeatMode_None;
using ABI::Windows::Media::MediaPlaybackAutoRepeatMode_Track;
using ABI::Windows::Media::MediaPlaybackStatus;
using ABI::Windows::Media::MediaPlaybackStatus_Closed;
using ABI::Windows::Media::MediaPlaybackStatus_Paused;
using ABI::Windows::Media::MediaPlaybackStatus_Playing;
using ABI::Windows::Media::MediaPlaybackStatus_Stopped;
using ABI::Windows::Media::MediaPlaybackType;
using ABI::Windows::Media::MediaPlaybackType_Music;
using ABI::Windows::Media::PlaybackPositionChangeRequestedEventArgs;
using ABI::Windows::Media::SystemMediaTransportControls;
using ABI::Windows::Media::SystemMediaTransportControlsButton;
using ABI::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs;
using ABI::Windows::Media::SystemMediaTransportControlsButton_FastForward;
using ABI::Windows::Media::SystemMediaTransportControlsButton_Next;
using ABI::Windows::Media::SystemMediaTransportControlsButton_Pause;
using ABI::Windows::Media::SystemMediaTransportControlsButton_Play;
using ABI::Windows::Media::SystemMediaTransportControlsButton_Previous;
using ABI::Windows::Media::SystemMediaTransportControlsButton_Rewind;
using ABI::Windows::Media::SystemMediaTransportControlsButton_Stop;
using ABI::Windows::Storage::Streams::IRandomAccessStreamReference;
using ABI::Windows::Storage::Streams::IRandomAccessStreamReferenceStatics;

namespace
{

// WinRT delegates are declared over runtime classes, but the interface the
// system hands to Invoke is the projected ABI interface. The two type
// parameters after the runtime classes spell those out.
template <class TSenderRuntime, class TArgsRuntime, class TSenderAbi, class TArgsAbi>
class TypedEventHandler final
    : public ITypedEventHandler<TSenderRuntime, TArgsRuntime>
{
public:
    using Fn = std::function<void(TSenderAbi, TArgsAbi)>;
    using Interface = ITypedEventHandler<TSenderRuntime, TArgsRuntime>;

    explicit TypedEventHandler(Fn fn) : m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(Interface) || riid == __uuidof(IUnknown))
        {
            *ppv = static_cast<Interface *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_refs;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --m_refs;
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(TSenderAbi sender, TArgsAbi args) override
    {
        if (m_fn)
            m_fn(sender, args);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    Fn m_fn;
};

LONGLONG seconds_to_ticks(double seconds)
{
    if (seconds <= 0.0)
        return 0;
    return (LONGLONG)(seconds * 10000000.0);
}

double ticks_to_seconds(LONGLONG ticks)
{
    return (double)ticks / 10000000.0;
}

} // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct WinMediaSession::Impl
{
    struct CommandSink
    {
        std::mutex mutex;
        CommandHandler fn;
    };

    explicit Impl(void *window) : m_window(window) {}

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();

        if (m_sink)
        {
            std::lock_guard<std::mutex> lock(m_sink->mutex);
            m_sink->fn = nullptr;
        }
    }

    bool valid() const { return m_ready.load(std::memory_order_acquire); }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        return m_error;
    }

    void set_error(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_error = message;
    }

    void set_command_handler(CommandHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_sink->mutex);
        m_sink->fn = std::move(handler);
    }

    void submit(const MediaSessionState &state)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_has_latest)
            {
                m_latest = state;
                m_has_latest = true;
            }
            else
            {
                const bool structural =
                    state.track_id != m_latest.track_id ||
                    state.title != m_latest.title ||
                    state.artist != m_latest.artist ||
                    state.album != m_latest.album ||
                    state.art != m_latest.art ||
                    state.playing != m_latest.playing ||
                    state.can_seek != m_latest.can_seek ||
                    state.can_next != m_latest.can_next ||
                    state.can_prev != m_latest.can_prev ||
                    state.shuffle != m_latest.shuffle ||
                    state.repeat != m_latest.repeat ||
                    std::abs(state.duration_sec - m_latest.duration_sec) > 0.5;

                // Position-only ticks are cheap to ignore: the shell
                // interpolates while playing. Paused seeks need to be exact.
                const double min_delta = state.playing ? 1.0 : 0.25;
                if (!structural &&
                    std::abs(state.position_sec - m_latest.position_sec) < min_delta)
                    return;

                m_latest = state;
            }
            m_pending = m_latest;
            m_has_pending = true;
        }
        m_cv.notify_one();
    }

    // ---- worker ----------------------------------------------------------

    void run()
    {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        const bool com_ready = SUCCEEDED(hr);
        if (!com_ready)
        {
            set_error("RoInitialize failed");
            cfwin::log_failure("RoInitialize", hr);
            // Drain submissions so callers never block; nothing to apply.
            wait_forever();
            return;
        }

        initialize_smtc();

        if (m_ready.load(std::memory_order_acquire))
        {
            for (;;)
            {
                MediaSessionState state;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] { return m_quit || m_has_pending; });
                    if (m_quit)
                        break;
                    state = m_pending;
                    m_has_pending = false;
                }
                apply(state);
            }
        }
        else
        {
            wait_forever();
        }

        shutdown_smtc();
        RoUninitialize();
    }

    void wait_forever()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_quit; });
    }

    void initialize_smtc()
    {
        if (!m_window)
        {
            set_error("no application window");
            return;
        }

        HWND hwnd = static_cast<HWND>(m_window);
        if (!IsWindow(hwnd))
        {
            set_error("application window is not valid");
            return;
        }

        cfwin::HString class_name(RuntimeClass_Windows_Media_SystemMediaTransportControls);
        cfwin::ComPtr<ISystemMediaTransportControlsInterop> interop;
        HRESULT hr = RoGetActivationFactory(class_name.get(),
                                            __uuidof(ISystemMediaTransportControlsInterop),
                                            interop.put_void());
        if (FAILED(hr))
        {
            set_error("SMTC interop activation failed");
            cfwin::log_failure("RoGetActivationFactory(SystemMediaTransportControls)", hr);
            return;
        }

        hr = interop->GetForWindow(hwnd, __uuidof(ISystemMediaTransportControls), m_smtc.put_void());
        if (FAILED(hr))
        {
            set_error("SMTC GetForWindow failed");
            cfwin::log_failure("ISystemMediaTransportControlsInterop::GetForWindow", hr);
            return;
        }

        m_smtc->put_IsEnabled(TRUE);
        m_smtc->put_IsPlayEnabled(TRUE);
        m_smtc->put_IsPauseEnabled(TRUE);
        m_smtc->put_IsStopEnabled(TRUE);
        m_smtc->put_IsNextEnabled(TRUE);
        m_smtc->put_IsPreviousEnabled(TRUE);
        m_smtc->put_IsFastForwardEnabled(FALSE);
        m_smtc->put_IsRewindEnabled(FALSE);
        m_smtc->put_IsRecordEnabled(FALSE);

        m_smtc->get_DisplayUpdater(m_updater.put());
        m_smtc->QueryInterface(__uuidof(ISystemMediaTransportControls2), m_smtc2.put_void());

        if (m_smtc2)
        {
            cfwin::HString timeline_class(RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties);
            cfwin::ComPtr<IInspectable> instance;
            hr = RoActivateInstance(timeline_class.get(), instance.put());
            if (SUCCEEDED(hr))
                instance->QueryInterface(__uuidof(ISystemMediaTransportControlsTimelineProperties),
                                         m_timeline.put_void());
        }

        auto sink = m_sink;
        m_button_handler.attach(new TypedEventHandler<SystemMediaTransportControls *,
                                                      SystemMediaTransportControlsButtonPressedEventArgs *,
                                                      ISystemMediaTransportControls *,
                                                      ISystemMediaTransportControlsButtonPressedEventArgs *>(
            [sink](ISystemMediaTransportControls *,
                   ISystemMediaTransportControlsButtonPressedEventArgs *args)
            {
                if (!args)
                    return;
                SystemMediaTransportControlsButton button = SystemMediaTransportControlsButton_Play;
                if (FAILED(args->get_Button(&button)))
                    return;

                MediaCommand command = MediaCommand::TogglePlayPause;
                bool have = true;
                switch (button)
                {
                case SystemMediaTransportControlsButton_Play: command = MediaCommand::Play; break;
                case SystemMediaTransportControlsButton_Pause: command = MediaCommand::Pause; break;
                case SystemMediaTransportControlsButton_Stop: command = MediaCommand::Stop; break;
                case SystemMediaTransportControlsButton_Next: command = MediaCommand::Next; break;
                case SystemMediaTransportControlsButton_Previous: command = MediaCommand::Previous; break;
                case SystemMediaTransportControlsButton_FastForward: command = MediaCommand::SeekBy; break;
                case SystemMediaTransportControlsButton_Rewind: command = MediaCommand::SeekBy; break;
                default: have = false; break;
                }
                if (!have)
                    return;

                double arg = 0.0;
                if (button == SystemMediaTransportControlsButton_FastForward)
                    arg = 10.0;
                else if (button == SystemMediaTransportControlsButton_Rewind)
                    arg = -10.0;
                emit(sink, command, arg);
            }));

        EventRegistrationToken token{};
        hr = m_smtc->add_ButtonPressed(m_button_handler.get(), &token);
        if (SUCCEEDED(hr))
            m_button_token = token;
        else
            cfwin::log_failure("add_ButtonPressed", hr);

        if (m_smtc2)
        {
            m_position_handler.attach(new TypedEventHandler<SystemMediaTransportControls *,
                                                            PlaybackPositionChangeRequestedEventArgs *,
                                                            ISystemMediaTransportControls *,
                                                            IPlaybackPositionChangeRequestedEventArgs *>(
                [sink](ISystemMediaTransportControls *, IPlaybackPositionChangeRequestedEventArgs *args)
                {
                    if (!args)
                        return;
                    TimeSpan position{};
                    if (FAILED(args->get_RequestedPlaybackPosition(&position)))
                        return;
                    emit(sink, MediaCommand::SeekTo, ticks_to_seconds(position.Duration));
                }));

            EventRegistrationToken position_token{};
            hr = m_smtc2->add_PlaybackPositionChangeRequested(
                m_position_handler.get(), &position_token);
            if (SUCCEEDED(hr))
                m_position_token = position_token;
            else
                cfwin::log_failure("add_PlaybackPositionChangeRequested", hr);
        }

        m_ready.store(true, std::memory_order_release);
        fprintf(stderr, "[crossfade/winrt] SMTC session ready\n");
    }

    static void emit(const std::shared_ptr<CommandSink> &sink, MediaCommand command, double arg)
    {
        CommandHandler fn;
        {
            std::lock_guard<std::mutex> lock(sink->mutex);
            fn = sink->fn;
        }
        if (fn)
            fn(command, arg);
    }

    void shutdown_smtc()
    {
        if (m_smtc && m_button_token.value)
            m_smtc->remove_ButtonPressed(m_button_token);
        if (m_smtc2 && m_position_token.value)
            m_smtc2->remove_PlaybackPositionChangeRequested(m_position_token);
        if (m_smtc)
        {
            m_smtc->put_IsEnabled(FALSE);
            m_smtc->put_PlaybackStatus(MediaPlaybackStatus_Closed);
        }
        m_timeline.reset();
        m_smtc2.reset();
        m_updater.reset();
        m_smtc.reset();
        m_button_handler.reset();
        m_position_handler.reset();
        m_ready.store(false, std::memory_order_release);
    }

    void apply(const MediaSessionState &state)
    {
        if (!m_smtc)
            return;

        const bool metadata_changed =
            !m_have_metadata ||
            state.track_id != m_applied.track_id ||
            state.title != m_applied.title ||
            state.artist != m_applied.artist ||
            state.album != m_applied.album ||
            state.art != m_applied.art;
        const bool status_changed = !m_have_metadata || state.playing != m_applied.playing;
        const bool flags_changed =
            !m_have_metadata ||
            state.can_next != m_applied.can_next ||
            state.can_prev != m_applied.can_prev ||
            state.shuffle != m_applied.shuffle ||
            state.repeat != m_applied.repeat;

        if (metadata_changed)
            apply_metadata(state);

        if (status_changed)
        {
            MediaPlaybackStatus status = state.playing ? MediaPlaybackStatus_Playing
                                                       : MediaPlaybackStatus_Paused;
            if (state.title.empty())
                status = MediaPlaybackStatus_Stopped;
            m_smtc->put_PlaybackStatus(status);
        }

        if (flags_changed)
        {
            m_smtc->put_IsNextEnabled(state.can_next ? TRUE : FALSE);
            m_smtc->put_IsPreviousEnabled(state.can_prev ? TRUE : FALSE);
            m_smtc->put_IsPlayEnabled(TRUE);
            m_smtc->put_IsPauseEnabled(TRUE);
            m_smtc->put_IsStopEnabled(TRUE);
            if (m_smtc2)
            {
                m_smtc2->put_ShuffleEnabled(state.shuffle ? TRUE : FALSE);
                MediaPlaybackAutoRepeatMode mode = MediaPlaybackAutoRepeatMode_None;
                if (state.repeat == 1)
                    mode = MediaPlaybackAutoRepeatMode_List;
                else if (state.repeat == 2)
                    mode = MediaPlaybackAutoRepeatMode_Track;
                m_smtc2->put_AutoRepeatMode(mode);
            }
        }

        apply_timeline(state, metadata_changed || status_changed ||
                                  (m_have_metadata &&
                                   std::abs(state.position_sec - m_applied.position_sec) > 2.0));

        m_applied = state;
        m_have_metadata = true;
    }

    void apply_metadata(const MediaSessionState &state)
    {
        if (!m_updater)
            return;

        if (state.title.empty())
        {
            m_updater->ClearAll();
            m_updater->Update();
            m_art_uri.clear();
            m_thumbnail.reset();
            return;
        }

        m_updater->put_Type(MediaPlaybackType_Music);

        cfwin::HString title(state.title);
        cfwin::HString artist(state.artist);
        cfwin::HString album(state.album);

        cfwin::ComPtr<IMusicDisplayProperties> music;
        m_updater->get_MusicProperties(music.put());
        if (music)
        {
            music->put_Title(title.get());
            music->put_Artist(artist.get());
            music->put_AlbumArtist(artist.get());
            if (!state.album.empty())
            {
                cfwin::ComPtr<IMusicDisplayProperties2> music2;
                if (SUCCEEDED(music->QueryInterface(__uuidof(IMusicDisplayProperties2),
                                                    music2.put_void())) &&
                    music2)
                    music2->put_AlbumTitle(album.get());
            }
        }

        if (!state.track_id.empty())
        {
            cfwin::HString media_id(state.track_id);
            m_updater->put_AppMediaId(media_id.get());
        }

        const std::string art_uri = cfwin::to_uri(state.art);
        if (art_uri != m_art_uri)
        {
            m_art_uri = art_uri;
            m_thumbnail.reset();
            if (!art_uri.empty())
                m_thumbnail = make_thumbnail(art_uri);
            m_updater->put_Thumbnail(m_thumbnail ? m_thumbnail.get() : nullptr);
        }

        m_updater->Update();
    }

    cfwin::ComPtr<IRandomAccessStreamReference> make_thumbnail(const std::string &uri)
    {
        cfwin::ComPtr<IRandomAccessStreamReference> result;

        cfwin::ComPtr<IRandomAccessStreamReferenceStatics> statics;
        cfwin::HString statics_class(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference);
        HRESULT hr = RoGetActivationFactory(statics_class.get(),
                                            __uuidof(IRandomAccessStreamReferenceStatics),
                                            statics.put_void());
        if (FAILED(hr) || !statics)
        {
            cfwin::log_failure("RandomAccessStreamReference activation", hr);
            return result;
        }

        cfwin::ComPtr<IUriRuntimeClassFactory> uri_factory;
        cfwin::HString uri_class(RuntimeClass_Windows_Foundation_Uri);
        hr = RoGetActivationFactory(uri_class.get(), __uuidof(IUriRuntimeClassFactory),
                                    uri_factory.put_void());
        if (FAILED(hr) || !uri_factory)
        {
            cfwin::log_failure("Uri activation", hr);
            return result;
        }

        cfwin::HString uri_string(uri);
        cfwin::ComPtr<IUriRuntimeClass> uri_object;
        hr = uri_factory->CreateUri(uri_string.get(), uri_object.put());
        if (FAILED(hr) || !uri_object)
        {
            cfwin::log_failure("CreateUri", hr);
            return result;
        }

        hr = statics->CreateFromUri(uri_object.get(), result.put());
        if (FAILED(hr))
        {
            cfwin::log_failure("RandomAccessStreamReference::CreateFromUri", hr);
            result.reset();
        }
        return result;
    }

    void apply_timeline(const MediaSessionState &state, bool force)
    {
        if (!m_smtc2 || !m_timeline)
            return;
        if (state.duration_sec <= 0.0)
            return;

        // The shell extrapolates the position from the last timeline update
        // while playing, so one update per second is plenty.
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - m_last_timeline < std::chrono::seconds(1))
            return;
        m_last_timeline = now;

        const double duration = state.duration_sec;
        const double position = std::clamp(state.position_sec, 0.0, duration);

        TimeSpan start{};
        start.Duration = 0;
        TimeSpan end{};
        end.Duration = seconds_to_ticks(duration);
        TimeSpan current{};
        current.Duration = seconds_to_ticks(position);

        m_timeline->put_StartTime(start);
        m_timeline->put_MinSeekTime(start);
        m_timeline->put_MaxSeekTime(end);
        m_timeline->put_EndTime(end);
        m_timeline->put_Position(current);
        m_smtc2->UpdateTimelineProperties(m_timeline.get());
    }

    void *m_window = nullptr;
    std::thread m_thread;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    MediaSessionState m_latest;
    MediaSessionState m_pending;
    MediaSessionState m_applied;
    bool m_has_latest = false;
    bool m_has_pending = false;
    bool m_quit = false;

    mutable std::mutex m_error_mutex;
    std::string m_error;
    std::atomic<bool> m_ready{false};

    std::shared_ptr<CommandSink> m_sink = std::make_shared<CommandSink>();

    cfwin::ComPtr<ISystemMediaTransportControls> m_smtc;
    cfwin::ComPtr<ISystemMediaTransportControls2> m_smtc2;
    cfwin::ComPtr<ISystemMediaTransportControlsDisplayUpdater> m_updater;
    cfwin::ComPtr<ISystemMediaTransportControlsTimelineProperties> m_timeline;
    cfwin::ComPtr<IRandomAccessStreamReference> m_thumbnail;
    cfwin::ComPtr<ITypedEventHandler<SystemMediaTransportControls *,
                                     SystemMediaTransportControlsButtonPressedEventArgs *>>
        m_button_handler;
    cfwin::ComPtr<ITypedEventHandler<SystemMediaTransportControls *,
                                     PlaybackPositionChangeRequestedEventArgs *>>
        m_position_handler;
    EventRegistrationToken m_button_token{};
    EventRegistrationToken m_position_token{};

    bool m_have_metadata = false;
    std::string m_art_uri;
    std::chrono::steady_clock::time_point m_last_timeline{};
};

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

WinMediaSession::WinMediaSession(void *native_window)
    : pImpl(std::make_unique<Impl>(native_window))
{
    Impl *impl = pImpl.get();
    impl->m_thread = std::thread([impl] { impl->run(); });
}

WinMediaSession::~WinMediaSession() = default;

bool WinMediaSession::valid() const
{
    return pImpl && pImpl->valid();
}

std::string WinMediaSession::last_error() const
{
    return pImpl ? pImpl->last_error() : std::string("no session");
}

void WinMediaSession::set_command_handler(CommandHandler handler)
{
    if (pImpl)
        pImpl->set_command_handler(std::move(handler));
}

void WinMediaSession::update(const MediaSessionState &state)
{
    if (pImpl)
        pImpl->submit(state);
}

void WinMediaSession::clear()
{
    if (!pImpl)
        return;
    MediaSessionState empty;
    empty.duration_sec = 0.0;
    empty.playing = false;
    pImpl->submit(empty);
}
