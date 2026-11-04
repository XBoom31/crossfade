"""Shared command loop and YouTube Music helpers for both auth backends.

The entry points (p100-backend.py and p100-backend-oauth.py) only differ in
how they create the YTMusic session. Everything below is shared so a command
cannot work in one backend and silently fail in the other.
"""

import base64
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor

YTDLP = os.environ.get('P100_YTDLP') or 'yt-dlp'

# video_id -> (resolved_at, url); avoids re-running yt-dlp for repeat plays.
_STREAM_TTL = 7200  # googlevideo URLs are valid ~6h; probed before caching
_stream_cache = {}


def ensure_config_dir(path):
    """Create the config directory owner-only (0700)."""
    os.makedirs(path, mode=0o700, exist_ok=True)
    try:
        os.chmod(path, 0o700)
    except OSError:
        pass


def secure_auth_file(path):
    """Best-effort 0600 for cookie/OAuth token files."""
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def _album_fields(track):
    """Return (album_name, album_browse_id) from a ytmusicapi item.

    Search results carry album as a dict ({'name', 'id'}), while tracks
    inside an album carry it as a plain string (the id is the parent album).
    """
    album = track.get('album')
    if isinstance(album, dict):
        return album.get('name') or '', album.get('id') or ''
    if isinstance(album, str):
        return album, ''
    return '', ''


def _duration_sec(track):
    """Parse ytmusicapi's various duration representations into seconds."""
    secs = track.get('duration_seconds')
    if isinstance(secs, (int, float)) and secs > 0:
        return int(secs)
    dur = track.get('duration')
    if isinstance(dur, (int, float)) and dur > 0:
        return int(dur)
    if isinstance(dur, str) and dur:
        try:
            parts = [int(p) for p in dur.split(':')]
        except ValueError:
            return 0
        total = 0
        for p in parts:
            total = total * 60 + p
        return total
    return 0


def _artist_id(track):
    """First artist/channel id from a ytmusicapi track item, if present."""
    artists = track.get('artists') or []
    if artists and isinstance(artists[0], dict):
        return artists[0].get('id') or artists[0].get('browseId') or ''
    if artists and isinstance(artists[0], str):
        return ''
    return ''


# Error kinds sent alongside {'error': ...} so the C++ UI can react
# appropriately (sign-in banner vs. offline message vs. retry) without
# substring-guessing the message text.
_AUTH_MARKERS = ('auth', 'cookie', 'oauth', 'browser.json', 'forbidden',
                 'unauthorized', '401', '403', 'sign in', 'sign-in', 'logged')
_NETWORK_MARKERS = ('name resolution', 'name or service not known',
                    'temporary failure', 'network is unreachable',
                    'connection refused', 'connection reset', 'connection aborted',
                    'timed out', 'timeout', 'getaddrinfo failed',
                    'max retries exceeded with url', 'connectionerror',
                    'read error', 'httpsconnectionpool', 'failed to resolve',
                    'network')


def _classify_error(msg):
    m = (msg or '').lower()
    if any(k in m for k in _AUTH_MARKERS):
        return 'auth'
    if any(k in m for k in _NETWORK_MARKERS):
        return 'network'
    return 'backend'


def _err(msg, kind=None):
    """Standard error payload; kind defaults to a message-based guess."""
    return {'error': msg, 'kind': kind or _classify_error(msg)}


def _thumb_url(item):
    thumbs = item.get('thumbnails') or []
    if thumbs and isinstance(thumbs[-1], dict):
        return thumbs[-1].get('url', '')
    return ''


def track_to_dict(track):
    video_id = track.get('videoId') or track.get('id')
    title = track.get('title', 'Unknown')
    artists = track.get('artists', [])
    if isinstance(artists, list) and artists and isinstance(artists[0], dict):
        artist_name = ', '.join(a['name'] for a in artists)
    else:
        artist_name = str(artists[0]) if artists else ''
    # Search/home items use "thumbnails"; radio/watch-playlist items use the
    # singular "thumbnail" key. Accept both so radio rows get cover art.
    thumbnails = track.get('thumbnails') or track.get('thumbnail') or []
    thumb_url = ''
    if thumbnails and isinstance(thumbnails[-1], dict):
        thumb_url = thumbnails[-1].get('url', '')
    album_name, album_id = _album_fields(track)
    return {
        'id': video_id,
        'title': title,
        'artist': artist_name,
        'artist_id': _artist_id(track),
        'album': album_name,
        'album_id': album_id,
        'track_number': track.get('trackNumber') or 0,
        'thumbnail_url': thumb_url,
        'duration_sec': _duration_sec(track),
        'url': f'https://www.youtube.com/watch?v={video_id}'
    }


def get_track_meta(yt, video_id):
    """Metadata-only lookup for party mode. Never resolves a stream."""
    vid = (video_id or '').strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{11}', vid):
        return _err('Invalid video id', kind='backend')
    details = {}
    try:
        song = yt.get_song(vid)
        details = (song or {}).get('videoDetails') or {}
    except Exception:
        details = {}
    if not details:
        try:
            for item in yt.search(vid, filter='songs', limit=5) or []:
                if (item.get('videoId') or item.get('id') or '') == vid:
                    d = track_to_dict(item)
                    return {
                        'ok': True,
                        'id': vid,
                        'title': d['title'],
                        'artist': d['artist'],
                        'durationSec': d['duration_sec'],
                        'thumbUrl': d['thumbnail_url'],
                    }
        except Exception as e:
            return _err(str(e))
        return _err('Video not found', kind='not_found')
    title = details.get('title') or ''
    artist = details.get('author') or ''
    try:
        length = int(details.get('lengthSeconds') or 0)
    except (TypeError, ValueError):
        length = 0
    thumbs = ((details.get('thumbnail') or {}).get('thumbnails') or [])
    thumb = thumbs[-1].get('url', '') if thumbs else f'https://i.ytimg.com/vi/{vid}/hqdefault.jpg'
    if not title:
        return _err('Video not found', kind='not_found')
    if details.get('isLive'):
        return _err('Live streams are not supported', kind='backend')
    return {
        'ok': True,
        'id': vid,
        'title': title,
        'artist': artist,
        'durationSec': length,
        'thumbUrl': thumb,
    }


def get_album_data(yt, browse_id):
    """Fetch an album header + its ordered track list by browse id (MPRE...)."""
    if not browse_id:
        return {'error': 'Missing album id'}
    try:
        album = yt.get_album(browse_id)
    except Exception as e:
        return {'error': f'Failed to fetch album: {str(e)}'}
    if not album:
        return {'error': 'Album not found'}

    artists = album.get('artists') or []
    if artists and isinstance(artists[0], dict):
        artist_name = ', '.join(a.get('name', '') for a in artists)
    elif artists:
        artist_name = ', '.join(str(a) for a in artists)
    else:
        artist_name = ''

    thumbs = album.get('thumbnails') or []
    thumb_url = ''
    if thumbs and isinstance(thumbs[-1], dict):
        thumb_url = thumbs[-1].get('url', '')

    album_title = album.get('title') or 'Unknown Album'
    tracks = []
    for item in album.get('tracks') or []:
        if not isinstance(item, dict):
            continue
        if not (item.get('videoId') or item.get('id')):
            continue
        try:
            t = track_to_dict(item)
        except Exception:
            continue
        # Album items don't repeat the parent browse id; add it so the UI
        # can offer "view album" on every row without another lookup.
        t['album'] = t.get('album') or album_title
        t['album_id'] = browse_id
        if not t.get('track_number'):
            t['track_number'] = len(tracks) + 1
        tracks.append(t)

    if not tracks:
        return {'error': 'Album has no playable tracks.'}

    year = album.get('year') or ''
    return {
        'browse_id': browse_id,
        'title': album_title,
        'artist': artist_name,
        'year': str(year),
        'track_count': album.get('trackCount') or len(tracks),
        'duration': album.get('duration') or '',
        'description': album.get('description') or '',
        'thumbnail_url': thumb_url,
        'audio_playlist_id': album.get('audioPlaylistId') or '',
        'tracks': tracks,
    }


def album_for_track(yt, video_id):
    """Resolve the album browse id for a song, when it isn't already known.

    Home/radio items don't always carry album metadata, so fall back to a
    song search and match the exact video id (or the same title+artist).
    """
    if not video_id:
        return {'error': 'Missing video_id'}
    title = artist = ''
    try:
        watch = yt.get_watch_playlist(videoId=video_id, limit=1) or {}
        seed = (watch.get('tracks') or [None])[0] or {}
        title = seed.get('title') or ''
        artists = seed.get('artists') or []
        if artists and isinstance(artists[0], dict):
            artist = artists[0].get('name') or ''
        name, album_id = _album_fields(seed)
        if album_id:
            return {'album_id': album_id, 'album_name': name}
    except Exception:
        pass

    if not title:
        return {'error': 'Could not identify the track.'}
    try:
        results = yt.search((title + ' ' + artist).strip(), filter='songs', limit=15)
    except Exception as e:
        return {'error': f'Album lookup failed: {str(e)}'}

    for r in results:
        if not isinstance(r, dict):
            continue
        name, album_id = _album_fields(r)
        if not album_id:
            continue
        if r.get('videoId') == video_id:
            return {'album_id': album_id, 'album_name': name}
    # No exact video id: accept an identical (normalised) title + artist.
    for r in results:
        if not isinstance(r, dict):
            continue
        name, album_id = _album_fields(r)
        if not album_id or _norm(r.get('title')) != _norm(title):
            continue
        cand_artists = r.get('artists') or []
        cand_artist = cand_artists[0].get('name', '') if cand_artists and isinstance(cand_artists[0], dict) else ''
        if not artist or _norm(artist) in _norm(cand_artist):
            return {'album_id': album_id, 'album_name': name}
    return {'error': 'No album found for this track.'}


def get_artist_data(yt, channel_id):
    """Fetch an artist header, top songs and top albums/singles."""
    if not channel_id:
        return {'error': 'Missing artist id'}
    try:
        artist = yt.get_artist(channel_id)
    except Exception as e:
        return {'error': f'Failed to fetch artist: {str(e)}'}
    if not artist:
        return {'error': 'Artist not found'}

    name = artist.get('name') or 'Unknown Artist'
    description = artist.get('description') or ''

    songs = []
    song_sec = artist.get('songs') or {}
    songs_browse_id = song_sec.get('browseId') or ''
    for item in song_sec.get('results') or []:
        if not isinstance(item, dict):
            continue
        if not (item.get('videoId') or item.get('id')):
            continue
        try:
            songs.append(track_to_dict(item))
        except Exception:
            continue

    albums = []
    seen = set()
    for key, default_type in (('albums', 'Album'), ('singles', 'Single')):
        section = artist.get(key) or {}
        for item in section.get('results') or []:
            if not isinstance(item, dict):
                continue
            browse_id = item.get('browseId') or ''
            if not browse_id or browse_id in seen:
                continue
            seen.add(browse_id)
            albums.append({
                'browse_id': browse_id,
                'title': item.get('title') or 'Unknown Album',
                'year': str(item.get('year') or ''),
                'thumbnail_url': _thumb_url(item),
                'type': item.get('type') or default_type,
            })

    return {
        'browse_id': channel_id,
        'name': name,
        'description': description,
        'subscribers': artist.get('subscribers') or '',
        'monthly_listeners': artist.get('monthlyListeners') or '',
        'thumbnail_url': _thumb_url(artist),
        'songs': songs,
        'songs_browse_id': songs_browse_id,
        'albums': albums,
    }


_ARTIST_SONGS_CACHE = {}


def get_artist_songs(yt, browse_id, offset, count):
    """Fetch a slice of the artist's full songs shelf.

    The artist header only returns a handful of top songs. Its songs browseId
    is a playlist-like endpoint with the full list; cache that list in the
    backend so consecutive "load more" clicks are cheap.
    """
    if not browse_id:
        return {'error': 'Missing songs browse id'}
    cached = _ARTIST_SONGS_CACHE.get(browse_id)
    if cached is None:
        try:
            playlist = yt.get_playlist(browse_id, limit=None)
        except Exception as e:
            return {'error': f'Failed to fetch artist songs: {str(e)}'}
        tracks = []
        for item in playlist.get('tracks') or []:
            if not isinstance(item, dict):
                continue
            if not (item.get('videoId') or item.get('id')):
                continue
            try:
                tracks.append(track_to_dict(item))
            except Exception:
                continue
        if len(_ARTIST_SONGS_CACHE) >= 8:
            _ARTIST_SONGS_CACHE.clear()
        _ARTIST_SONGS_CACHE[browse_id] = tracks
        cached = tracks
    offset = max(0, int(offset))
    count = max(0, int(count))
    return cached[offset:offset + count]


def artist_for_track(yt, video_id):
    """Resolve the artist channel id for a song."""
    if not video_id:
        return {'error': 'Missing video_id'}
    seed = {}
    try:
        watch = yt.get_watch_playlist(videoId=video_id, limit=1) or {}
        seed = (watch.get('tracks') or [None])[0] or {}
        artists = seed.get('artists') or []
        if artists and isinstance(artists[0], dict):
            aid = artists[0].get('id') or artists[0].get('browseId') or ''
            if aid:
                return {'artist_id': aid, 'artist_name': artists[0].get('name') or ''}
    except Exception:
        pass

    title = seed.get('title') or ''
    artist_name = seed.get('artists', [{}])[0].get('name', '') if seed.get('artists') else ''
    if not title:
        return {'error': 'Could not identify the track.'}
    try:
        results = yt.search((title + ' ' + artist_name).strip(), filter='songs', limit=10)
    except Exception as e:
        return {'error': f'Artist lookup failed: {str(e)}'}
    for r in results:
        if not isinstance(r, dict) or r.get('videoId') != video_id:
            continue
        artists = r.get('artists') or []
        if artists and isinstance(artists[0], dict):
            aid = artists[0].get('id') or ''
            if aid:
                return {'artist_id': aid, 'artist_name': artists[0].get('name') or ''}
    return {'error': 'No artist found for this track.'}


def search_artists(yt, query, limit=20):
    """Search YouTube Music artists."""
    if not query:
        return {'error': 'Missing query'}
    try:
        results = yt.search(query, filter='artists', limit=limit)
    except Exception as e:
        return {'error': f'Artist search failed: {str(e)}'}

    out = []
    seen = set()
    for r in results or []:
        if not isinstance(r, dict):
            continue
        browse_id = r.get('browseId') or ''
        if not browse_id or browse_id in seen:
            continue
        seen.add(browse_id)
        out.append({
            'browse_id': browse_id,
            'name': r.get('artist') or r.get('title') or 'Unknown Artist',
            'thumbnail_url': _thumb_url(r),
            'subscribers': r.get('subscribers') or '',
        })
    return out


def search_albums(yt, query, limit=20):
    if not query:
        return {'error': 'Missing query'}
    try:
        results = yt.search(query, filter='albums', limit=limit)
    except Exception as e:
        return {'error': f'Album search failed: {str(e)}'}

    out = []
    seen = set()
    for r in results or []:
        if not isinstance(r, dict):
            continue
        browse_id = r.get('browseId') or ''
        if not browse_id or browse_id in seen:
            continue
        seen.add(browse_id)
        artists = r.get('artists') or []
        if isinstance(artists, list) and artists and isinstance(artists[0], dict):
            artist_name = ', '.join(a.get('name', '') for a in artists)
        elif artists:
            artist_name = ', '.join(str(a) for a in artists)
        else:
            artist_name = ''
        out.append({
            'browse_id': browse_id,
            'title': r.get('title') or 'Unknown Album',
            'artist': artist_name,
            'year': str(r.get('year') or ''),
            'thumbnail_url': _thumb_url(r),
            'track_count': r.get('trackCount') or 0,
        })
    return out


def search_all(yt, query, limit=20):
    """Search songs, albums and artists in one backend round-trip.

    The three searches are independent HTTP calls, so they run in parallel --
    total latency becomes the slowest single call instead of the sum of four
    (search_tracks itself issues two)."""
    if not query:
        return {'error': 'Missing query'}
    with ThreadPoolExecutor(max_workers=4) as pool:
        fut_songs = pool.submit(search_tracks, yt, query, limit)
        fut_albums = pool.submit(search_albums, yt, query, limit)
        fut_artists = pool.submit(search_artists, yt, query, limit)
        songs = fut_songs.result()
        albums = fut_albums.result()
        artists = fut_artists.result()
    if isinstance(songs, dict) and isinstance(albums, dict) and isinstance(artists, dict):
        return {'error': songs.get('error') or albums.get('error') or artists.get('error') or 'Search failed'}
    return {
        'songs': songs if isinstance(songs, list) else [],
        'albums': albums if isinstance(albums, list) else [],
        'artists': artists if isinstance(artists, list) else [],
    }


def get_charts_tracks(yt, limit=50):
    """
    YouTube Music's global charts (most-played songs). get_charts() returns
    sections keyed 'songs'/'videos' with {'rank', 'videoId', ...} items;
    normalize the songs section through track_to_dict like everywhere else.
    """
    try:
        charts = yt.get_charts()
    except Exception as e:
        return {'error': f"Failed to fetch charts: {str(e)}"}

    tracks = []
    seen_ids = set()
    for s in (charts or {}).get('songs', []):
        if not isinstance(s, dict):
            continue
        video_id = s.get('videoId')
        if not video_id or video_id in seen_ids or not s.get('title'):
            continue
        seen_ids.add(video_id)
        try:
            d = track_to_dict(s)
            d['rank'] = s.get('rank') or (len(tracks) + 1)
            tracks.append(d)
        except Exception:
            continue
        if len(tracks) >= limit:
            break

    if not tracks:
        # The mobile get_charts() endpoint only exposes 'songs' in some
        # regions/accounts. Elsewhere it returns chart *playlists* ("Daily
        # Top Music Videos - ..."); fall back to the first one's tracks.
        videos = (charts or {}).get('videos') or []
        for vp in videos:
            pid = (vp or {}).get('playlistId') if isinstance(vp, dict) else None
            if not pid:
                continue
            try:
                pl = yt.get_playlist(pid, limit=limit)
            except Exception:
                continue
            for item in (pl or {}).get('tracks') or []:
                if not isinstance(item, dict):
                    continue
                video_id = item.get('videoId')
                if not video_id or video_id in seen_ids or not item.get('title'):
                    continue
                seen_ids.add(video_id)
                try:
                    tracks.append(track_to_dict(item))
                except Exception:
                    continue
                if len(tracks) >= limit:
                    break
            if tracks:
                break

    if not tracks:
        return {'error': 'Charts returned no songs (may be unavailable in this region).'}
    return tracks


def get_mood_categories(yt):
    """Moods & genres: [{'title', 'params'}] — params feed get_mood_playlists."""
    try:
        return yt.get_mood_categories()
    except Exception as e:
        return {'error': f"Failed to fetch mood categories: {str(e)}"}


def get_mood_playlists(yt, params):
    """Playlists for one mood/genre category (params from get_mood_categories)."""
    if not params:
        return {'error': 'Missing mood params'}
    try:
        return yt.get_mood_playlists(params) or []
    except Exception as e:
        return {'error': f"Failed to fetch mood playlists: {str(e)}"}


def check_logged_in(yt):
    """
    Sanity check: confirms the server actually sees us as authenticated,
    rather than silently falling back to anonymous/generic behavior.
    Returns (True, None) or (False, error_message).
    """
    try:
        response = yt._send_request("account/account_menu", {})
        tracking = response.get('responseContext', {}).get('serviceTrackingParams', [])
        for service in tracking:
            if service.get('service') == 'GFEEDBACK':
                for param in service.get('params', []):
                    if param.get('key') == 'logged_in':
                        return param.get('value') == '1', None
        return False, "Could not find logged_in flag in response"
    except Exception as e:
        return False, str(e)


def get_home_tracks(yt, limit=40):
    """
    Mirrors the YouTube Music homepage: returns tracks pulled from the
    same sections shown there (Quick picks, Mixed for you, etc.),
    rather than deriving a radio mix from listening history.
    """
    try:
        sections = yt.get_home(limit=limit)
    except Exception as e:
        return {'error': f"Failed to fetch home feed: {str(e)}"}

    if not sections:
        return {'error': 'Home feed returned no sections. Try again once your account has some listening activity.'}

    tracks = []
    seen_ids = set()
    for section in sections:
        if not isinstance(section, dict):
            continue
        for item in section.get('contents') or []:
            if not isinstance(item, dict):
                # Skip non-track entries (dividers, malformed items, etc.)
                continue
            video_id = item.get('videoId')
            # Skip non-track items (e.g. whole playlists/albums with no videoId)
            if not video_id or video_id in seen_ids:
                continue
            seen_ids.add(video_id)
            try:
                tracks.append(track_to_dict(item))
            except Exception:
                # Don't let one malformed track kill the whole batch
                continue
            if len(tracks) >= limit:
                return tracks

    if not tracks:
        return {'error': 'No individual tracks found in home feed sections (they may be playlists/albums instead of songs).'}

    return tracks


def search_tracks(yt, query, limit=20):
    """
    Search both the YouTube Music song catalogue and regular YouTube.

    The 'songs' filter only returns items indexed in the YTM catalogue, so
    YouTube-only uploads (UGC videos, lyric/live clips, unofficial releases)
    never show up there -- even though the home feed and radio do surface
    them. Merge both result sets, songs first, de-duplicated by videoId.
    """
    tracks = []
    seen_ids = set()

    def _collect(filt):
        # Returns (results, error) so the merge below can preserve the
        # original semantics: error only surfaces when nothing was collected.
        try:
            return yt.search(query, filter=filt, limit=limit), None
        except Exception as e:
            return None, str(e)

    with ThreadPoolExecutor(max_workers=2) as pool:
        fut_songs = pool.submit(_collect, 'songs')
        fut_videos = pool.submit(_collect, 'videos')
        for fut in (fut_songs, fut_videos):
            results, err = fut.result()
            if results is None:
                if not tracks and err:
                    return {'error': err}
                continue
            for r in results:
                video_id = r.get('videoId') or r.get('id')
                if not video_id or video_id in seen_ids or not r.get('title'):
                    continue
                seen_ids.add(video_id)
                try:
                    tracks.append(track_to_dict(r))
                except Exception:
                    continue
    return tracks


def get_radio_tracks(yt, video_id, limit=50):
    """
    Starts a "radio" queue seeded by a specific song - the same feature as
    right-clicking a track in the actual app and choosing "Start radio".
    """
    try:
        radio = yt.get_watch_playlist(videoId=video_id, radio=True, limit=limit)
    except Exception as e:
        return {'error': f"Failed to start radio: {str(e)}"}

    if not radio or 'tracks' not in radio:
        return {'error': 'No radio tracks returned for this song.'}

    tracks = []
    for track in radio['tracks']:
        if not isinstance(track, dict):
            continue
        if not track.get('videoId'):
            continue
        try:
            tracks.append(track_to_dict(track))
        except Exception:
            continue

    if not tracks:
        return {'error': 'Radio returned no playable tracks.'}

    return tracks


def get_lyrics(yt, video_id):
    """
    Fetch lyrics for a song. Metadata only -- no stream resolution, no
    downloads, so this also works for tracks that play from the local audio
    cache.

    Several providers are tried in order and the first SYNCED (timestamped)
    result wins; if none of them has timestamps, the first plain result is
    used. Providers:
      1. YouTube Music native timed lyrics (public mobile browse)
      2. LRCLIB (community; synced, then plain)
      3. NetEase Cloud Music (synced LRC)
      4. Kugou (synced LRC)
      5. lyrics.ovh (plain)
      6. YouTube Music native plain (LyricFind)
    """
    try:
        watch = yt.get_watch_playlist(videoId=video_id) or {}
    except Exception as e:
        return {'error': f"Failed to look up lyrics: {str(e)}"}

    lyrics_field = watch.get('lyrics')
    browse_id = lyrics_field if isinstance(lyrics_field, str) else (lyrics_field or {}).get('browseId')
    track_name, artist_name, album_name, duration_sec = _extract_meta(
        (watch.get('tracks') or [None])[0])

    providers = (
        lambda: _ytm_timed_lyrics(browse_id),
        lambda: _lrclib_lyrics(artist_name, track_name, album_name, duration_sec),
        lambda: _netease_lyrics(track_name, artist_name, duration_sec),
        lambda: _kugou_lyrics(track_name, artist_name, duration_sec),
        lambda: _lyrics_ovh(artist_name, track_name),
        lambda: _ytm_plain_lyrics(yt, browse_id),
    )

    plain_fallback = None
    for provider in providers:
        try:
            result = provider()
        except Exception:
            result = None
        if not result:
            continue
        if result.get('synced'):
            return result
        if plain_fallback is None:
            plain_fallback = result
    return plain_fallback or {'none': True}


def _extract_meta(meta):
    """Pull (title, artist, album, duration_sec) out of a watch-playlist track."""
    track_name = artist_name = album_name = None
    duration_sec = 0
    if isinstance(meta, dict):
        track_name = meta.get('title')
        artists = meta.get('artists') or []
        if artists and isinstance(artists[0], dict):
            artist_name = artists[0].get('name')
        album = meta.get('album')
        if isinstance(album, dict):
            album_name = album.get('name')
        elif isinstance(album, str):
            album_name = album
        dur = meta.get('duration')
        if isinstance(dur, str) and ':' in dur:
            parts = dur.split(':')
            try:
                duration_sec = int(parts[0]) * 60 + int(parts[1])
            except ValueError:
                duration_sec = 0
    return track_name, artist_name, album_name, duration_sec


# ---- HTTP / matching helpers -------------------------------------------

_BROWSER_UA = ('Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
               '(KHTML, like Gecko) Chrome/120.0 Safari/537.36')
_YTM_MOBILE_UA = ('com.google.android.apps.youtube.music/7.21.50 '
                  '(Linux; U; Android 14) gzip')


def _http_json(url, headers=None, data=None, timeout=10):
    req_headers = {'User-Agent': _BROWSER_UA}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=data, headers=req_headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode('utf-8', 'replace'))
    except Exception:
        return None


def _find_key(obj, key):
    """Depth-first search for the first value stored under `key`."""
    stack = [obj]
    while stack:
        cur = stack.pop()
        if isinstance(cur, dict):
            if key in cur:
                return cur[key]
            stack.extend(cur.values())
        elif isinstance(cur, list):
            stack.extend(cur)
    return None


def _norm(s):
    """Normalise a title/artist for matching (drop case + punctuation)."""
    return re.sub(r'[\W_]+', '', (s or '').lower(), flags=re.UNICODE)


def _pick_song(candidates, track, artist, duration_sec, name_of, artists_of, dur_ms_of):
    """
    Choose the best search hit. Only candidates whose (normalised) title
    matches the wanted title are considered, so we never return lyrics for the
    wrong song. Among those, prefer an artist match and the closest duration.
    """
    want = _norm(track)
    if not want:
        return None
    best = None
    best_score = None
    for cand in candidates or []:
        try:
            name = _norm(name_of(cand))
            cand_artist = artists_of(cand)
            dur_ms = dur_ms_of(cand) or 0
        except Exception:
            continue
        if not name or (want not in name and name not in want):
            continue
        score = 0
        if name == want:
            score -= 1000
        if artist and _norm(artist) and _norm(artist) in _norm(cand_artist):
            score -= 200
        if duration_sec and dur_ms:
            score += abs(dur_ms / 1000.0 - duration_sec)
        if best_score is None or score < best_score:
            best_score = score
            best = cand
    return best


def _plain_result(source, text):
    lines = [{'t': 0, 'text': p}
             for p in (text or '').replace('\r\n', '\n').split('\n') if p.strip()]
    return {'synced': False, 'source': source, 'lines': lines} if lines else None


def _synced_from_lrc(source, lrc):
    lines = _parse_lrc(lrc or '')
    if not lines or not any(l['t'] > 0 for l in lines):
        return None
    return {'synced': True, 'source': source, 'lines': lines}


_LRC_ANY = re.compile(r'\[[^\]]*\]')


def _strip_lrc_tags(text):
    return _LRC_ANY.sub('', text or '').strip()


# ---- Providers ----------------------------------------------------------

def _ytm_timed_lyrics(browse_id):
    """
    YouTube Music's own timed lyrics. The browser-cookie auth path 400s on the
    mobile client, but an unauthenticated mobile browse of the lyrics page
    returns the public timed-lyrics payload.
    """
    if not browse_id:
        return None
    payload = json.dumps({
        'context': {'client': {
            'clientName': 'ANDROID_MUSIC', 'clientVersion': '7.21.50',
            'androidSdkVersion': 34, 'osName': 'Android', 'osVersion': '14',
            'hl': 'en', 'gl': 'US', 'utcOffsetMinutes': 0,
        }},
        'browseId': browse_id,
    }).encode('utf-8')
    data = _http_json('https://music.youtube.com/youtubei/v1/browse?alt=json',
                      headers={'Content-Type': 'application/json',
                               'User-Agent': _YTM_MOBILE_UA,
                               'Origin': 'https://music.youtube.com'},
                      data=payload)
    timed = _find_key(data or {}, 'timedLyricsData')
    if not timed:
        return None
    source = _find_key(data, 'sourceMessage') or 'YouTube'
    if isinstance(source, str) and source.lower().startswith('source:'):
        source = source.split(':', 1)[1].strip()

    synced_lines = []
    plain_lines = []
    for item in timed:
        if not isinstance(item, dict):
            continue
        text = item.get('lyricLine')
        if not text or not text.strip():
            continue
        try:
            start_ms = int((item.get('cueRange') or {}).get('startTimeMilliseconds') or 0)
        except (TypeError, ValueError):
            start_ms = 0
        plain_lines.append({'t': 0, 'text': text})
        synced_lines.append({'t': start_ms // 1000, '_ms': start_ms, 'text': text})
    if not plain_lines:
        return None
    if any(l['_ms'] > 0 for l in synced_lines):
        synced_lines.sort(key=lambda l: l['_ms'])
        return {'synced': True, 'source': source,
                'lines': [{'t': l['t'], 'text': l['text']} for l in synced_lines]}
    return {'synced': False, 'source': source, 'lines': plain_lines}


def _ytm_plain_lyrics(yt, browse_id):
    if not browse_id:
        return None
    try:
        data = yt.get_lyrics(browse_id, timestamps=False)
    except Exception:
        return None
    if data and data.get('lyrics'):
        return _plain_result(data.get('source') or 'LyricFind', data['lyrics'])
    return None


def _lrclib_lyrics(artist, track, album, duration_sec):
    if not (track and artist):
        return None
    data = _lrclib_lookup(artist, track, album, duration_sec)
    if not data or data.get('instrumental'):
        return None
    if data.get('syncedLyrics'):
        result = _synced_from_lrc('LRCLIB', data['syncedLyrics'])
        if result:
            return result
    if data.get('plainLyrics'):
        return _plain_result('LRCLIB', data['plainLyrics'])
    return None


def _netease_lyrics(track, artist, duration_sec):
    if not track:
        return None
    query = (track + ' ' + (artist or '')).strip()
    search = _http_json(
        'https://music.163.com/api/search/get?' +
        urllib.parse.urlencode({'s': query, 'type': 1, 'limit': 8}),
        headers={'Referer': 'https://music.163.com/'})
    songs = (((search or {}).get('result') or {}).get('songs')) or []
    song = _pick_song(
        songs, track, artist, duration_sec,
        name_of=lambda s: s.get('name'),
        artists_of=lambda s: ' '.join(a.get('name', '') for a in (s.get('artists') or [])),
        dur_ms_of=lambda s: s.get('duration') or 0)
    if not song or not song.get('id'):
        return None
    lyric = _http_json(
        'https://music.163.com/api/song/lyric?' +
        urllib.parse.urlencode({'id': song['id'], 'lv': 1, 'kv': 1, 'tv': -1}),
        headers={'Referer': 'https://music.163.com/'})
    lrc = ((lyric or {}).get('lrc') or {}).get('lyric') or ''
    return _synced_from_lrc('NetEase', lrc) or _plain_result('NetEase', _strip_lrc_tags(lrc))


def _kugou_lyrics(track, artist, duration_sec):
    if not track:
        return None
    query = (track + ' ' + (artist or '')).strip()
    search = _http_json(
        'https://songsearch.kugou.com/song_search_v2?' +
        urllib.parse.urlencode({'keyword': query, 'page': 1, 'pagesize': 8,
                                'platform': 'WebFilter', 'tag': 'em'}))
    songs = (((search or {}).get('data') or {}).get('lists')) or []
    song = _pick_song(
        songs, track, artist, duration_sec,
        name_of=lambda s: s.get('SongName'),
        artists_of=lambda s: s.get('SingerName'),
        dur_ms_of=lambda s: (s.get('Duration') or 0) * 1000)
    if not song:
        return None
    file_hash = song.get('FileHash') or song.get('HQFileHash')
    if not file_hash:
        return None
    cands = _http_json(
        'https://krcs.kugou.com/search?' +
        urllib.parse.urlencode({'ver': 1, 'man': 'yes', 'client': 'mobi',
                                'keyword': query, 'hash': file_hash}))
    cand_list = (cands or {}).get('candidates') or []
    if not cand_list:
        return None
    cand = cand_list[0]
    dl = _http_json(
        'https://krcs.kugou.com/download?' +
        urllib.parse.urlencode({'ver': 1, 'client': 'mobi', 'id': cand.get('id'),
                                'accesskey': cand.get('accesskey'),
                                'fmt': 'lrc', 'charset': 'utf8'}))
    content = (dl or {}).get('content')
    if not content:
        return None
    try:
        lrc = base64.b64decode(content).decode('utf-8', 'replace')
    except Exception:
        return None
    return _synced_from_lrc('Kugou', lrc) or _plain_result('Kugou', _strip_lrc_tags(lrc))


def _lyrics_ovh(artist, track):
    if not (track and artist):
        return None
    data = _http_json('https://api.lyrics.ovh/v1/' +
                      urllib.parse.quote(artist) + '/' + urllib.parse.quote(track))
    lyrics = (data or {}).get('lyrics')
    if not lyrics:
        return None
    return _plain_result('lyrics.ovh', lyrics)


_LRCLIB_UA = 'p100/0.1.0 (https://github.com/anomalyco/p100)'
_LRC_TAG = re.compile(r'\[(\d+):(\d+(?:\.\d+)?)\]')


def _lrclib_lookup(artist, track, album, duration_sec):
    """
    Query the LRCLIB exact-match endpoint. Tries with album+duration first
    (best match) and then drops them, since a wrong extra field yields a 404.
    Returns the parsed JSON dict, or None when nothing matched / on error.
    """
    def attempt(with_extra):
        params = {'artist_name': artist, 'track_name': track}
        if with_extra:
            if album:
                params['album_name'] = album
            if duration_sec:
                params['duration'] = str(duration_sec)
        url = 'https://lrclib.net/api/get?' + urllib.parse.urlencode(params)
        req = urllib.request.Request(url, headers={'User-Agent': _LRCLIB_UA})
        try:
            with urllib.request.urlopen(req, timeout=12) as resp:
                return json.loads(resp.read().decode('utf-8'))
        except Exception:
            return None

    return attempt(True) or attempt(False)


def _parse_lrc(text):
    """Parse LRC ("[mm:ss.xx] line") into [{'t': <sec int>, 'text': ...}]."""
    lines = []
    for raw in text.replace('\r\n', '\n').split('\n'):
        m = _LRC_TAG.search(raw)
        if not m:
            continue
        try:
            t = int(int(m.group(1)) * 60 + float(m.group(2)))
        except ValueError:
            continue
        lyric = _LRC_TAG.sub('', raw).strip()
        if lyric:
            lines.append({'t': t, 'text': lyric})
    lines.sort(key=lambda l: l['t'])
    return lines


# YouTube enforces per-client fetch checks (the `rqh=1` URL flag): a client
# can pass extraction while its URLs 403 on every fetch (seen with TVHTML and
# ANDROID_VR, Oct 2026), and which client wins extraction is effectively a
# dice roll. So every resolved URL is probed with mpv's exact initial request
# and dead ones trigger a forced re-extraction with the next client listed.
_PLAYER_CLIENT_FALLBACKS = (
    'visionos', 'ios', 'android_vr', 'tv', 'web_safari', 'mweb',
)


def _url_playable(url):
    """Mimic mpv's initial open (open-ended Range) to see if the URL really
    serves audio. One quick retry: googlevideo occasionally 403s a perfectly
    good URL for a moment (rate limiting), which must not discard it."""
    for attempt in range(2):
        try:
            req = urllib.request.Request(url, headers={
                'Range': 'bytes=0-',
                'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) '
                              'AppleWebKit/537.36 (KHTML, like Gecko) '
                              'Chrome/120.0.0.0 Safari/537.36',
            })
            with urllib.request.urlopen(req, timeout=8) as resp:
                return bool(resp.read(2048))
        except Exception:
            if attempt == 0:
                time.sleep(0.6)
    return False


_WARM_EXTRACTOR = os.environ.get('P100_WARM_EXTRACTOR') == '1'

# In-process extraction (P100_WARM_EXTRACTOR=1): run yt-dlp as a module so a
# resolve skips the per-subprocess Python interpreter startup (~1-3 s). The
# module is imported lazily; any failure falls back to the subprocess path.
_ydl_cls = None
_ydl_tried = False


def _in_process_extract(video_id, client_arg, browser):
    """One in-process yt-dlp extraction; returns (url, error)."""
    global _ydl_cls, _ydl_tried
    if not _ydl_tried:
        _ydl_tried = True
        try:
            from yt_dlp import YoutubeDL
            _ydl_cls = YoutubeDL
        except Exception:
            _ydl_cls = None
    if _ydl_cls is None:
        return None, 'yt_dlp module unavailable'
    opts = {
        'format': 'bestaudio/best',
        'quiet': True,
        'no_warnings': True,
        'skip_download': True,
        'noplaylist': True,
    }
    if client_arg:
        opts['extractor_args'] = {'youtube': {'player_client': [client_arg]}}
    if browser:
        opts['cookiesfrombrowser'] = browser
    try:
        with _ydl_cls(opts) as ydl:
            info = ydl.extract_info(
                f'https://www.youtube.com/watch?v={video_id}', download=False)
    except Exception as e:
        return None, str(e)
    if not info:
        return None, 'yt-dlp module returned no info'
    url = info.get('url')
    if not url:
        # The requested format lives in the formats list; prefer audio-only.
        best = None
        for f in info.get('formats') or []:
            if not f.get('url') or f.get('vcodec') not in (None, 'none'):
                continue
            if best is None or (f.get('abr') or 0) > (best.get('abr') or 0):
                best = f
        url = best.get('url') if best else None
    if not url:
        return None, 'yt-dlp module returned no URL'
    return url, None


def _extract_stream_url(video_id, client_arg, browser):
    """Resolve a stream URL. With the warm extractor enabled the in-process
    module path is tried first and the original yt-dlp subprocess (with its
    cookie variant) stays as the fallback."""
    if _WARM_EXTRACTOR:
        url, err = _in_process_extract(video_id, client_arg, browser)
        if url:
            return url, None
        last_error = err
    else:
        last_error = "yt-dlp returned no URL"
    cmd = [YTDLP, "-f", "bestaudio", "-g", "--no-warnings"]
    if client_arg:
        cmd += ["--extractor-args", f"youtube:player_client={client_arg}"]
    cmd.append(f"https://www.youtube.com/watch?v={video_id}")
    attempts = [cmd]
    if browser:
        attempts.append(cmd[:3] + ["--cookies-from-browser", browser] + cmd[3:])
    for attempt in attempts:
        try:
            url = subprocess.check_output(attempt, text=True).strip()
            if url:
                return url, None
        except Exception as e:
            last_error = str(e)
    return None, last_error


# Client that last produced a playable URL this session (parsed from the
# `c=` parameter of a probe-verified googlevideo URL). Tried first on every
# resolve; None means fall through to yt-dlp's default order.
_warm_client = None


def _client_from_url(url):
    """Map a resolved URL's `c=` parameter back to a yt-dlp player_client
    name, or None when it isn't one we force ourselves."""
    m = re.search(r'[?&]c=([A-Za-z_]+)', url)
    if m and m.group(1).lower() in _PLAYER_CLIENT_FALLBACKS:
        return m.group(1).lower()
    return None


def get_stream_url(video_id, fresh=False):
    # Resolved googlevideo URLs stay valid for hours; reuse one for repeat
    # plays (and the player's prefetch) instead of re-running extraction.
    # `fresh` bypasses the cache after a player-side load failure.
    if fresh:
        _stream_cache.pop(video_id, None)
    cached = _stream_cache.get(video_id)
    if not fresh and cached and time.time() - cached[0] < _STREAM_TTL:
        return {'url': cached[1]}

    browser = os.environ.get('P100_BROWSER')
    last_error = "yt-dlp returned no URL"
    last_url = None
    # Warm client first: the one that last produced a playable URL this
    # session. yt-dlp's default order can land on clients whose URLs pass
    # extraction but 403 at fetch, so reusing a known-good client skips the
    # dice roll entirely.
    global _warm_client
    client_args = []
    if _warm_client:
        client_args.append(_warm_client)
    client_args += [c for c in (None,) + _PLAYER_CLIENT_FALLBACKS
                    if c != _warm_client]
    for client_arg in client_args:
        url, err = _extract_stream_url(video_id, client_arg, browser)
        if not url:
            last_error = err
            continue
        if _url_playable(url):
            _stream_cache[video_id] = (time.time(), url)
            _warm_client = _client_from_url(url) or client_arg
            return {'url': url}
        last_url = url
        last_error = ("resolved URL rejected at fetch "
                      f"(client: {client_arg or 'yt-dlp default'})")
    # Every client was rejected (e.g. googlevideo rate-limiting all probes).
    # Hand over the last extraction anyway rather than erroring out: mpv may
    # still accept it, and if not, the player's fresh-resolve retry fires.
    if last_url:
        return {'url': last_url}
    return {'error': last_error}


# Repeat/back-navigation searches hit YTM every time; a small TTL cache makes
# them instant. Errors are never cached (a transient failure must retry).
_SEARCH_TTL = 900  # seconds
_SEARCH_CACHE_MAX = 100
_search_cache = {}


def _cached_search(query, fn):
    key = (query or '').strip().lower()
    if not key:
        return fn()
    hit = _search_cache.get(key)
    now = time.time()
    if hit and now - hit[0] < _SEARCH_TTL:
        return hit[1]
    result = fn()
    if not (isinstance(result, dict) and 'error' in result):
        _search_cache[key] = (now, result)
        if len(_search_cache) > _SEARCH_CACHE_MAX:
            for k, _ in sorted(_search_cache.items(), key=lambda kv: kv[1][0])[:len(_search_cache) - _SEARCH_CACHE_MAX]:
                _search_cache.pop(k, None)
    return result


def _json_arg(arg):
    """Parse a JSON object argument (the whole rest of the command line)."""
    if not arg:
        return None
    try:
        obj = json.loads(arg)
    except Exception:
        return None
    return obj if isinstance(obj, dict) else None




def run_command_loop(yt, stdin=None, stdout=None):
    """Read newline-delimited commands from stdin until EOF/quit.

    Every reply is one JSON object per line. Errors are stamped with a
    machine-readable kind so the C++ UI can distinguish auth, network and
    backend failures without parsing the message text.
    """
    stdin = stdin if stdin is not None else sys.stdin
    stdout = stdout if stdout is not None else sys.stdout
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        parts = line.split(' ', 1)
        cmd = parts[0]
        arg = parts[1] if len(parts) > 1 else None

        try:
            if cmd == 'recommendations':
                limit = 40
                if arg:
                    try:
                        limit = int(arg)
                    except ValueError:
                        limit = 40
                result = get_home_tracks(yt, limit=limit)
            elif cmd == 'search':
                result = (_cached_search(arg, lambda: search_tracks(yt, arg))
                          if arg else {'error': 'Missing query'})
            elif cmd == 'search_all':
                result = (_cached_search(arg, lambda: search_all(yt, arg))
                          if arg else {'error': 'Missing query'})
            elif cmd == 'radio':
                result = get_radio_tracks(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'album':
                result = get_album_data(yt, arg) if arg else {'error': 'Missing album_id'}
            elif cmd == 'album_of':
                result = album_for_track(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'artist':
                result = get_artist_data(yt, arg) if arg else {'error': 'Missing artist_id'}
            elif cmd == 'artist_songs':
                try:
                    bits = (arg or '').split()
                    if len(bits) != 3:
                        result = {'error': 'Usage: artist_songs <browse_id> <offset> <count>'}
                    else:
                        result = get_artist_songs(yt, bits[0], int(bits[1]), int(bits[2]))
                except ValueError:
                    result = {'error': 'Bad artist_songs offset/count'}
            elif cmd == 'artist_of':
                result = artist_for_track(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'artist_search':
                result = (_cached_search(arg, lambda: search_artists(yt, arg))
                          if arg else {'error': 'Missing query'})
            elif cmd == 'play':
                result = get_stream_url(arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'play_fresh':
                result = get_stream_url(arg, fresh=True) if arg else {'error': 'Missing video_id'}
            elif cmd == 'lyrics':
                result = get_lyrics(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'meta':
                result = get_track_meta(yt, arg) if arg else {'error': 'Missing video_id'}
            elif cmd == 'liked':
                result = yt.get_liked_songs(limit=200)
            elif cmd == 'library_playlists':
                result = yt.get_library_playlists(limit=100)
            elif cmd == 'playlist':
                result = yt.get_playlist(arg, limit=1000) if arg else {'error': 'Missing playlist_id'}
            elif cmd == 'charts':
                result = get_charts_tracks(yt)
            elif cmd == 'mood_categories':
                result = get_mood_categories(yt)
            elif cmd == 'mood_playlists':
                result = get_mood_playlists(yt, arg)
            elif cmd == 'rate_song':
                payload = _json_arg(arg) or {}
                vid = payload.get('videoId')
                rating = payload.get('rating', 'LIKE')
                if not vid:
                    result = {'error': 'Missing videoId'}
                elif rating not in ('LIKE', 'DISLIKE', 'INDIFFERENT'):
                    result = {'error': f'Invalid rating: {rating}'}
                else:
                    yt.rate_song(vid, rating)
                    result = {'status': 'ok'}
            elif cmd == 'create_playlist':
                payload = _json_arg(arg) or {}
                title = payload.get('title')
                if not title:
                    result = {'error': 'Missing title'}
                else:
                    pid = yt.create_playlist(
                        title=title,
                        description=payload.get('description'),
                        privacy_status=payload.get('privacyStatus', 'PRIVATE'),
                        video_ids=payload.get('videoIds'))
                    result = {'status': 'ok', 'playlistId': pid}
            elif cmd == 'delete_playlist':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                if not pid:
                    result = {'error': 'Missing playlistId'}
                else:
                    yt.delete_playlist(pid)
                    result = {'status': 'ok'}
            elif cmd == 'edit_playlist':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                if not pid:
                    result = {'error': 'Missing playlistId'}
                else:
                    kwargs = {}
                    for key in ('title', 'description', 'privacyStatus'):
                        if payload.get(key) is not None:
                            kwargs[key] = payload[key]
                    if payload.get('moveItem') is not None:
                        kwargs['moveItem'] = payload['moveItem']
                    yt.edit_playlist(pid, **kwargs)
                    result = {'status': 'ok'}
            elif cmd == 'add_playlist_items':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                video_ids = payload.get('videoIds')
                if not pid or not video_ids:
                    result = {'error': 'Missing playlistId or videoIds'}
                else:
                    yt.add_playlist_items(pid, videoIds=video_ids, duplicates=False)
                    result = {'status': 'ok'}
            elif cmd == 'remove_playlist_items':
                payload = _json_arg(arg) or {}
                pid = payload.get('playlistId')
                videos = payload.get('videos')
                if not pid or not videos:
                    result = {'error': 'Missing playlistId or videos'}
                else:
                    yt.remove_playlist_items(pid, videos)
                    result = {'status': 'ok'}
            elif cmd == 'quit':
                break
            else:
                result = {'error': f'Unknown command: {cmd}'}
        except Exception as e:
            result = {'error': f'Unhandled error in command {cmd!r}: {str(e)}'}

        if isinstance(result, dict) and 'error' in result and 'kind' not in result:
            result['kind'] = _classify_error(result['error'])

        print(json.dumps(result, ensure_ascii=False), file=stdout)
        stdout.flush()
