#include "party_queue.h"

#include <algorithm>

namespace party_queue
{

int round_for_guest(const std::vector<Track> &queue, const std::string &guest_id)
{
    if (guest_id.empty())
        return 0;
    int n = 0;
    for (const Track &t : queue)
    {
        if (t.from_party && t.guest_id == guest_id)
            ++n;
    }
    return n;
}

int insert_fair(std::vector<Track> &queue, Track item)
{
    if (item.guest_id.empty())
    {
        queue.push_back(std::move(item));
        return (int)queue.size() - 1;
    }
    item.party_round = round_for_guest(queue, item.guest_id);
    item.from_party = true;

    // Leading host items are the "Play next" pins. Guest requests go below
    // them, never above the host's explicit intent.
    size_t start = 0;
    while (start < queue.size() && queue[start].guest_id.empty())
        ++start;

    size_t pos = queue.size();
    for (size_t i = start; i < queue.size(); ++i)
    {
        const Track &t = queue[i];
        if (!t.guest_id.empty() && t.party_round > item.party_round)
        {
            pos = i;
            break;
        }
    }
    queue.insert(queue.begin() + (std::ptrdiff_t)pos, std::move(item));
    return (int)pos;
}

bool is_duplicate(const std::vector<Track> &queue, const std::string &video_id,
                  const std::string &now_playing_id, bool allow_duplicates)
{
    if (allow_duplicates || video_id.empty())
        return false;
    if (!now_playing_id.empty() && video_id == now_playing_id)
        return true;
    for (const Track &t : queue)
    {
        if (t.id == video_id)
            return true;
    }
    return false;
}

int count_guest_items(const std::vector<Track> &queue, const std::string &guest_id)
{
    if (guest_id.empty())
        return 0;
    int n = 0;
    for (const Track &t : queue)
    {
        if (t.from_party && t.guest_id == guest_id)
            ++n;
    }
    return n;
}

} // namespace party_queue
