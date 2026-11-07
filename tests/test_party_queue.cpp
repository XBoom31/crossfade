#include "party_queue.h"
#include "test_util.h"
using testutil::check;
#include "ytdlp.h"

#include <cstdio>
#include <string>
#include <vector>

Track make(const std::string &id, const std::string &guest, const std::string &title = "")
{
    Track t;
    t.id = id;
    t.title = title.empty() ? id : title;
    if (!guest.empty())
    {
        t.guest_id = guest;
        t.from_party = true;
    }
    return t;
}

std::string ids(const std::vector<Track> &q)
{
    std::string out;
    for (const Track &t : q)
    {
        if (!out.empty())
            out += ",";
        out += t.id;
    }
    return out;
}

int main()
{
    {
        std::vector<Track> q;
        check(party_queue::round_for_guest(q, "a") == 0, "empty round");
        check(party_queue::count_guest_items(q, "a") == 0, "empty count");
        check(party_queue::insert_fair(q, make("a1", "a")) == 0, "first insert index");
        check(ids(q) == "a1", "first insert order");
        check(q[0].from_party, "guest item flagged from_party");
        check(q[0].party_round == 0, "first round is 0");
    }

    {
        std::vector<Track> q;
        party_queue::insert_fair(q, make("a1", "a"));
        party_queue::insert_fair(q, make("b1", "b"));
        party_queue::insert_fair(q, make("a2", "a"));
        party_queue::insert_fair(q, make("b2", "b"));
        check(ids(q) == "a1,b1,a2,b2", "round-robin interleave");
        check(party_queue::round_for_guest(q, "a") == 2, "round count for a");
        check(party_queue::count_guest_items(q, "b") == 2, "count guest b");
    }

    {
        std::vector<Track> q;
        q.push_back(make("host", ""));
        party_queue::insert_fair(q, make("a1", "a"));
        party_queue::insert_fair(q, make("b1", "b"));
        check(ids(q) == "host,a1,b1", "host pins stay first");
    }

    {
        std::vector<Track> q;
        q.push_back(make("a1", "a"));
        party_queue::insert_fair(q, make("host2", ""));
        party_queue::insert_fair(q, make("a2", "a"));
        check(ids(q) == "a1,host2,a2" || ids(q) == "a1,a2,host2",
              "guest + host insertion stays coherent");
    }
    {
        std::vector<Track> q;
        Track pl1 = make("pl1", "");
        pl1.from_queue_source = true;
        Track pl2 = make("pl2", "");
        pl2.from_queue_source = true;
        q.push_back(pl1);
        q.push_back(pl2);
        party_queue::insert_fair(q, make("a1", "a"));
        party_queue::insert_fair(q, make("b1", "b"));
        check(ids(q) == "a1,b1,pl1,pl2", "guest requests go ahead of playlist tracks");
    }

    {
        std::vector<Track> q;
        q.push_back(make("pin", ""));
        Track pl1 = make("pl1", "");
        pl1.from_queue_source = true;
        q.push_back(pl1);
        party_queue::insert_fair(q, make("a1", "a"));
        check(ids(q) == "pin,a1,pl1", "play-next pins stay ahead of guests and playlist");
    }

    {
        std::vector<Track> q;
        q.push_back(make("a1", "a"));
        check(party_queue::is_duplicate(q, "a1", "", false), "duplicate in queue");
        check(!party_queue::is_duplicate(q, "a1", "", true), "duplicates allowed");
        check(party_queue::is_duplicate(q, "now", "now", false), "duplicate now playing");
        check(!party_queue::is_duplicate(q, "other", "now", false), "new id is not duplicate");
        check(!party_queue::is_duplicate(q, "", "", false), "empty id is not duplicate");
    }

    {
        // Long queue: fairness stays stable and bounded by the guest count.
        std::vector<Track> q;
        for (int i = 0; i < 120; ++i)
            party_queue::insert_fair(q, make("a" + std::to_string(i), "a"));
        check(q.size() == 120, "long queue size");
        check(party_queue::round_for_guest(q, "a") == 120, "long queue round");
        check(party_queue::count_guest_items(q, "a") == 120, "long queue count");
    }

    if (testutil::failures == 0)
        std::printf("party_queue: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
