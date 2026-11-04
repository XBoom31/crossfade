#!/usr/bin/env python3
"""Protocol regression test for the shared backend command loop.

The entry points only differ in auth setup; every command is handled by
cf_backend.core.run_command_loop. This test monkeypatches the metadata and
stream helpers, drives the loop with a fixed command list, and checks that the
replies are JSON objects with the expected shape and error kinds.
"""

import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py"))

import cf_backend.core as core  # noqa: E402


class FakeYT:
    def __init__(self):
        self.rated = []
        self.created = []

    def get_liked_songs(self, limit=200):
        return {"tracks": [{"videoId": "liked1"}], "limit": limit}

    def get_library_playlists(self, limit=100):
        return {"playlists": [{"playlistId": "LP1"}], "limit": limit}

    def get_playlist(self, playlist_id, limit=1000):
        return {"id": playlist_id, "limit": limit}

    def rate_song(self, video_id, rating):
        self.rated.append((video_id, rating))

    def create_playlist(self, **kwargs):
        self.created.append(kwargs)
        return "PL_NEW"

    def delete_playlist(self, playlist_id):
        self.deleted = playlist_id

    def edit_playlist(self, playlist_id, **kwargs):
        self.edited = (playlist_id, kwargs)

    def add_playlist_items(self, playlist_id, videoIds=None, duplicates=False):
        self.added = (playlist_id, videoIds)

    def remove_playlist_items(self, playlist_id, videos):
        self.removed = (playlist_id, videos)


def run(commands, yt=None):
    out = io.StringIO()
    core.run_command_loop(yt or FakeYT(), stdin=io.StringIO("\n".join(commands) + "\n"),
                          stdout=out)
    return [json.loads(line) for line in out.getvalue().splitlines()]


def main():
    failures = 0

    def check(cond, what):
        nonlocal failures
        if not cond:
            print("FAIL:", what, file=sys.stderr)
            failures += 1

    core.get_home_tracks = lambda yt, limit=40: [{"id": "home", "limit": limit}]
    core.search_tracks = lambda yt, query, limit=20: [{"id": "s", "q": query}]
    core.search_all = lambda yt, query, limit=20: {
        "songs": [{"id": "s", "q": query}], "albums": [], "artists": []}
    core.search_artists = lambda yt, query, limit=20: [{"id": "a", "q": query}]
    core.get_radio_tracks = lambda yt, video_id, limit=50: [{"id": "r", "v": video_id}]
    core.get_album_data = lambda yt, browse_id: {"id": browse_id, "tracks": []}
    core.album_for_track = lambda yt, video_id: {"album": video_id}
    core.get_artist_data = lambda yt, channel_id: {"id": channel_id, "songs": []}
    core.get_artist_songs = lambda yt, browse_id, offset, count: {
        "id": browse_id, "offset": offset, "count": count}
    core.artist_for_track = lambda yt, video_id: {"artist": video_id}
    core.get_track_meta = lambda yt, video_id: {"id": video_id}
    core.get_lyrics = lambda yt, video_id: {"id": video_id, "lines": []}
    core.get_stream_url = lambda video_id, fresh=False: {"url": f"url:{video_id}"}
    core.get_charts_tracks = lambda yt, limit=50: [{"id": "chart"}]
    core.get_mood_categories = lambda yt: [{"id": "mood"}]
    core.get_mood_playlists = lambda yt, params: [{"id": "moodpl", "params": params}]

    out = run([
        "recommendations",
        "recommendations 7",
        "search hello world",
        "search_all hello",
        "artist_search someone",
        "radio VID",
        "album ALB",
        "album_of VID",
        "artist ART",
        "artist_songs ART 10 5",
        "artist_of VID",
        "play VID",
        "play_fresh VID",
        "lyrics VID",
        "meta VID",
        "liked",
        "library_playlists",
        "playlist PL1",
        "charts",
        "mood_categories",
        "mood_playlists PARAMS",
        'rate_song {"videoId":"VID","rating":"LIKE"}',
        'create_playlist {"title":"T","privacyStatus":"PRIVATE"}',
        'delete_playlist {"playlistId":"PL1"}',
        'edit_playlist {"playlistId":"PL1","title":"T2"}',
        'add_playlist_items {"playlistId":"PL1","videoIds":["V1"]}',
        'remove_playlist_items {"playlistId":"PL1","videos":[{"videoId":"V1"}]}',
        "unknown",
        "search",
        "quit",
        "recommendations",
    ])

    check(out[0][0]["limit"] == 40, "default recommendations limit")
    check(out[1][0]["limit"] == 7, "explicit recommendations limit")
    check(out[2][0]["q"] == "hello world", "search argument")
    check(out[3]["songs"][0]["q"] == "hello", "search_all argument")
    check(out[4][0]["q"] == "someone", "artist_search")
    check(out[5][0]["v"] == "VID", "radio")
    check(out[6]["id"] == "ALB", "album")
    check(out[7]["album"] == "VID", "album_of")
    check(out[8]["id"] == "ART", "artist")
    check(out[9]["offset"] == 10 and out[9]["count"] == 5, "artist_songs")
    check(out[10]["artist"] == "VID", "artist_of")
    check(out[11]["url"] == "url:VID", "play")
    check(out[12]["url"] == "url:VID", "play_fresh")
    check(out[13]["id"] == "VID", "lyrics")
    check(out[14]["id"] == "VID", "meta")
    check(out[15]["tracks"][0]["videoId"] == "liked1", "liked")
    check(out[16]["playlists"][0]["playlistId"] == "LP1", "library_playlists")
    check(out[17]["id"] == "PL1", "playlist")
    check(out[18][0]["id"] == "chart", "charts")
    check(out[19][0]["id"] == "mood", "mood_categories")
    check(out[20][0]["params"] == "PARAMS", "mood_playlists")
    check(out[21]["status"] == "ok", "rate_song")
    check(out[22]["playlistId"] == "PL_NEW", "create_playlist")
    check(out[23]["status"] == "ok", "delete_playlist")
    check(out[24]["status"] == "ok", "edit_playlist")
    check(out[25]["status"] == "ok", "add_playlist_items")
    check(out[26]["status"] == "ok", "remove_playlist_items")
    check(out[27]["error"] == "Unknown command: unknown", "unknown command")
    check(out[27]["kind"] == "backend", "unknown command kind")
    check(out[28]["error"] == "Missing query", "missing query")
    check(out[28]["kind"] == "backend", "missing query kind")
    check(len(out) == 29, "quit stops the loop")

    # Auth/network classifier is shared and deterministic.
    check(core._classify_error("auth failed") == "auth", "auth classifier")
    check(core._classify_error("connection reset") == "network", "network classifier")
    check(core._classify_error("boom") == "backend", "backend classifier")

    # Both entry points must stay thin and call the shared loop.
    for name in ("p100-backend.py", "p100-backend-oauth.py"):
        path = os.path.join(ROOT, name)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        check(len(text.splitlines()) < 170, f"{name} stays thin")
        check("run_command_loop" in text, f"{name} uses shared loop")

    if failures:
        print(f"backend protocol: {failures} failure(s)", file=sys.stderr)
        return 1
    print("backend protocol: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
