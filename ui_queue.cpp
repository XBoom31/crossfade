#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_layout.h"
#include "ui_theme.h"

#include <algorithm>
#include <string>

void draw_queue_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    smooth_scroll_child("PageContent");

    const int count = (int)state.play_queue.size();
    const std::string meta = std::to_string(count) + (count == 1 ? " track" : " tracks");
    if (ui_section_header("Up Next", meta.c_str(), count > 0 ? "Clear queue" : nullptr))
        queue_clear(state);

    if (!state.current_track_id.empty())
    {
        ImGui::TextColored(ui_c().text_muted, "NOW PLAYING");
        ui_push_medium(20.0f);
        ImGui::TextUnformatted(state.current_title.empty() ? "Unknown Title" : state.current_title.c_str());
        ImGui::PopFont();
        ui_push_small();
        ImGui::TextColored(ui_c().text_dim, "%s",
                           state.current_artist.empty() ? "Unknown Artist" : state.current_artist.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
    }

    if (state.play_queue.empty())
    {
        ui_empty_state(UiIcon::Queue, "Nothing queued",
                       "Right-click a track and choose Play next or Add to queue");
        return;
    }

    const float art = 44.0f * m.layout_scale;
    const float title_line = 24.0f * m.text_scale * 1.22f;
    const float meta_line = 18.0f * m.text_scale * 1.24f;
    const float row_h = std::max(art, title_line + m.space_1 + meta_line) + m.row_pad_y * 2.0f;
    const float pitch = row_h + m.row_gap;
    const double reveal_start =
        ui_anim_track_list_reveal("queue", state.play_queue, (uint64_t)(uintptr_t)&state.play_queue);

    const float start_y = ImGui::GetCursorPosY();
    ImGuiListClipper clipper;
    clipper.Begin(count, pitch);
    while (clipper.Step())
    {
        ImGui::SetCursorPosY(start_y + clipper.DisplayStart * pitch);
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Track &track = state.play_queue[i];
            TrackRowOptions opt;
            opt.show_duration = true;
            opt.art_size = art;
            opt.show_art_play = false;
            opt.auto_play_double_click = false;
            opt.draggable = true;
            opt.drag_type = "P100_QUEUE";
            opt.reveal_start = reveal_start;
            opt.reveal_index = i;
            TrackRowResult row = ui_track_row(state, track, i, opt);
            if (row.drop_received)
                queue_move(state, row.drag_from, row.drop_to);
            if (row.double_clicked || (row.clicked && ui_touch_tap_recent()))
                state.queue_play_index = i;
        }
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    draw_scroll_edge_fades();
}
