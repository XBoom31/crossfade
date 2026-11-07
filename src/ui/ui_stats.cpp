#include "ui.h"
#include "texture.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <utility>

namespace
{

// ----------------------------------------------------------------------
// Time windows: sliding "last 7/30 days" periods.
// ----------------------------------------------------------------------
int64_t period_window_start(int period, int week_offset)
{
    int64_t now = (int64_t)time(nullptr);
    if (period == 1)
        return now - 30LL * 86400LL;
    if (period == 2)
        return now - (int64_t)(week_offset + 1) * 7LL * 86400LL;
    return 0; // all time
}

const char *period_label(int period)
{
    switch (period)
    {
    case 1: return "This Month";
    case 2: return "This Week";
    default: return "All Time";
    }
}

static float clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float ease_out_back(float t)
{
    t = clamp01f(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// One place for the Stats page motion clocks. `stagger` returns an eased
// 0..1 value for an element with a delay, `pop` adds a little overshoot for
// things that should feel like they physically land, and `in_view` fades
// elements as they scroll into the viewport.
struct StatsAnim
{
    double now = 0.0;
    double start = 0.0;

    float stagger(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float d = ui_anim_delay(delay);
        const float du = ui_anim_duration(dur);
        if (du <= 0.0001f)
            return 1.0f;
        return ui_anim_ease_out(clamp01f((float)((now - start - d) / du)));
    }

    float pop(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float d = ui_anim_delay(delay);
        const float du = ui_anim_duration(dur);
        if (du <= 0.0001f)
            return 1.0f;
        return ease_out_back(clamp01f((float)((now - start - d) / du)));
    }

    float in_view(float screen_y, float span = 120.0f) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        return ui_anim_ease_out(clamp01f((bottom - screen_y) / ui_px(span)));
    }
};

static void stats_card(const char *label, const std::string &value, float x, float y,
                       float w, float h, float t, float hover, float pulse)
{
    const UiMetrics &m = ui_m();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float lift = -ui_px(2.5f) * hover;
    const float press = (pulse >= 0.0f && pulse < 0.4f)
                            ? sinf(clamp01f(pulse / 0.4f) * 3.14159265f) * 0.022f
                            : 0.0f;
    const float sc = 0.93f + 0.07f * t + 0.014f * hover - press;
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f + lift;
    auto P = [&](float px, float py) {
        return ImVec2(cx + (px - cx) * sc, cy + (py - cy) * sc);
    };

    // Click pulse: an expanding accent ring around the card.
    if (pulse >= 0.0f && pulse < 1.0f)
    {
        const float pr = ui_anim_ease_out(pulse);
        const float grow = ui_px(16.0f) * pr;
        dl->AddRect(P(x - grow, y - grow), P(x + w + grow, y + h + grow),
                    ui_accent_u32(0.5f * (1.0f - pr) * t),
                    m.radius_md + grow, 0, ui_px(2.0f));
    }

    // Hover glow behind the card.
    if (hover > 0.01f)
    {
        dl->AddRectFilled(P(x - ui_px(5.0f), y - ui_px(5.0f)),
                          P(x + w + ui_px(5.0f), y + h + ui_px(5.0f)),
                          ui_accent_u32(0.075f * hover * t), m.radius_md + ui_px(5.0f));
    }

    dl->AddRectFilled(P(x, y), P(x + w, y + h),
                      ui_with_alpha(ui_col(ui_c().surface_hover), t),
                      m.radius_md);
    dl->AddRect(P(x, y), P(x + w, y + h),
                ui_with_alpha(ui_border_u32(), (0.4f + 0.6f * t) * t * (0.75f + 0.25f * hover)),
                m.radius_md, 0, 1.0f);

    // Accent tick that grows in and extends on hover.
    const float tick = (24.0f + 38.0f * hover) * m.layout_scale * t;
    dl->AddRectFilled(P(x + m.space_3, y + m.space_3),
                      P(x + m.space_3 + tick, y + m.space_3 + 2.0f * m.layout_scale),
                      ui_accent_u32(0.55f + 0.45f * hover), 1.0f);

    dl->AddText(g_ui_fonts.body, 16.0f * m.text_scale,
                P(x + m.space_3, y + m.space_3 + 9.0f * m.layout_scale),
                ui_with_alpha(ui_text_muted_u32(), t), label);

    // Auto-fit long values instead of immediately truncating them.
    const float max_vw = std::max(1.0f, w - m.space_3 * 2.0f);
    const float full_vw = g_ui_fonts.semibold->CalcTextSizeA(24.0f * m.text_scale, FLT_MAX, 0.0f,
                                                             value.c_str()).x;
    float value_size = 24.0f;
    if (full_vw > max_vw)
        value_size = std::max(16.0f, 24.0f * max_vw / full_vw);
    std::string v = ui_ellipsize(g_ui_fonts.semibold, value_size, value, max_vw);
    const float vy = y + h - value_size * m.text_scale * 1.25f - m.space_3 +
                     (1.0f - t) * ui_anim_offset(8.0f);
    dl->AddText(g_ui_fonts.semibold, value_size * m.text_scale, P(x + m.space_3, vy),
                ui_with_alpha(ui_text_u32(), t), v.c_str());
}

} // namespace

void rebuild_stats(AppState &state)
{
    int64_t start_ts = period_window_start(state.stats_period, state.stats_week_offset);
    int64_t end_ts = 0;
    if (state.stats_period == 2)
        end_ts = start_ts + 7LL * 86400LL;

    struct Agg
    {
        std::string title;
        std::string artist;
        std::string thumb;
        int count = 0;
    };
    std::unordered_map<std::string, Agg> by_track;
    std::unordered_map<std::string, int> by_artist;

    for (const auto &ev : state.play_log)
    {
        if (ev.ts < start_ts)
            continue;
        if (end_ts && ev.ts >= end_ts)
            continue;
        auto &a = by_track[ev.id];
        a.title = ev.title;
        a.artist = ev.artist;
        a.thumb = ev.thumbnail_url;
        ++a.count;
        if (!ev.artist.empty())
            ++by_artist[ev.artist];
    }

    state.stats.tracks.clear();
    state.stats.plays.clear();

    std::vector<std::pair<std::string, Agg>> items(by_track.begin(), by_track.end());
    std::sort(items.begin(), items.end(),
              [](const auto &x, const auto &y)
              {
                  if (x.second.count != y.second.count)
                      return x.second.count > y.second.count;
                  return x.second.title < y.second.title;
              });

    for (const auto &kv : items)
    {
        Track t;
        t.id = kv.first;
        t.title = kv.second.title.empty() ? "Unknown Track" : kv.second.title;
        t.artist = kv.second.artist.empty() ? "Unknown Artist" : kv.second.artist;
        t.thumbnail_url = kv.second.thumb;
        t.url = "https://www.youtube.com/watch?v=" + kv.first;
        state.stats.tracks.push_back(t);
        state.stats.plays.push_back(kv.second.count);
    }

    state.stats.top_artist.clear();
    state.stats.top_artist_plays = 0;
    for (const auto &kv : by_artist)
        if (kv.second > state.stats.top_artist_plays)
        {
            state.stats.top_artist = kv.first;
            state.stats.top_artist_plays = kv.second;
        }

    // Top artists sorted.
    state.stats.top_artists.clear();
    std::vector<std::pair<std::string, int>> artist_items(by_artist.begin(), by_artist.end());
    std::sort(artist_items.begin(), artist_items.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (auto &kv : artist_items)
        state.stats.top_artists.push_back(kv);

    state.stats.total_plays = 0;
    for (const auto &kv : by_track)
        state.stats.total_plays += kv.second.count;
    state.stats.unique_songs = (int)by_track.size();
    state.stats.unique_artists = (int)by_artist.size();

    // Hour / weekday histograms + unique days + streak.
    for (int i = 0; i < 24; ++i) state.stats.hour_plays[i] = 0;
    for (int i = 0; i < 7; ++i) state.stats.weekday_plays[i] = 0;
    std::set<int64_t> days;
    for (const auto &ev : state.play_log)
    {
        if (ev.ts < start_ts)
            continue;
        if (end_ts && ev.ts >= end_ts)
            continue;
        time_t t = (time_t)ev.ts;
        struct tm *tm = localtime(&t);
        if (!tm)
            continue;
        state.stats.hour_plays[tm->tm_hour]++;
        state.stats.weekday_plays[tm->tm_wday]++;
        int64_t day = ev.ts / 86400;
        days.insert(day);
    }
    state.stats.unique_days = (int)days.size();
    state.stats.peak_hour = -1;
    int peak_h = 0;
    for (int i = 0; i < 24; ++i)
        if (state.stats.hour_plays[i] > peak_h)
        {
            peak_h = state.stats.hour_plays[i];
            state.stats.peak_hour = i;
        }
    state.stats.peak_weekday = -1;
    int peak_w = 0;
    for (int i = 0; i < 7; ++i)
        if (state.stats.weekday_plays[i] > peak_w)
        {
            peak_w = state.stats.weekday_plays[i];
            state.stats.peak_weekday = i;
        }

    // Streak: consecutive days ending today with at least one play.
    int64_t today = (int64_t)time(nullptr) / 86400;
    int streak = 0;
    for (int64_t d = today; ; --d)
    {
        if (days.count(d))
            ++streak;
        else
            break;
        if (streak > 365)
            break;
    }
    state.stats.streak_days = streak;

    state.stats.repeat_rate = state.stats.unique_songs > 0
                                  ? (float)state.stats.total_plays / (float)state.stats.unique_songs
                                  : 0.0f;

    state.stats.anim_start = ImGui::GetTime();
    state.stats_dirty = false;
}

void draw_stats_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    if (state.stats_dirty)
        rebuild_stats(state);

    static uint64_t s_last_epoch = (uint64_t)-1;
    if (s_last_epoch != ui_page_epoch())
        state.stats.anim_start = ImGui::GetTime(); // replay when the page reappears
    s_last_epoch = ui_page_epoch();
    if (state.stats.anim_start <= 0.0)
        state.stats.anim_start = ImGui::GetTime();

    StatsAnim anim;
    anim.now = ImGui::GetTime();
    anim.start = state.stats.anim_start;
    if (ui_animations_enabled() &&
        (float)(anim.now - anim.start) < ui_anim_delay(1.15f) + ui_anim_duration(0.9f))
        ui_anim_request();

    const float gap = m.space_3;
    const float avail = ImGui::GetContentRegionAvail().x;
    const char *periods[] = {"All Time", "This Month", "This Week"};

    // Section headers fade/slide in independently.
    auto header = [&](const char *title, const char *meta, float delay) {
        const ImVec2 p = ImGui::GetCursorPos();
        const float sy = ImGui::GetCursorScreenPos().y;
        const float t = anim.stagger(delay, 0.5f) * anim.in_view(sy, 100.0f);
        const float off = (1.0f - t) * ui_anim_offset(12.0f);
        ImGui::SetCursorPosY(p.y + off);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        ui_section_header(title, meta);
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - off);
        return t;
    };

    const float title_y = ImGui::GetCursorPosY();
    const float combo_w = 190.0f * m.layout_scale;
    header("Listening Stats", "Your play counts over time", 0.0f);
    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - m.content_pad - combo_w, title_y + m.space_1));
    ImGui::SetNextItemWidth(combo_w);
    if (ImGui::BeginCombo("##statsperiod", period_label(state.stats_period)))
    {
        for (int i = 0; i < 3; ++i)
        {
            if (ImGui::Selectable(periods[i], state.stats_period == i))
            {
                state.stats_period = i;
                state.stats_dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    if (state.stats_period == 2)
    {
        const float wt = anim.stagger(0.12f, 0.4f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, wt);
        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        float bw = 34.0f * m.layout_scale;
        if (ui_ghost_button("<", ImVec2(bw, m.control_h)))
        {
            state.stats_week_offset++;
            state.stats_dirty = true;
        }
        ImGui::SameLine(0.0f, m.space_2);
        int64_t s = period_window_start(2, state.stats_week_offset);
        int64_t e = s + 7LL * 86400LL;
        char sbuf[32], ebuf[32];
        time_t ts = (time_t)s;
        time_t te = (time_t)(e - 1);
        struct tm tms_local{};
        struct tm tme_local{};
#ifdef P100_WINDOWS
        bool have_s = localtime_s(&tms_local, &ts) == 0;
        bool have_e = localtime_s(&tme_local, &te) == 0;
        struct tm *tms = have_s ? &tms_local : nullptr;
        struct tm *tme = have_e ? &tme_local : nullptr;
#else
        struct tm *tms = localtime_r(&ts, &tms_local);
        struct tm *tme = localtime_r(&te, &tme_local);
#endif
        if (tms) strftime(sbuf, sizeof(sbuf), "%b %d", tms); else sbuf[0] = '\0';
        if (tme) strftime(ebuf, sizeof(ebuf), "%b %d", tme); else ebuf[0] = '\0';
        std::string range = std::string(sbuf) + " – " + ebuf;
        ImGui::TextUnformatted(range.c_str());
        ImGui::SameLine(0.0f, m.space_2);
        if (state.stats_week_offset > 0)
        {
            if (ui_ghost_button(">", ImVec2(bw, m.control_h)))
            {
                state.stats_week_offset--;
                state.stats_dirty = true;
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ui_ghost_button(">", ImVec2(bw, m.control_h));
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();
    }

    const auto &st = state.stats;
    const double now = anim.now;
    if (st.total_plays == 0)
    {
        ui_empty_state(UiIcon::Stats, "No plays recorded yet", "Listen to something and your stats will appear here");
        return;
    }

    // Shared helper for jumping from a stats element to the artist page.
    auto goto_artist = [&](const std::string &name) {
        ++state.search_id;
        state.queued_artist_search = name;
        state.artist_search_dirty = true;
        state.artist_search_active = true;
        state.artist_search_loading = true;
        std::atomic_store(&state.artist_search_results,
                          std::make_shared<const std::vector<ArtistSummary>>());
        ui_invalidate_forward(state);
        state.current_tab = Tab::Artists;
        state.repaint_requested = true;
    };

    // ------------------------------------------------------------------
    // KPI cards: staggered pop-in with count-up numbers, hover lift and a
    // click ripple so every interaction feels physical.
    // ------------------------------------------------------------------
    char peak_hour_buf[16];
    if (st.peak_hour >= 0)
        std::snprintf(peak_hour_buf, sizeof(peak_hour_buf), "%02d:00", st.peak_hour);
    else
        std::snprintf(peak_hour_buf, sizeof(peak_hour_buf), "—");
    const char *weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string peak_day = (st.peak_weekday >= 0 && st.peak_weekday < 7) ? weekday_names[st.peak_weekday] : "—";
    std::string artist_val = st.top_artist.empty() ? "—" : st.top_artist;

    static constexpr int kCardCount = 8;
    static float s_card_hover[kCardCount] = {0};
    static double s_card_pulse[kCardCount] = {-1, -1, -1, -1, -1, -1, -1, -1};

    const float cards_y = ImGui::GetCursorScreenPos().y + m.space_2;
    const int cols = avail >= 640.0f * m.layout_scale ? 4 : (avail >= 420.0f * m.layout_scale ? 2 : 1);
    const float card_w = cols == 4 ? (avail - gap * 3.0f) / 4.0f
                        : cols == 2 ? (avail - gap) / 2.0f : avail;
    const float card_h = 88.0f * m.layout_scale;
    const float card_x = ImGui::GetCursorScreenPos().x;

    auto card_clock = [&](int idx) { return anim.stagger(0.06f + (float)idx * 0.05f, 0.5f); };

    auto card_at = [&](int idx, const char *label, const std::string &value, bool opens_artist) {
        const int r = idx / cols;
        const int c = idx % cols;
        const float x = card_x + c * (card_w + gap);
        const float y = cards_y + r * (card_h + gap);

        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(9100 + idx);
        ImGui::Dummy(ImVec2(card_w, card_h));
        const bool hov = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        ImGui::PopID();

        float &hv = s_card_hover[idx];
        const float target = hov ? 1.0f : 0.0f;
        hv = ui_animations_enabled() ? ui_anim_approach(hv, target, 16.0f) : target;
        if (fabsf(hv - target) > 0.004f)
            ui_anim_request();

        if (clicked)
        {
            s_card_pulse[idx] = now;
            ui_anim_request();
            if (opens_artist && !st.top_artist.empty())
                goto_artist(st.top_artist);
        }
        const float pulse = (ui_animations_enabled() && s_card_pulse[idx] >= 0.0)
                                ? (float)((now - s_card_pulse[idx]) / 0.5)
                                : 1.0f;
        if (pulse < 1.0f)
            ui_anim_request();

        const float t = card_clock(idx) * anim.in_view(y, 140.0f);
        stats_card(label, value, x, y, card_w, card_h, t, hv, pulse);
    };

    auto fmt_int = [](int v) {
        std::string s = std::to_string(v);
        for (int i = (int)s.size() - 3; i > 0; i -= 3)
            s.insert((size_t)i, ",");
        return s;
    };
    auto counted = [&](int idx, int v) {
        return fmt_int((int)std::lround((float)v * card_clock(idx)));
    };
    char rr[16];
    std::snprintf(rr, sizeof(rr), "%.2fx", st.repeat_rate * card_clock(7));

    card_at(0, "Total plays", counted(0, st.total_plays), false);
    card_at(1, "Unique songs", counted(1, st.unique_songs), false);
    card_at(2, "Unique artists", counted(2, st.unique_artists), false);
    card_at(3, "Top artist", artist_val, true);
    card_at(4, "Peak hour", std::string(peak_hour_buf), false);
    card_at(5, "Peak day", peak_day, false);
    card_at(6, "Streak (days)", counted(6, st.streak_days), false);
    card_at(7, "Repeat rate", std::string(rr), false);
    const int card_rows = (kCardCount + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(card_x, cards_y + card_rows * (card_h + gap) + m.space_4));

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // ------------------------------------------------------------------
    // Top artists: rows slide in from the left, bars grow, hover reveals an
    // accent wash + chevron, click springs a ripple and opens the artist.
    // ------------------------------------------------------------------
    header("Top Artists", nullptr, 0.34f);
    {
        const int n = std::min((int)st.top_artists.size(), 8);
        const int maxp = n > 0 ? st.top_artists[0].second : 1;
        const float bar_h = 26.0f * m.layout_scale;
        const float row_gap = 5.0f * m.layout_scale;
        const float label_w = 150.0f * m.layout_scale;
        const float count_w = 46.0f * m.layout_scale;
        const float bar_area = std::max(20.0f, avail - label_w - count_w - m.space_3);
        const float font_s = 16.0f * m.text_scale;

        for (int i = 0; i < n; ++i)
        {
            const std::string &name = st.top_artists[i].first;
            const int p = st.top_artists[i].second;
            const float frac = maxp > 0 ? (float)p / (float)maxp : 0.0f;

            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::PushID(9200 + i);
            ImGui::Dummy(ImVec2(avail, bar_h + row_gap));
            const bool hov = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked();
            ImGui::PopID();

            const float t = anim.pop(0.40f + (float)i * 0.045f, 0.5f) * anim.in_view(pos.y, 110.0f);
            const float hover = hov ? 1.0f : 0.0f;
            const float slide = (1.0f - t) * ui_anim_offset(16.0f);
            const float y = pos.y + (bar_h + row_gap - bar_h) * 0.5f;

            // Hover wash behind the row.
            if (hover > 0.01f)
            {
                dl->AddRectFilled(ImVec2(pos.x - m.space_2, y),
                                  ImVec2(pos.x + avail, y + bar_h),
                                  ui_overlay_u32(0.045f * hover * t),
                                  m.radius_sm);
                dl->AddRectFilled(ImVec2(pos.x - m.space_2, y + ui_px(4.0f)),
                                  ImVec2(pos.x - m.space_2 + ui_px(2.0f), y + bar_h - ui_px(4.0f)),
                                  ui_accent_u32(hover * t), 1.0f);
            }

            std::string ell = ui_ellipsize(g_ui_fonts.body, 16.0f, name, label_w - ui_px(6.0f));
            dl->AddText(g_ui_fonts.body, font_s,
                        ImVec2(pos.x + slide + ui_px(4.0f) * hover,
                               y + (bar_h - font_s * 1.2f) * 0.5f),
                        ui_with_alpha(hov ? ui_text_u32() : ui_text_dim_u32(), t), ell.c_str());

            const float bx = pos.x + label_w + m.space_2 + slide;
            const float bw = std::max(ui_px(3.0f), std::min(bar_area, bar_area * frac * t));
            dl->AddRectFilled(ImVec2(bx, y + ui_px(3.0f)), ImVec2(bx + bar_area, y + bar_h - ui_px(3.0f)),
                              ui_overlay_u32(0.05f * t), ui_px(4.0f));
            const ImU32 fill_col = ui_accent_u32((0.62f + 0.38f * hover) * t);
            dl->AddRectFilled(ImVec2(bx, y + ui_px(3.0f)), ImVec2(bx + bw, y + bar_h - ui_px(3.0f)),
                              fill_col, ui_px(4.0f));
            // Bright cap on the bar tip.
            if (bw > ui_px(6.0f))
                dl->AddRectFilled(ImVec2(bx + bw - ui_px(3.0f), y + ui_px(3.0f)),
                                  ImVec2(bx + bw, y + bar_h - ui_px(3.0f)),
                                  ui_with_alpha(IM_COL32(255, 255, 255, 255), 0.35f * t), ui_px(2.0f));

            std::string cnt = std::to_string(p);
            ImVec2 cs = g_ui_fonts.body->CalcTextSizeA(font_s, FLT_MAX, 0.0f, cnt.c_str());
            const float count_alpha = (0.55f + 0.45f * hover) * t;
            dl->AddText(g_ui_fonts.body, font_s,
                        ImVec2(pos.x + avail - cs.x, y + (bar_h - font_s * 1.2f) * 0.5f),
                        ui_with_alpha(hov ? ui_accent_text_u32() : ui_text_muted_u32(), count_alpha),
                        cnt.c_str());

            if (hov)
            {
                // Little chevron hint on the right edge.
                const float cxp = pos.x + avail - count_w - m.space_4;
                dl->AddText(g_ui_fonts.body, font_s, ImVec2(cxp, y + (bar_h - font_s * 1.2f) * 0.5f),
                            ui_with_alpha(ui_accent_text_u32(), 0.8f), "›");
            }

            if (clicked)
                goto_artist(name);
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
    }

    // ------------------------------------------------------------------
    // Listening by hour: each bar springs up with a small stagger, hovering
    // brightens it and shows its exact count.
    // ------------------------------------------------------------------
    header("Listening by Hour", nullptr, 0.48f);
    {
        int maxh = 1;
        for (int i = 0; i < 24; ++i)
            maxh = std::max(maxh, st.hour_plays[i]);
        const float chart_h = 120.0f * m.layout_scale;
        const float label_h = 16.0f * m.layout_scale;
        const float bar_w = (avail - m.space_1 * 23.0f) / 24.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float base_y = origin.y + chart_h;

        for (int i = 0; i < 24; ++i)
        {
            const float x = origin.x + (float)i * (bar_w + m.space_1);
            ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
            ImGui::PushID(9300 + i);
            ImGui::Dummy(ImVec2(bar_w + m.space_1, chart_h + label_h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();

            const float t = anim.pop(0.54f + (float)i * 0.012f, 0.55f) * anim.in_view(origin.y, 120.0f);
            const float frac = (float)st.hour_plays[i] / (float)maxh;
            const float bh = std::max(ui_px(2.0f), std::min(chart_h, chart_h * frac * t));
            const bool peak = (i == st.peak_hour);
            const ImU32 col = peak ? ui_accent_u32(t)
                                   : ui_accent_u32((hov ? 0.95f : 0.42f) * t);
            dl->AddRectFilled(ImVec2(x, base_y - bh), ImVec2(x + bar_w, base_y), col, ui_px(2.0f));
            if (peak || hov)
                dl->AddRectFilled(ImVec2(x, base_y - bh), ImVec2(x + bar_w, base_y - bh + ui_px(2.0f)),
                                  ui_with_alpha(IM_COL32(255, 255, 255, 255), 0.55f * t), ui_px(1.0f));

            if (i % 3 == 0)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%02d", i);
                dl->AddText(g_ui_fonts.body, 11.0f * m.text_scale,
                            ImVec2(x, base_y + ui_px(3.0f)),
                            ui_with_alpha(ui_text_muted_u32(), (0.6f + 0.4f * hov) * t), buf);
            }

            if (hov && st.hour_plays[i] > 0)
            {
                char cnt[32];
                std::snprintf(cnt, sizeof(cnt), "%d %s", st.hour_plays[i],
                              st.hour_plays[i] == 1 ? "play" : "plays");
                ImVec2 csz = g_ui_fonts.body->CalcTextSizeA(12.0f * m.text_scale, FLT_MAX, 0.0f, cnt);
                float tx = x + (bar_w - csz.x) * 0.5f;
                tx = std::max(origin.x, std::min(tx, origin.x + avail - csz.x));
                float ty = std::max(origin.y + ui_px(2.0f), base_y - bh - csz.y - ui_px(4.0f));
                dl->AddText(g_ui_fonts.body, 12.0f * m.text_scale, ImVec2(tx, ty),
                            ui_accent_text_u32(), cnt);
            }
        }

        // Baseline.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, base_y + label_h));
        ImGui::Dummy(ImVec2(avail, m.space_3));
    }

    // ------------------------------------------------------------------
    // Listening by day: same spring, with labels that fade in with the bars.
    // ------------------------------------------------------------------
    header("Listening by Day", nullptr, 0.60f);
    {
        const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int maxw = 1;
        for (int i = 0; i < 7; ++i)
            maxw = std::max(maxw, st.weekday_plays[i]);
        const float chart_h = 90.0f * m.layout_scale;
        const float label_h = 18.0f * m.layout_scale;
        const float step = m.space_2;
        const float bar_w = (avail - step * 6.0f) / 7.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float base_y = origin.y + chart_h;

        for (int i = 0; i < 7; ++i)
        {
            const float x = origin.x + (float)i * (bar_w + step);
            ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
            ImGui::PushID(9400 + i);
            ImGui::Dummy(ImVec2(bar_w + step, chart_h + label_h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();

            const float t = anim.pop(0.66f + (float)i * 0.05f, 0.55f) * anim.in_view(origin.y, 110.0f);
            const float frac = (float)st.weekday_plays[i] / (float)maxw;
            const float bh = std::max(ui_px(2.0f), std::min(chart_h, chart_h * frac * t));
            const bool peak = (i == st.peak_weekday);
            const ImU32 col = peak ? ui_accent_u32(t)
                                   : ui_accent_u32((hov ? 0.95f : 0.42f) * t);
            dl->AddRectFilled(ImVec2(x, base_y - bh), ImVec2(x + bar_w, base_y), col, ui_px(3.0f));
            if (peak || hov)
                dl->AddRectFilled(ImVec2(x, base_y - bh), ImVec2(x + bar_w, base_y - bh + ui_px(2.0f)),
                                  ui_with_alpha(IM_COL32(255, 255, 255, 255), 0.55f * t), ui_px(2.0f));

            ImVec2 ts = g_ui_fonts.body->CalcTextSizeA(12.0f * m.text_scale, FLT_MAX, 0.0f, names[i]);
            dl->AddText(g_ui_fonts.body, 12.0f * m.text_scale,
                        ImVec2(x + (bar_w - ts.x) * 0.5f, base_y + ui_px(3.0f)),
                        ui_with_alpha(hov ? ui_accent_text_u32() : ui_text_muted_u32(),
                                      (0.6f + 0.4f * hov) * t),
                        names[i]);

            if (hov && st.weekday_plays[i] > 0)
            {
                char cnt[32];
                std::snprintf(cnt, sizeof(cnt), "%d", st.weekday_plays[i]);
                ImVec2 csz = g_ui_fonts.body->CalcTextSizeA(13.0f * m.text_scale, FLT_MAX, 0.0f, cnt);
                float tx = x + (bar_w - csz.x) * 0.5f;
                float ty = std::max(origin.y + ui_px(2.0f), base_y - bh - csz.y - ui_px(4.0f));
                dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale, ImVec2(tx, ty),
                            ui_accent_text_u32(), cnt);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, base_y + label_h));
        ImGui::Dummy(ImVec2(avail, m.space_3));
    }

    // ------------------------------------------------------------------
    // Most played tracks: the shared staggered row reveal, refreshed for
    // every period/week selection.
    // ------------------------------------------------------------------
    header("Most Played Tracks",
           (std::to_string(std::min((int)st.tracks.size(), 50)) + " tracks").c_str(), 0.74f);

    const int list_size = std::min((int)st.tracks.size(), 50);
    const int max_plays = st.plays.empty() ? 1 : st.plays.front();
    const std::string reveal_key = "stats:" + std::to_string(state.stats_period) + ":" +
                                   std::to_string(state.stats_week_offset);
    const double reveal_start = ui_anim_track_list_reveal(
        reveal_key.c_str(), st.tracks, (uint64_t)(st.anim_start * 1000.0));
    for (int i = 0; i < list_size; ++i)
    {
        TrackRowOptions opt;
        opt.show_rank = true;
        opt.rank = i + 1;
        opt.show_duration = false;
        opt.show_plays = false;
        opt.art_size = 44.0f * m.layout_scale;
        // This view plays on single click; see the matching note in
        // ui_youtube.cpp -- avoid a second play path for touch taps.
        opt.auto_play_double_click = false;
        opt.reveal_start = reveal_start;
        opt.reveal_index = i;
        opt.right_text = std::to_string(st.plays[i]) + (st.plays[i] == 1 ? " play" : " plays");
        opt.progress = max_plays > 0 ? (float)st.plays[i] / (float)max_plays : 0.0f;
        TrackRowResult row = ui_track_row(state, st.tracks[i], i, opt);
        if (row.clicked && !row.ctrl_clicked && !row.shift_clicked)
        {
            state.current_playlist_source = PlaylistSource::STATS;
            ui_play_track(state, st.tracks[i], i);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_4));
}
