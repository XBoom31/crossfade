#include "ui.h"

#include "party.h"
#include "party_util.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_layout.h"
#include "ui_notify.h"
#include "ui_theme.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "qrcodegen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// Party mode — host UI
//
// The HTTP server, guest sessions and rate limits live in party.cpp and
// party_server.cpp. This file only draws the host controls and hands the
// settings over to party_start(). Every queue mutation still happens on
// this (the main) thread.
// ----------------------------------------------------------------------

namespace
{

void party_help(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopStyleColor();
}

void party_gap()
{
    ImGui::Dummy(ImVec2(0.0f, ui_m().space_4));
}

void begin_party_card(const char *id, ImVec2 size)
{
    const UiMetrics &m = ui_m();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_c().surface);
    ImGui::PushStyleColor(ImGuiCol_Border, ui_c().border_soft);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, m.radius_lg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(m.space_5, m.space_4));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders |
                                    ImGuiChildFlags_AlwaysUseWindowPadding |
                                    ImGuiChildFlags_AutoResizeY);
}

void end_party_card()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

float text_width(const char *text, float base_size = 18.0f)
{
    return ui_measure_width(g_ui_fonts.body, base_size, text ? text : "");
}

// Draws a QR code with a white quiet zone. Returns the drawn square size, or
// 0 when the payload cannot be encoded. Drawing straight into ImDrawList
// avoids a texture upload and keeps the QR crisp at any scale.
float draw_qr_code(ImDrawList *dl, const std::string &text, ImVec2 top_left, float max_size)
{
    if (text.empty() || max_size <= 0.0f)
        return 0.0f;
    try
    {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();
        const int border = 4; // quiet zone, in modules
        const int total = modules + border * 2;
        const float px = std::floor(max_size / (float)total);
        if (px < 2.0f)
            return 0.0f;
        const float side = px * (float)total;

        dl->AddRectFilled(top_left, ImVec2(top_left.x + side, top_left.y + side),
                          IM_COL32(255, 255, 255, 255), ui_m().radius_sm);
        const ImU32 dark = IM_COL32(10, 12, 16, 255);
        for (int y = 0; y < modules; ++y)
        {
            for (int x = 0; x < modules; ++x)
            {
                if (!qr.getModule(x, y))
                    continue;
                const ImVec2 a(top_left.x + (float)(x + border) * px,
                               top_left.y + (float)(y + border) * px);
                dl->AddRectFilled(a, ImVec2(a.x + px, a.y + px), dark);
            }
        }
        return side;
    }
    catch (const std::exception &)
    {
        return 0.0f;
    }
    catch (...)
    {
        return 0.0f;
    }
}

void draw_qr_centered(const std::string &url, float max_size)
{
    const UiMetrics &m = ui_m();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float size = std::min(max_size, avail);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();

    const float x = cursor.x + (avail - size) * 0.5f;
    const ImVec2 top_left(x, cursor.y);
    const float drawn = draw_qr_code(dl, url, top_left, size);
    if (drawn <= 0.0f)
    {
        ui_ghost_button("Could not draw the QR code", ImVec2(size, m.control_h));
        return;
    }
    // The square is `drawn` wide inside a `size` box; advance past the drawn
    // height only so the URL sits close to the code.
    ImGui::Dummy(ImVec2(avail, drawn + m.space_2));
}

std::string guest_count_label(int guests)
{
    return std::to_string(guests) + (guests == 1 ? " guest" : " guests");
}

void draw_live_controls(AppState &state)
{
    const UiMetrics &m = ui_m();
    const PartySettings &ps = party_active_settings();
    const PartyStatus st = party_status();

    begin_party_card("##party_live_card", ImVec2(0, 0));
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // Live dot + headline.
        const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        const float dot_r = 5.0f * m.layout_scale;
        dl->AddCircleFilled(ImVec2(dot_pos.x + dot_r, dot_pos.y + ImGui::GetTextLineHeight() * 0.55f),
                            dot_r, ui_accent_u32(), 16);
        dl->AddCircle(ImVec2(dot_pos.x + dot_r, dot_pos.y + ImGui::GetTextLineHeight() * 0.55f),
                      dot_r * 2.1f, ui_accent_u32(0.22f), 20, 1.5f);
        ImGui::Dummy(ImVec2(dot_r * 2.0f + m.space_2, 0.0f));
        ImGui::SameLine();
        ui_push_semibold(22.0f);
        ImGui::TextUnformatted("Party is live");
        ImGui::PopFont();
        ImGui::SameLine();
        const std::string guests = guest_count_label(st.guests);
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "· %s", guests.c_str());
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        ImGui::TextColored(ui_c().text_dim, "Scan this with a phone camera");
        ImGui::Dummy(ImVec2(0.0f, m.space_2));

        draw_qr_centered(st.url, std::min(290.0f * m.layout_scale,
                                          ImGui::GetContentRegionAvail().x * 0.72f));

        // The URL is also shown as text so it can be read out or copied by
        // hand when the clipboard is unavailable (some Wayland/X11 setups).
        ui_push_small();
        const float url_w = std::min(ImGui::GetContentRegionAvail().x,
                                     text_width(st.url.c_str()) + m.space_4);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (ImGui::GetContentRegionAvail().x - url_w) * 0.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + url_w);
        ImGui::TextUnformatted(st.url.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        const float bw = 150.0f * m.layout_scale;
        const float total = bw * 2.0f + m.space_2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - total) * 0.5f));
        if (ui_primary_button("Copy link", ImVec2(bw, m.button_h)))
        {
            ImGui::SetClipboardText(st.url.c_str());
            ui_notify(UiNotifyType::Success, "Party link copied",
                      "Paste it into a message if a guest can't scan the code.", 4.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_secondary_button("Fullscreen", ImVec2(bw, m.button_h)))
            state.party_fullscreen = true;

        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, m.space_2));

        const std::string added = std::to_string(st.requests_added) +
                                  (st.requests_added == 1 ? " song requested"
                                                          : " songs requested");
        ImGui::TextColored(ui_c().text_dim, "%s", added.c_str());
        ImGui::SameLine();
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "· %s:%d · %s", st.ip.c_str(), st.port,
                           ps.name.c_str());
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        if (ui_ghost_button("End party", ImVec2(130.0f * m.layout_scale, m.control_h)))
        {
            party_stop();
            state.party_fullscreen = false;
            state.repaint_requested = true;
        }
        party_help("Guests keep the page open, but requests stop working as soon as the party ends. The link and guest names are discarded; only the settings below are saved.");
    }
    end_party_card();
}

void draw_pending_requests(AppState &state)
{
    const UiMetrics &m = ui_m();
    std::vector<PartyPendingRequest> pending = party_pending_requests();
    if (pending.empty())
        return;

    party_gap();
    const std::string meta = std::to_string(pending.size()) +
                             (pending.size() == 1 ? " waiting" : " waiting");
    if (ui_section_header("Waiting for approval", meta.c_str(),
                          pending.size() > 1 ? "Approve all" : nullptr))
        party_approve_all();

    for (const PartyPendingRequest &p : pending)
    {
        ImGui::PushID(p.request_id.c_str());
        begin_party_card("##party_pending", ImVec2(0, 0));
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float art = 44.0f * m.layout_scale;
        const float buttons_w = 86.0f * m.layout_scale * 2.0f + m.space_2;
        const float text_w = std::max(80.0f, row_w - art - buttons_w - m.space_4);

        ui_push_medium(18.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
        ImGui::TextUnformatted(p.title.empty() ? "Unknown title" : p.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ui_push_small();
        std::string sub = p.artist.empty() ? "Unknown artist" : p.artist;
        if (!p.by.empty())
            sub += " · " + p.by;
        if (p.duration_sec > 0)
        {
            char dur[24];
            std::snprintf(dur, sizeof(dur), " · %d:%02d", p.duration_sec / 60,
                          p.duration_sec % 60);
            sub += dur;
        }
        ImGui::TextColored(ui_c().text_dim, "%s", sub.c_str());
        ImGui::PopFont();

        ImGui::SameLine(row_w - buttons_w);
        if (ui_primary_button("Approve", ImVec2(86.0f * m.layout_scale, m.control_h)))
            party_approve(p.request_id);
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Reject", ImVec2(86.0f * m.layout_scale, m.control_h)))
            party_reject(p.request_id);
        end_party_card();
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, m.space_2));
    }
}

void draw_guest_queue(AppState &state)
{
    const UiMetrics &m = ui_m();
    int guest_items = 0;
    for (const Track &t : state.play_queue)
        if (t.from_party)
            ++guest_items;
    if (guest_items == 0)
        return;

    party_gap();
    const std::string meta = std::to_string(guest_items) +
                             (guest_items == 1 ? " song" : " songs");
    ui_section_header("Guest requests in Up Next", meta.c_str());

    int shown = 0;
    for (const Track &t : state.play_queue)
    {
        if (!t.from_party)
            continue;
        if (shown++ >= 5)
            break;
        ImGui::PushID(t.id.c_str());
        ImGui::Dummy(ImVec2(0.0f, m.space_1));
        ui_push_medium(17.0f);
        ImGui::TextUnformatted(t.title.empty() ? "Unknown title" : t.title.c_str());
        ImGui::PopFont();
        ui_push_small();
        std::string sub = t.artist.empty() ? "Unknown artist" : t.artist;
        if (!t.requested_by.empty())
            sub += " · " + t.requested_by;
        ImGui::TextColored(ui_c().text_dim, "%s", sub.c_str());
        ImGui::PopFont();
        ImGui::PopID();
    }
    if (guest_items > 5)
    {
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "and %d more in the queue",
                           guest_items - 5);
        ImGui::PopFont();
    }
}

void draw_setup(AppState &state)
{
    const UiMetrics &m = ui_m();
    PartySettings &ps = state.settings.party;
    const float content_w = std::min(780.0f * m.layout_scale,
                                     ImGui::GetContentRegionAvail().x);
    const float field_w = std::min(420.0f * m.layout_scale, content_w);
    bool save = false;

    ui_section_header("Party mode", "Let guests add songs from their phones");
    party_help("Start a party and show the QR code. Guests scan it, paste a YouTube link, and their songs join Up Next. Nothing leaves your local network — there is no account, no cloud, and no telemetry.");
    party_gap();

    begin_party_card("##party_setup", ImVec2(0, 0));
    {
        ImGui::TextColored(ui_c().text_muted, "Party name");
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputText("##party_name", &ps.name);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            ps.name = party_util::clean_utf8(ps.name, 60);
            if (ps.name.empty())
                ps.name = "crossfade party";
            save = true;
        }

        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        ImGui::TextColored(ui_c().text_muted, "Network");
        std::vector<PartyInterfaceInfo> ifaces = party_interfaces();
        std::vector<std::string> labels;
        labels.push_back("Automatic (best network)");
        int current = 0;
        for (int i = 0; i < (int)ifaces.size(); ++i)
        {
            std::string label = ifaces[i].label;
            if (!ifaces[i].is_private)
                label += "  (public address)";
            labels.push_back(label);
            if (!ps.interface_ip.empty() && ifaces[i].ip == ps.interface_ip)
                current = (int)labels.size() - 1;
        }
        if (!ps.interface_ip.empty() && current == 0)
        {
            labels.push_back("Saved: " + ps.interface_ip);
            current = (int)labels.size() - 1;
        }
        std::vector<const char *> ptrs;
        ptrs.reserve(labels.size());
        for (const std::string &l : labels)
            ptrs.push_back(l.c_str());
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::Combo("##party_iface", &current, ptrs.data(), (int)ptrs.size()))
        {
            if (current == 0)
                ps.interface_ip.clear();
            else if (current - 1 < (int)ifaces.size())
                ps.interface_ip = ifaces[current - 1].ip;
            else
                ps.interface_ip.clear();
            save = true;
        }
        party_help("Pick the Wi-Fi or hotspot address phones can reach. Automatic skips virtual adapters like Docker and VPNs.");

        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        ImGui::TextColored(ui_c().text_muted, "Port");
        ImGui::SetNextItemWidth(160.0f * m.layout_scale);
        ImGui::InputInt("##party_port", &ps.port, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            ps.port = std::max(1024, std::min(65535, ps.port));
            save = true;
        }

        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        ImGui::TextColored(ui_c().text_muted, "Who can add songs");
        if (ImGui::Checkbox("Host approves every request", &ps.require_approval))
            save = true;
        party_help("When on, new songs wait here until you approve them. When off, they join Up Next immediately.");

        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderInt("##party_max_guest", &ps.max_per_guest, 0, 20,
                         ps.max_per_guest == 0 ? "Per guest  unlimited" : "Per guest  %d songs");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderInt("##party_cooldown", &ps.cooldown_sec, 0, 60,
                         ps.cooldown_sec == 0 ? "Cooldown  off" : "Cooldown  %d s between adds");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderInt("##party_max_minutes", &ps.max_video_minutes, 0, 60,
                         ps.max_video_minutes == 0 ? "Max video length  unlimited"
                                                   : "Max video length  %d min");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderInt("##party_max_queue", &ps.max_queue, 0, 300,
                         ps.max_queue == 0 ? "Queue cap  unlimited" : "Queue cap  %d songs");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;

        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        ImGui::TextColored(ui_c().text_muted, "Guest experience");
        if (ImGui::Checkbox("Let guests search YouTube", &ps.allow_search))
            save = true;
        party_help("Turns the guest search box into a paste-only field when off. Search runs through the same local backend as the app.");
        if (ImGui::Checkbox("Allow duplicate songs", &ps.allow_duplicates))
            save = true;
        if (ImGui::Checkbox("Show guest names next to songs", &ps.show_names))
            save = true;

        ImGui::Dummy(ImVec2(0.0f, m.space_4));
        if (ui_primary_button("Start party", ImVec2(180.0f * m.layout_scale, m.button_h)))
        {
            std::string error;
            if (party_start(state.settings.party, error))
            {
                state.party_fullscreen = true;
                state.repaint_requested = true;
                save = true;
            }
            else
            {
                state.repaint_requested = true;
            }
        }
        ImGui::SameLine();
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted,
                           "The link is shown here and can be reopened with P.");
        ImGui::PopFont();

        const PartyStatus st = party_status();
        if (st.state == PartyRunState::Error && !st.error.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, m.space_2));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s", st.error.c_str());
            ImGui::PopStyleColor();
        }
    }
    end_party_card();

    party_gap();
    ui_section_header("How it works", nullptr);
    party_help("1. Start the party and show the QR code to your guests.");
    party_help("2. They scan it, paste a YouTube or YouTube Music link, and optionally set a name.");
    party_help("3. Approved songs are interleaved round-robin between guests, below anything you queued yourself.");
    party_help("4. Press P any time to reopen the party page. Ending the party invalidates every guest link.");
    party_gap();
    party_help("The party server only listens on your selected LAN address. Links use a random token, the session cookie is HttpOnly/SameSite=Strict, and state-changing requests require an X-CF header, so other websites cannot queue songs.");

    if (save)
        save_settings(state.settings);
}

} // namespace

void draw_party_panel(AppState &state)
{
    const UiMetrics &m = ui_m();

    if (!party_running())
    {
        draw_setup(state);
        return;
    }

    draw_live_controls(state);
    draw_pending_requests(state);
    draw_guest_queue(state);

    party_gap();
    const PartySettings &ps = party_active_settings();
    ui_section_header("Party rules", "Stop the party to change these");
    ui_push_small();
    ImGui::TextColored(ui_c().text_dim, "Approval: %s",
                       ps.require_approval ? "host approves each song" : "songs join immediately");
    ImGui::TextColored(ui_c().text_dim, "Limits: %s, %s, %s",
                       ps.max_per_guest == 0 ? "unlimited songs per guest"
                                             : (std::to_string(ps.max_per_guest) + " songs per guest").c_str(),
                       ps.cooldown_sec == 0 ? "no cooldown"
                                            : (std::to_string(ps.cooldown_sec) + "s cooldown").c_str(),
                       ps.max_video_minutes == 0 ? "no length limit"
                                                 : (std::to_string(ps.max_video_minutes) + " min max").c_str());
    ImGui::TextColored(ui_c().text_dim, "Queue cap: %s",
                       ps.max_queue == 0 ? "unlimited" : (std::to_string(ps.max_queue) + " songs").c_str());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, m.space_5));
    draw_scroll_edge_fades();
}

void draw_party_fullscreen(AppState &state)
{
    // Opening the overlay happens from a button click. That same click is
    // still "pressed" when this function first draws, so an outside-click
    // dismiss would close the overlay instantly. Skip exactly the first frame.
    static bool was_open = false;
    if (!state.party_fullscreen)
    {
        was_open = false;
        return;
    }
    const bool just_opened = !was_open;
    was_open = true;
    if (!party_running())
    {
        state.party_fullscreen = false;
        was_open = false;
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    const UiMetrics &m = ui_m();
    const PartyStatus st = party_status();
    if (st.url.empty())
        return;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_c().bg);
    ImGui::Begin("##party_fullscreen", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;

    // Close affordance, top right.
    ImGui::SetCursorScreenPos(ImVec2(w - m.content_pad - m.control_h,
                                     m.content_pad));
    if (ui_icon_button("##party_fs_close", UiIcon::Close, m.control_h, false, 0,
                       "Close (Esc)"))
        state.party_fullscreen = false;

    const std::string guests = guest_count_label(st.guests);
    ui_push_small();
    const float tag_w = text_width(guests.c_str()) + m.space_4 * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(m.content_pad, m.content_pad));
    ImGui::PushStyleColor(ImGuiCol_Button, ui_overlay_vec4(0.05f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_overlay_vec4(0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui_overlay_vec4(0.08f));
    ImGui::Button(guests.c_str(), ImVec2(tag_w, m.control_h * 0.8f));
    ImGui::PopStyleColor(3);
    ImGui::PopFont();

    const float max_qr = std::min(w * 0.58f, h * 0.58f);
    const float gap = m.space_5;
    const float title_h = ImGui::GetTextLineHeight() * 1.2f;
    const float url_h = ImGui::GetTextLineHeight() * 1.2f;
    const float hint_h = ImGui::GetTextLineHeight() * 1.2f;
    const float block_h = title_h + gap * 0.5f + max_qr + gap * 0.6f + url_h +
                          gap * 0.4f + hint_h;
    const float top = std::max(m.content_pad, (h - block_h) * 0.5f);
    const ImVec2 qr_top((w - max_qr) * 0.5f, top + title_h + gap * 0.5f);

    ui_push_display();
    const char *title = party_active_settings().name.c_str();
    const float title_w = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorScreenPos(ImVec2((w - title_w) * 0.5f, top));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();

    const float drawn = draw_qr_code(dl, st.url, qr_top, max_qr);

    ui_push_medium(22.0f);
    const float url_w = text_width(st.url.c_str(), 22.0f);
    ImGui::SetCursorScreenPos(ImVec2((w - url_w) * 0.5f,
                                     qr_top.y + drawn + m.space_4));
    ImGui::TextColored(ui_c().text_dim, "%s", st.url.c_str());
    ImGui::PopFont();

    ui_push_small();
    const char *hint = "Scan with a phone camera to add songs · Esc to close";
    const float hint_w = text_width(hint);
    ImGui::SetCursorScreenPos(ImVec2((w - hint_w) * 0.5f,
                                     qr_top.y + drawn + m.space_4 + url_h +
                                         m.space_3));
    ImGui::TextColored(ui_c().text_muted, "%s", hint);
    ImGui::PopFont();

    // Click anywhere outside the close button to dismiss as well.
    if (!just_opened && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered())
        state.party_fullscreen = false;

    ImGui::End();
}

void draw_party_chip(AppState &state)
{
    if (!party_running())
        return;
    const UiMetrics &m = ui_m();
    ImGuiIO &io = ImGui::GetIO();
    const AppLayout layout = ui_compute_layout(state, io);
    const PartyStatus st = party_status();

    char label[96];
    std::snprintf(label, sizeof(label), "Party · %d %s", st.guests,
                  st.guests == 1 ? "guest" : "guests");
    const float h = std::max(32.0f * m.layout_scale, m.control_h * 0.78f);
    const float w = text_width(label, 15.0f) + h * 1.9f;
    const ImVec2 pos(io.DisplaySize.x - w - m.content_pad,
                     layout.bottom_pos.y - h - m.space_2);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, h * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##party_chip", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::InvisibleButton("##party_chip_hit", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui_col(ui_c().surface), h * 0.5f);
    dl->AddRect(p0, p1, ui_accent_u32(hovered ? 0.65f : 0.32f), h * 0.5f, 0,
                1.0f);
    dl->AddCircleFilled(ImVec2(p0.x + h * 0.62f, p0.y + h * 0.5f),
                        3.4f * m.layout_scale, ui_accent_u32(), 12);
    dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                ImVec2(p0.x + h * 1.0f, p0.y + (h - 15.0f * m.text_scale) * 0.5f),
                ui_text_u32(), label);

    if (clicked)
    {
        state.current_tab = Tab::Party;
        state.repaint_requested = true;
    }
    if (hovered)
        ImGui::SetTooltip("Open party mode");

    ImGui::End();
}
