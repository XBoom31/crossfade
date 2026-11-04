#pragma once

#include "ytdlp.h"

#include <string>
#include <vector>

// Pure fair-queue helpers. Kept free of UI/app state so they can be tested
// directly (tests/test_party_queue.cpp).
namespace party_queue
{
// Number of songs this guest already has in the queue. This is the guest's
// "round" for a newly added request.
int round_for_guest(const std::vector<Track> &queue, const std::string &guest_id);

// Insert a guest request with round-robin fairness:
//   * after the leading host-pinned "Play next" items,
//   * before the first guest item with a higher round,
//   * stable (FIFO) within a round.
// Returns the insertion index.
int insert_fair(std::vector<Track> &queue, Track item);

bool is_duplicate(const std::vector<Track> &queue, const std::string &video_id,
                  const std::string &now_playing_id, bool allow_duplicates);

int count_guest_items(const std::vector<Track> &queue, const std::string &guest_id);

} // namespace party_queue
