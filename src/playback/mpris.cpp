#include "mpris.h"
#include "texture.h"
#include <glib.h>
#include <gio/gio.h>
#include <mutex>
#include <thread>
#include <string>
#include <cstring>

// ------------------------------------------------------------
// Helper: map a track id to a legal D-Bus object-path element.
// Object-path elements may only contain [A-Za-z0-9_]; YouTube ids
// routinely contain '-' (and other punctuation), which makes
// g_variant_new_object_path() return NULL and crash the metadata build.
// ------------------------------------------------------------
static std::string sanitize_path_element(const std::string &id)
{
    std::string out;
    out.reserve(id.size());
    for (char c : id)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')
            out += c;
        else
            out += '_';
    }
    if (out.empty())
        out = "_";
    return out;
}

// ------------------------------------------------------------
// Helper: build GVariant metadata dictionary (a{sv})
// ------------------------------------------------------------
static GVariant *build_metadata_gvariant(const Track &track, double duration_sec)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    if (!track.title.empty())
        g_variant_builder_add(&builder, "{sv}", "xesam:title",
                              g_variant_new_string(track.title.c_str()));

    if (!track.artist.empty())
    {
        const gchar *artists[] = {track.artist.c_str(), nullptr};
        g_variant_builder_add(&builder, "{sv}", "xesam:artist",
                              g_variant_new_strv(artists, -1));
    }

    if (!track.url.empty())
        g_variant_builder_add(&builder, "{sv}", "xesam:url",
                              g_variant_new_string(track.url.c_str()));

    if (!track.id.empty())
    {
        std::string track_id = "/org/mpris/MediaPlayer2/Track/" + sanitize_path_element(track.id);
        g_variant_builder_add(&builder, "{sv}", "mpris:trackid",
                              g_variant_new_object_path(track_id.c_str()));
    }

    if (duration_sec > 0)
        g_variant_builder_add(&builder, "{sv}", "mpris:length",
                              g_variant_new_int64((int64_t)(duration_sec * 1000000)));

    if (!track.thumbnail_url.empty())
    {
        // KDE Connect (and several other MPRIS clients) cannot fetch remote
        // HTTPS thumbnails, so expose a locally cached copy as a file:// URI.
        // Fall back to the remote URL if the cache/download fails.
        std::string art_url = track.thumbnail_url;
        // Stat-only check: the old get_cached_thumbnail_path() here could run
        // a synchronous curl on the D-Bus/main thread when the art wasn't
        // cached yet. Instead, kick the async thumbnail pipeline (the same
        // URL is normally being fetched for the UI anyway) and fall back to
        // the remote URL for now.
        if (thumbnail_is_cached(track.thumbnail_url))
            art_url = "file://" + get_cached_thumbnail_path(track.thumbnail_url);
        else
            texture_info(track.thumbnail_url);
        g_variant_builder_add(&builder, "{sv}", "xesam:artUrl",
                              g_variant_new_string(art_url.c_str()));
    }

    return g_variant_builder_end(&builder);
}

// ------------------------------------------------------------
// MPRIS implementation using GDBus
// ------------------------------------------------------------
class MprisPlayer::Impl
{
public:
    Impl(AppState &state, MpvPlayer *&player)
        : m_state(state), m_player(player),
          m_playing(false), m_position(0), m_volume(1.0), m_duration(0),
          m_connection(nullptr), m_root_registration_id(0), m_player_registration_id(0)
    {
        GError *error = nullptr;
        m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!m_connection)
        {
            g_printerr("Failed to connect to session bus: %s\n", error->message);
            g_error_free(error);
            return;
        }

        g_bus_own_name_on_connection(m_connection,
                                     "org.mpris.MediaPlayer2.p100",
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     nullptr, nullptr, nullptr, nullptr);

        // Register root
        GDBusNodeInfo *root_node = build_root_node();
        static GDBusInterfaceVTable root_vtable = {
            .method_call = nullptr,
            .get_property = root_get_property,
            .set_property = nullptr,
        };
        m_root_registration_id = g_dbus_connection_register_object(
            m_connection,
            "/org/mpris/MediaPlayer2",
            root_node->interfaces[0],
            &root_vtable,
            this,
            nullptr,
            &error);
        if (error)
        {
            g_printerr("Failed to register root object: %s\n", error->message);
            g_error_free(error);
        }
        g_dbus_node_info_unref(root_node);

        // Register player
        GDBusNodeInfo *player_node = build_player_node();
        static GDBusInterfaceVTable player_vtable = {
            .method_call = player_method_call,
            .get_property = player_get_property,
            .set_property = player_set_property,
        };
        m_player_registration_id = g_dbus_connection_register_object(
            m_connection,
            "/org/mpris/MediaPlayer2",
            player_node->interfaces[0],
            &player_vtable,
            this,
            nullptr,
            &error);
        if (error)
        {
            g_printerr("Failed to register player object: %s\n", error->message);
            g_error_free(error);
        }
        g_dbus_node_info_unref(player_node);

        // Emit initial properties changed (safe empty dict)
        emit_properties_changed();
    }

    ~Impl()
    {
        if (m_connection)
        {
            if (m_root_registration_id)
                g_dbus_connection_unregister_object(m_connection, m_root_registration_id);
            if (m_player_registration_id)
                g_dbus_connection_unregister_object(m_connection, m_player_registration_id);
            g_object_unref(m_connection);
        }
    }

    void update_metadata(const Track &track, double duration_sec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_track = track;
        m_duration = duration_sec;
        emit_properties_changed(); // safe
    }

    void update_playback_status(bool playing)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_playing != playing)
        {
            m_playing = playing;
            emit_properties_changed(); // safe
        }
    }

    void update_position(double position_sec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_position = position_sec;
    }

    void update_volume(double volume)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_volume != volume)
        {
            m_volume = volume;
            emit_properties_changed(); // safe
        }
    }

private:
    AppState &m_state;
    MpvPlayer *&m_player;
    GDBusConnection *m_connection;
    guint m_root_registration_id;
    guint m_player_registration_id;
    mutable std::mutex m_mutex;
    Track m_current_track;
    bool m_playing;
    double m_position;
    double m_volume;
    double m_duration;

    // ---------- root_get_property ----------
    static GVariant *root_get_property(GDBusConnection *conn,
                                       const gchar *sender,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       const gchar *property_name,
                                       GError **error,
                                       gpointer user_data)
    {
        if (strcmp(property_name, "Identity") == 0)
            return g_variant_new_string("p100 Music Player");
        if (strcmp(property_name, "SupportedUriSchemes") == 0)
        {
            const char *schemes[] = {"file", "http", "https", nullptr};
            return g_variant_new_strv(schemes, -1);
        }
        if (strcmp(property_name, "SupportedMimeTypes") == 0)
        {
            const char *mimes[] = {"audio/mpeg", "audio/ogg", "audio/flac", "audio/opus", nullptr};
            return g_variant_new_strv(mimes, -1);
        }
        return nullptr;
    }

    // ---------- player_method_call ----------
    static void player_method_call(GDBusConnection *conn,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *method_name,
                                   GVariant *parameters,
                                   GDBusMethodInvocation *invocation,
                                   gpointer user_data)
    {
        Impl *self = (Impl *)user_data;
        if (strcmp(method_name, "Play") == 0)
        {
            self->m_player->set_paused(false);
            self->m_state.playing = true;
            self->update_playback_status(true);
            self->m_state.repaint_requested = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "Pause") == 0)
        {
            self->m_player->set_paused(true);
            self->m_state.playing = false;
            self->update_playback_status(false);
            self->m_state.repaint_requested = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "PlayPause") == 0)
        {
            self->m_player->toggle_pause();
            self->update_playback_status(self->m_state.playing);
            self->m_state.repaint_requested = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "Stop") == 0)
        {
            self->m_player->stop();
            self->m_state.playing = false;
            self->update_playback_status(false);
            self->m_state.repaint_requested = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "Next") == 0)
        {
            self->m_state.next_requested = true;
            self->m_state.repaint_requested = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "Previous") == 0)
        {
            self->m_state.prev_requested = true;
            self->m_state.repaint_requested = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "Seek") == 0)
        {
            gint64 offset;
            g_variant_get(parameters, "(x)", &offset);
            double pos = self->m_position + offset / 1000000.0;
            if (pos < 0)
                pos = 0;
            self->m_player->seek(pos);
            self->m_position = pos;
            self->emit_seeked(pos);
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else if (strcmp(method_name, "SetPosition") == 0)
        {
            const gchar *track_id;
            gint64 position;
            g_variant_get(parameters, "(sx)", &track_id, &position);
            double pos = position / 1000000.0;
            self->m_player->seek(pos);
            self->m_position = pos;
            self->emit_seeked(pos);
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else
        {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                                  "Unknown method '%s'", method_name);
        }
    }

    // ---------- player_get_property ----------
    static GVariant *player_get_property(GDBusConnection *conn,
                                         const gchar *sender,
                                         const gchar *object_path,
                                         const gchar *interface_name,
                                         const gchar *property_name,
                                         GError **error,
                                         gpointer user_data)
    {
        Impl *self = (Impl *)user_data;
        std::lock_guard<std::mutex> lock(self->m_mutex);

        if (strcmp(property_name, "PlaybackStatus") == 0)
        {
            const char *status = self->m_playing ? "Playing" : "Paused";
            return g_variant_new_string(status);
        }
        if (strcmp(property_name, "Metadata") == 0)
        {
            return build_metadata_gvariant(self->m_current_track, self->m_duration);
        }
        if (strcmp(property_name, "Volume") == 0)
        {
            return g_variant_new_double(self->m_volume);
        }
        if (strcmp(property_name, "Position") == 0)
        {
            return g_variant_new_int64((int64_t)(self->m_position * 1000000));
        }
        if (strcmp(property_name, "CanGoNext") == 0 ||
            strcmp(property_name, "CanGoPrevious") == 0 ||
            strcmp(property_name, "CanPlay") == 0 ||
            strcmp(property_name, "CanPause") == 0 ||
            strcmp(property_name, "CanSeek") == 0 ||
            strcmp(property_name, "CanControl") == 0)
        {
            return g_variant_new_boolean(true);
        }
        return nullptr;
    }

    // ---------- player_set_property ----------
    static gboolean player_set_property(GDBusConnection *conn,
                                        const gchar *sender,
                                        const gchar *object_path,
                                        const gchar *interface_name,
                                        const gchar *property_name,
                                        GVariant *value,
                                        GError **error,
                                        gpointer user_data)
    {
        Impl *self = (Impl *)user_data;
        if (strcmp(property_name, "Volume") == 0)
        {
            double vol;
            g_variant_get(value, "d", &vol);
            if (vol < 0)
                vol = 0;
            if (vol > 1.0)
                vol = 1.0;
            self->m_volume = vol;
            self->m_player->set_volume((int)(vol * 100));
            self->emit_properties_changed(); // safe
            return TRUE;
        }
        return FALSE;
    }

    // ---------- Build nodes ----------
    static GDBusNodeInfo *build_root_node()
    {
        const char *xml =
            "<node>"
            "  <interface name='org.mpris.MediaPlayer2'>"
            "    <property name='Identity' type='s' access='read'/>"
            "    <property name='SupportedUriSchemes' type='as' access='read'/>"
            "    <property name='SupportedMimeTypes' type='as' access='read'/>"
            "  </interface>"
            "</node>";
        GError *error = nullptr;
        GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
        if (error)
        {
            g_printerr("Failed to parse root XML: %s\n", error->message);
            g_error_free(error);
        }
        return node;
    }

    static GDBusNodeInfo *build_player_node()
    {
        const char *xml =
            "<node>"
            "  <interface name='org.mpris.MediaPlayer2.Player'>"
            "    <method name='Play'/>"
            "    <method name='Pause'/>"
            "    <method name='PlayPause'/>"
            "    <method name='Stop'/>"
            "    <method name='Next'/>"
            "    <method name='Previous'/>"
            "    <method name='Seek'>"
            "      <arg name='Offset' type='x' direction='in'/>"
            "    </method>"
            "    <method name='SetPosition'>"
            "      <arg name='TrackId' type='s' direction='in'/>"
            "      <arg name='Position' type='x' direction='in'/>"
            "    </method>"
            "    <property name='PlaybackStatus' type='s' access='read'/>"
            "    <property name='Metadata' type='a{sv}' access='read'/>"
            "    <property name='Volume' type='d' access='readwrite'/>"
            "    <property name='Position' type='x' access='read'/>"
            "    <property name='CanGoNext' type='b' access='read'/>"
            "    <property name='CanGoPrevious' type='b' access='read'/>"
            "    <property name='CanPlay' type='b' access='read'/>"
            "    <property name='CanPause' type='b' access='read'/>"
            "    <property name='CanSeek' type='b' access='read'/>"
            "    <property name='CanControl' type='b' access='read'/>"
            "    <signal name='Seeked'>"
            "      <arg name='Position' type='x'/>"
            "    </signal>"
            "  </interface>"
            "</node>";
        GError *error = nullptr;
        GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
        if (error)
        {
            g_printerr("Failed to parse player XML: %s\n", error->message);
            g_error_free(error);
        }
        return node;
    }

    // ---------- emit_properties_changed (safe: empty dict) ----------
    void emit_properties_changed()
    {
        if (!m_connection)
            return;

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

        g_variant_builder_add(&builder, "{sv}", "PlaybackStatus",
                              g_variant_new_string(m_playing ? "Playing" : "Paused"));
        g_variant_builder_add(&builder, "{sv}", "Volume",
                              g_variant_new_double(m_volume));
        g_variant_builder_add(&builder, "{sv}", "Metadata",
                              build_metadata_gvariant(m_current_track, m_duration));
        g_variant_builder_add(&builder, "{sv}", "CanGoNext", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "CanGoPrevious", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "CanPlay", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "CanPause", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "CanSeek", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "CanControl", g_variant_new_boolean(TRUE));
        // Note: Position is deliberately NOT included here — per the MPRIS spec,
        // Position changes are communicated via the separate Seeked signal, not
        // PropertiesChanged, since polling-via-signal for a constantly-changing
        // value would be wasteful.

        GVariant *dict = g_variant_builder_end(&builder);
        GVariant *invalidated = g_variant_new_strv(NULL, 0);

        GVariant *children[] = {
            g_variant_new_string("org.mpris.MediaPlayer2.Player"),
            dict,
            invalidated};
        GVariant *signal_args = g_variant_new_tuple(children, 3);

        g_dbus_connection_emit_signal(
            m_connection,
            nullptr,
            "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            signal_args,
            nullptr);
    }
    // ---------- emit_seeked ----------
    void emit_seeked(double position_sec)
    {
        if (!m_connection)
            return;
        gint64 micros = (int64_t)(position_sec * 1000000);
        g_dbus_connection_emit_signal(
            m_connection,
            nullptr,
            "/org/mpris/MediaPlayer2",
            "org.mpris.MediaPlayer2.Player",
            "Seeked",
            g_variant_new("(x)", micros),
            nullptr);
    }
};

// ------------------------------------------------------------
// Public interface
// ------------------------------------------------------------
MprisPlayer::MprisPlayer(AppState &state, MpvPlayer *&player)
    : pImpl(std::make_unique<Impl>(state, player)) {}

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