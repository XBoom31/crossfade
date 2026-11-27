"""Shared command loop and YouTube Music helpers for both auth backends.

The entry points (p100-backend.py and p100-backend-oauth.py) only differ in
how they create the YTMusic session. Everything below is shared so a command
cannot work in one backend and silently fail in the other.
"""

import array
import base64
import difflib
import glob
import json
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import html as _html
from concurrent.futures import ThreadPoolExecutor

YTDLP = os.environ.get('P100_YTDLP') or 'yt-dlp'

# crossfade is a GUI app on Windows: child console processes must not get a
# console window of their own.
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0) if sys.platform == "win32" else 0

# video_id -> (resolved_at, url); avoids re-running yt-dlp for repeat plays.

# Developer tunables (Settings -> Tunables). The app writes <config>/tunables.json;
# values marked "restart" in that panel are read once, here, at startup.
def _tunable_dirs():
    dirs = []
    env = os.environ.get('P100_CONFIG_DIR')
    if env:
        dirs.append(env)
    xdg = os.environ.get('XDG_CONFIG_HOME') or os.path.join(os.path.expanduser('~'), '.config')
    dirs.append(os.path.join(xdg, 'crossfade'))
    dirs.append(os.path.join(xdg, 'p100'))
    return dirs


def _load_tunables():
    for d in _tunable_dirs():
        try:
            with open(os.path.join(d, 'tunables.json'), encoding='utf-8') as f:
                data = json.load(f)
            if isinstance(data, dict):
                return data
        except Exception:
            continue
    return {}


_TUNE = _load_tunables()


def _tune(key, default):
    try:
        return float(_TUNE.get(key, default))
    except (TypeError, ValueError):
        return float(default)


def _tunei(key, default):
    return int(round(_tune(key, default)))


def _install_http_timeout():
    """Bound ytmusicapi's HTTP requests.

    ytmusicapi's default session uses a 30 s timeout. One stalled request then
    blocks the whole command channel for half a minute, which is exactly the
    "opening an album fails and everything is down" symptom. Fail fast instead
    so the retry can succeed.
    """
    try:
        import requests
    except Exception:
        return
    original = requests.Session.request
    if getattr(original, '_crossfade_timeout', False):
        return
    default = max(3.0, _tune('backend.ytmusic_timeout_sec', 8))

    def request(self, *args, **kwargs):
        if kwargs.get('timeout') is None:
            kwargs['timeout'] = default
        return original(self, *args, **kwargs)

    request._crossfade_timeout = True
    requests.Session.request = request


_install_http_timeout()


_STREAM_TTL = _tunei('backend.stream_ttl_sec', 7200)  # googlevideo URLs are valid ~6h; probed before caching
_stream_cache = {}
_stream_lock = threading.Lock()

# Resolved URLs are shared with the other backend processes (play, prefetch)
# through a small on-disk cache: a URL warmed by one channel is usable by
# another, and repeat plays across restarts skip extraction entirely.
_STREAM_DISK_MAX = _tunei('backend.stream_disk_max', 400)
_stream_disk_loaded = False
_stream_save_lock = threading.Lock()


def _stream_disk_path():
    return os.path.join(_config_dir(), 'stream_urls.json')


def _load_stream_disk():
    global _stream_disk_loaded
    if _stream_disk_loaded:
        return
    _stream_disk_loaded = True
    try:
        with open(_stream_disk_path(), encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        return
    if not isinstance(data, dict):
        return
    now = time.time()
    with _stream_lock:
        for vid, entry in data.items():
            if not isinstance(entry, list) or len(entry) != 2:
                continue
            ts, url = entry
            if (isinstance(ts, (int, float)) and isinstance(url, str) and url
                    and now - ts < _STREAM_TTL and vid not in _stream_cache):
                _stream_cache[vid] = (float(ts), url)
    _prune_stream_cache()


def _cache_stream_url(video_id, url):
    with _stream_lock:
        _stream_cache[video_id] = (time.time(), url)
    _prune_stream_cache()


def _prune_stream_cache():
    """Drop expired URLs and cap the in-memory cache (the disk writer uses
    the same cap). Called on insert so a long session that resolves many
    unique tracks cannot grow this dict without bound."""
    now = time.time()
    with _stream_lock:
        fresh = [(vid, entry) for vid, entry in _stream_cache.items()
                 if now - entry[0] < _STREAM_TTL]
        fresh.sort(key=lambda kv: kv[1][0], reverse=True)
        _stream_cache.clear()
        _stream_cache.update(fresh[:_STREAM_DISK_MAX])


def _stream_disk_lookup(video_id):
    """Re-read the shared cache: another channel may have warmed this id."""
    try:
        with open(_stream_disk_path(), encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        return None
    entry = data.get(video_id) if isinstance(data, dict) else None
    if not (isinstance(entry, list) and len(entry) == 2):
        return None
    ts, url = entry
    if not (isinstance(ts, (int, float)) and isinstance(url, str) and url):
        return None
    if time.time() - ts >= _STREAM_TTL:
        return None
    with _stream_lock:
        _stream_cache.setdefault(video_id, (float(ts), url))
    return url


def _save_stream_disk():
    now = time.time()
    with _stream_lock:
        items = [(vid, entry) for vid, entry in _stream_cache.items()
                 if now - entry[0] < _STREAM_TTL]
    items.sort(key=lambda kv: kv[1][0], reverse=True)
    payload = {vid: [entry[0], entry[1]] for vid, entry in items[:_STREAM_DISK_MAX]}
    tmp = '%s.%d.tmp' % (_stream_disk_path(), os.getpid())
    with _stream_save_lock:
        try:
            os.makedirs(os.path.dirname(tmp), exist_ok=True)
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(payload, f, separators=(',', ':'))
            os.replace(tmp, _stream_disk_path())
        except OSError:
            pass


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


def _thumb_url(item, size=0):
    """Largest thumbnail URL for a ytmusicapi item.

    YouTube Music only advertises 120px art for search cards, but its image
    CDN serves the same asset at any requested width. `size` rewrites those
    Google-hosted URLs so a hero-sized card isn't upscaled from 120px.
    """
    thumbs = item.get('thumbnails') or item.get('thumbnail') or []
    if thumbs and isinstance(thumbs[-1], dict):
        url = thumbs[-1].get('url', '')
        if size and 'googleusercontent.com' in url:
            url = re.sub(r'=w\d+-h\d+', '=w%d-h%d' % (size, size), url)
        return url
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
    # singular "thumbnail" key. _thumb_url accepts both.
    thumb_url = _thumb_url(track)
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
        'videoType': details.get('musicVideoType') or '',
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


def _artist_name(item):
    artists = item.get('artists') or []
    if artists and isinstance(artists[0], dict):
        return ', '.join(a.get('name') or '' for a in artists).strip(', ')
    return item.get('artist') or item.get('title') or ''


def _top_card(item):
    """Normalise YouTube Music's own "Top result" card for the UI.

    The server resolves query intent (artist name -> artist card, album name
    -> album card, track -> song card); this only reshapes the fields. The
    validation/override policy lives in C++, next to the local library data.
    """
    kind = (item.get('resultType') or '').lower()
    if kind not in ('song', 'video', 'album', 'artist'):
        return None
    artists = [a for a in (item.get('artists') or []) if isinstance(a, dict)]
    artist_name = _artist_name(item)
    artist_id = artists[0].get('id') or '' if artists else ''
    title = item.get('title') or ''
    if kind == 'artist':
        title = artist_name or title
    if not title:
        return None
    if kind in ('song', 'video'):
        card_id = item.get('videoId') or ''
    elif kind == 'album':
        card_id = item.get('browseId') or ''
    else:
        card_id = artist_id
    if not card_id:
        return None
    album_name, album_id = _album_fields(item)
    return {
        'kind': kind,
        'id': card_id,
        'title': title,
        'artist': '' if kind == 'artist' else artist_name,
        'artist_id': artist_id,
        'album': album_name,
        'album_id': album_id,
        'year': str(item.get('year') or ''),
        'duration': str(item.get('duration') or ''),
        'duration_sec': _duration_sec(item),
        'subscribers': str(item.get('subscribers') or ''),
        # Search cards advertise 120px art; the CDN serves hero size.
        'thumbnail_url': _thumb_url(item, size=544),
    }


def _suggestion_runs_text(runs):
    return ''.join((r.get('text') or '') for r in (runs or []) if isinstance(r, dict))


def get_search_suggestions(yt, query):
    """Typeahead suggestions + the music items YouTube recommends for them.

    The endpoint's first section is query suggestions (history entries when the
    query repeats something the account searched before); the second is a small
    set of songs/albums/artists. Playlists and podcasts are dropped: the
    suggestion UI only navigates entities the app can open."""
    try:
        raw = yt._send_request('music/get_search_suggestions', {'input': query or ''})
    except Exception as e:
        return _err(f'Suggestions failed: {e}')

    queries, items = [], []
    for section in raw.get('contents') or []:
        contents = ((section.get('searchSuggestionsSectionRenderer') or {})
                    .get('contents')) or []
        for entry in contents:
            if not isinstance(entry, dict):
                continue
            renderer = (entry.get('searchSuggestionRenderer') or
                        entry.get('historySuggestionRenderer'))
            if renderer:
                text = _suggestion_runs_text((renderer.get('suggestion') or {}).get('runs'))
                if text:
                    queries.append({
                        'text': text,
                        'history': 'historySuggestionRenderer' in entry,
                    })
                continue

            r = entry.get('musicResponsiveListItemRenderer')
            if not isinstance(r, dict):
                continue
            cols = []
            for c in r.get('flexColumns') or []:
                runs = (((c.get('musicResponsiveListItemFlexColumnRenderer') or {})
                         .get('text') or {}).get('runs')) or []
                cols.append(_suggestion_runs_text(runs))
            title = cols[0] if cols else ''
            meta = cols[1] if len(cols) > 1 else ''
            extra = cols[2] if len(cols) > 2 else ''
            parts = [p.strip() for p in meta.split('•')] if meta else []
            kind = ''
            if parts and parts[0].lower() in (
                    'song', 'video', 'album', 'artist', 'playlist', 'podcast', 'episode'):
                kind = parts[0].lower()
                parts = parts[1:]
            elif 'monthly audience' in meta.lower() or 'subscriber' in meta.lower():
                kind = 'artist'
            elif extra:
                kind = 'song'
            if not title or kind not in ('song', 'video', 'album', 'artist'):
                continue

            nav = r.get('navigationEndpoint') or {}
            pid = r.get('playlistItemData') or {}
            video_id = pid.get('videoId') or (nav.get('watchEndpoint') or {}).get('videoId') or ''
            browse_id = (nav.get('browseEndpoint') or {}).get('browseId') or ''
            item_id = video_id if kind in ('song', 'video') else browse_id
            if not item_id:
                continue

            artist = ''
            if parts:
                artist = parts[0]
            elif kind == 'artist':
                artist = title
            # "228M plays" is metadata noise in a compact dropdown.
            cleaned = [p for p in parts if p and not re.fullmatch(r'[\d.,]+\s*[KMB]?\s*plays', p)]
            subtitle_parts = cleaned[:1]
            if kind in ('song', 'video') and extra:
                subtitle_parts.append(extra)
            elif kind in ('album', 'artist') and len(cleaned) > 1:
                subtitle_parts.append(cleaned[1])
            if kind == 'artist':
                subtitle_parts = [meta] if meta else []
            thumbs = (((r.get('thumbnail') or {}).get('musicThumbnailRenderer') or {})
                      .get('thumbnail') or {}).get('thumbnails') or []
            thumb = thumbs[-1].get('url', '') if thumbs and isinstance(thumbs[-1], dict) else ''
            items.append({
                'kind': kind,
                'id': item_id,
                'title': title,
                'artist': artist,
                'album': extra if kind in ('song', 'video') else (title if kind == 'album' else ''),
                'subtitle': ' · '.join(p for p in subtitle_parts if p),
                'thumbnail_url': thumb,
            })

    return {'queries': queries, 'items': items}


def search_all(yt, query, limit=20):
    """Fetch one search page in two parallel requests.

    1. the default (unfiltered) search -- it carries YouTube Music's own
       "Top result" card plus the album/artist/playlist/video shelves;
    2. the song-filtered search -- the default page only devotes 5-10 slots
       to songs because podcasts and episodes share the page.

    Both run concurrently, so first paint costs the same as a single call.
    Either request may fail without taking the whole search down."""
    if not query:
        return {'error': 'Missing query'}

    def _run(fn):
        try:
            return fn() or [], None
        except Exception as e:
            return [], str(e)

    with ThreadPoolExecutor(max_workers=2) as pool:
        fut_summary = pool.submit(_run, lambda: yt.search(query, limit=40))
        fut_songs = pool.submit(_run, lambda: yt.search(query, filter='songs', limit=limit))
        summary, summary_err = fut_summary.result()
        songs, songs_err = fut_songs.result()

    if not summary and not songs and (summary_err or songs_err):
        return _err(songs_err or summary_err or 'Search failed')

    top = None
    albums, artists = [], []
    seen_albums, seen_artists = set(), set()
    for item in summary:
        if not isinstance(item, dict):
            continue
        category = item.get('category')
        if category == 'More from YouTube':
            continue  # the top card's related strip; the song shelf covers it
        if category == 'Top result':
            if top is None:
                top = _top_card(item)
            continue
        result_type = (item.get('resultType') or '').lower()
        if result_type == 'album':
            browse_id = item.get('browseId') or ''
            if not browse_id or browse_id in seen_albums:
                continue
            seen_albums.add(browse_id)
            albums.append({
                'browse_id': browse_id,
                'title': item.get('title') or 'Unknown Album',
                'artist': _artist_name(item),
                'year': str(item.get('year') or ''),
                'thumbnail_url': _thumb_url(item),
                'track_count': item.get('trackCount') or 0,
            })
        elif result_type == 'artist':
            name = _artist_name(item)
            item_artists = item.get('artists') or []
            browse_id = item.get('browseId') or ''
            if not browse_id and item_artists and isinstance(item_artists[0], dict):
                browse_id = item_artists[0].get('id') or ''
            if not browse_id or not name or browse_id in seen_artists:
                continue
            seen_artists.add(browse_id)
            artists.append({
                'browse_id': browse_id,
                'name': name,
                'thumbnail_url': _thumb_url(item),
                'subscribers': str(item.get('subscribers') or ''),
            })

    tracks, seen_songs = [], set()
    extra = [i for i in summary
             if isinstance(i, dict) and
             (i.get('resultType') or '').lower() in ('song', 'video')]
    for item in list(songs) + extra:
        if not isinstance(item, dict):
            continue
        video_id = item.get('videoId') or item.get('id')
        if not video_id or video_id in seen_songs or not item.get('title'):
            continue
        seen_songs.add(video_id)
        try:
            tracks.append(track_to_dict(item))
        except Exception:
            continue

    return {'top': top, 'songs': tracks, 'albums': albums, 'artists': artists}


def _duration_seconds(value):
    """Normalise YT Music's human duration ("1 hr 25 min", "45 min") or seconds."""
    if isinstance(value, (int, float)):
        return int(value)
    text = str(value or "").strip().lower()
    if not text:
        return 0
    m = re.fullmatch(r"(\d+):(\d{1,2})(?::(\d{1,2}))?", text)
    if m:
        parts = [int(x) for x in m.groups() if x is not None]
        if len(parts) == 2:
            return parts[0] * 60 + parts[1]
        return parts[0] * 3600 + parts[1] * 60 + parts[2]
    total = 0
    for amount, unit in re.findall(r"(\d+)\s*(hr|hour|min|minute|sec|second)", text):
        n = int(amount)
        if unit.startswith("h"):
            total += n * 3600
        elif unit.startswith("m"):
            total += n * 60
        else:
            total += n
    return total


def _podcast_show(item):
    channel = item.get("channel") or item.get("author") or {}
    if not isinstance(channel, dict):
        channel = {}
    return {
        "browse_id": item.get("browseId") or item.get("podcastId") or "",
        "title": item.get("title") or item.get("name") or "",
        "author": channel.get("name") or item.get("author_name") or "",
        "thumbnail_url": _thumb_url(item),
        "description": item.get("description") or "",
        "saved": bool(item.get("saved")),
    }


def _podcast_episode(item, show=None):
    show = show or {}
    return {
        "video_id": item.get("videoId") or item.get("id") or "",
        "title": item.get("title") or "",
        "author": show.get("author") or item.get("author") or "",
        "thumbnail_url": _thumb_url(item) or show.get("thumbnail_url") or "",
        "duration_sec": _duration_seconds(item.get("duration") or item.get("duration_seconds")),
        "date": item.get("date") or "",
        "description": item.get("description") or "",
    }


def get_podcast_library(yt, limit=100):
    try:
        items = yt.get_library_podcasts(limit=limit)
    except Exception as e:
        return _err(f"Could not load podcasts: {e}")
    return [_podcast_show(x) for x in items if (x.get("title") or "").lower() != "episodes for later"]


def search_podcasts(yt, query, limit=20):
    try:
        items = yt.search(query, filter="podcasts", limit=limit)
    except Exception as e:
        return _err(f"Podcast search failed: {e}")
    return [_podcast_show(x) for x in items]


def get_podcast_details(yt, browse_id, limit=100):
    if not browse_id:
        return {"error": "Missing podcast id"}
    try:
        data = yt.get_podcast(browse_id, limit=limit)
    except Exception as e:
        return _err(f"Could not load podcast: {e}")
    show = _podcast_show(data)
    if not show.get("browse_id"):
        show["browse_id"] = browse_id
    show["saved"] = bool(data.get("saved"))
    episodes = [_podcast_episode(x, show) for x in (data.get("episodes") or [])]
    return {"show": show, "episodes": episodes}


def _spotify_id(link):
    link = (link or "").strip()
    m = re.search(r"(?:playlist[/:])([A-Za-z0-9]+)", link)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9]{16,}", link):
        return link
    return ""


def _spotify_cookie():
    path = os.path.join(
        os.environ.get("P100_CONFIG_DIR")
        or os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "crossfade"),
        "spotify.json")
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f).get("sp_dc") or None
    except Exception:
        return None


SPOTIFY_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36")
SPOTIFY_BOOTSTRAP = "37i9dQZF1DXcBWIGoYBM5M"
SPOTIFY_OPS = ("fetchPlaylist", "fetchLibraryTracks", "libraryV3", "profileAttributes")


def _config_dir():
    return (os.environ.get("P100_CONFIG_DIR")
            or os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"),
                            "crossfade"))


def _spotify_progress_path():
    return os.path.join(_config_dir(), "spotify_import_progress.json")


def _spotify_progress(phase, done, total, name):
    path = _spotify_progress_path()
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"phase": phase, "done": int(done), "total": int(total),
                       "name": name or ""}, f, ensure_ascii=False,
                      separators=(",", ":"))
        os.replace(tmp, path)
    except OSError:
        pass


def _spotify_match_debug(name, total, rejects):
    """Write the rejected candidates so strictness can be tuned against real
    playlists. Values are only track metadata, never account data."""
    path = os.path.join(_config_dir(), "spotify_match_debug.json")
    payload = {"name": name, "total": int(total),
               "unmatched": int(len(rejects)),
               "rejected": rejects[:100]}
    tmp = path + ".tmp"
    try:
        os.makedirs(_config_dir(), exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=1)
        os.replace(tmp, path)
    except OSError:
        pass


def _spotify_get(url, headers=None, timeout=30):
    req = urllib.request.Request(url, headers=headers or {})
    return urllib.request.urlopen(req, timeout=timeout).read()


def _spotify_token(playlist_id=None):
    """The web player's user-scoped token, read from a playlist embed page.

    The embed page includes `settings.session.accessToken`. Unlike
    /api/token (which Spotify blocks for many networks), this works with just
    the sp_dc cookie and is valid for the signed-in account.
    """
    headers = {"User-Agent": SPOTIFY_UA, "Accept-Language": "en"}
    sp_dc = _spotify_cookie()
    if sp_dc:
        headers["Cookie"] = f"sp_dc={sp_dc}"
    pid = playlist_id or SPOTIFY_BOOTSTRAP
    page = _spotify_get(f"https://open.spotify.com/embed/playlist/{pid}",
                        headers).decode("utf-8", "replace")
    m = re.search(r'<script id="__NEXT_DATA__" type="application/json">(.*?)</script>',
                  page, re.S)
    if not m:
        raise RuntimeError("Spotify did not return a playlist page")
    data = json.loads(m.group(1))
    session = (data.get("props", {}).get("pageProps", {}).get("state", {})
               .get("settings", {}).get("session") or {})
    token = session.get("accessToken")
    if not token:
        raise RuntimeError("Spotify did not return an access token")
    return token, bool(session.get("isAnonymous"))


def _spotify_hashes():
    """Persisted-query hashes from the web-player bundle, cached per bundle."""
    cache = os.path.join(_config_dir(), "spotify_queries.json")
    try:
        with open(cache, encoding="utf-8") as f:
            saved = json.load(f)
    except Exception:
        saved = {}
    html = _spotify_get("https://open.spotify.com", {"User-Agent": SPOTIFY_UA}).decode(
        "utf-8", "replace")
    m = re.search(r'<script[^>]+src="([^"]+)"', html)
    bundle = None
    for src in re.findall(r'<script[^>]+src="([^"]+)"', html):
        if "web-player/web-player" in src and src.endswith(".js"):
            bundle = src
            break
    if not bundle:
        raise RuntimeError("Spotify's web-player bundle was not found")
    if saved.get("bundle") == bundle and saved.get("ops", {}).get("fetchPlaylist"):
        return saved["ops"]
    js = _spotify_get(bundle, {"User-Agent": SPOTIFY_UA}).decode("utf-8", "replace")
    ops = {}
    for op in SPOTIFY_OPS:
        hit = re.search(r'"%s","query","([0-9a-f]{64})"' % re.escape(op), js)
        if hit:
            ops[op] = hit.group(1)
    if "fetchPlaylist" not in ops:
        raise RuntimeError("Spotify's playlist query was not found")
    try:
        with open(cache, "w", encoding="utf-8") as f:
            json.dump({"bundle": bundle, "ops": ops}, f)
    except OSError:
        pass
    return ops


def _spotify_query(token, operation, variables):
    ops = _spotify_hashes()
    h = ops.get(operation)
    if not h:
        raise RuntimeError(f"Spotify's {operation} query is unavailable")
    ext = json.dumps({"persistedQuery": {"version": 1, "sha256Hash": h}},
                     separators=(",", ":"))
    params = urllib.parse.urlencode({
        "operationName": operation,
        "variables": json.dumps(variables, separators=(",", ":")),
        "extensions": ext,
    })
    data = json.loads(_spotify_get(
        "https://api-partner.spotify.com/pathfinder/v1/query?" + params,
        {"Authorization": f"Bearer {token}", "User-Agent": SPOTIFY_UA,
         "Accept": "application/json"}))
    if data.get("errors"):
        raise RuntimeError(str(data["errors"][0].get("message") or data["errors"][0]))
    return data.get("data") or {}


def _spotify_image(obj):
    """Pick the smallest usable image URL from Spotify's nested image shapes."""
    if isinstance(obj, dict):
        if isinstance(obj.get("sources"), list):
            sources = [x for x in obj["sources"] if isinstance(x, dict) and x.get("url")]
            sources.sort(key=lambda x: x.get("width") or 0)
            chosen = next((x for x in sources if (x.get("width") or 0) >= 60), None)
            if chosen or sources:
                return (chosen or sources[0])["url"]
        for key in ("image", "images", "coverArt"):
            if key in obj:
                found = _spotify_image(obj[key])
                if found:
                    return found
        for value in obj.values():
            if isinstance(value, (dict, list)):
                found = _spotify_image(value)
                if found:
                    return found
    elif isinstance(obj, list):
        for value in obj:
            found = _spotify_image(value)
            if found:
                return found
    return ""


def _spotify_library():
    token, anonymous = _spotify_token()
    if anonymous:
        raise RuntimeError("Sign in to Spotify to browse your playlists")
    out = []
    offset = 0
    total = 0
    while True:
        data = _spotify_query(token, "libraryV3", {
            "filters": ["Playlists"], "order": None, "textFilter": None,
            "features": ["LIKED_SONGS"], "limit": 50, "offset": offset,
            "flatten": True, "folderUri": None, "includeFoldersWhenFlattening": True,
        })
        lib = ((data.get("me") or {}).get("libraryV3") or {})
        page = lib.get("items") or []
        total = int(lib.get("totalCount") or 0)
        for raw in page:
            wrapper = raw.get("item") or raw
            node = wrapper.get("data") if isinstance(wrapper.get("data"), dict) else wrapper
            uri = (node.get("uri") or node.get("_uri")
                   or wrapper.get("uri") or wrapper.get("_uri") or "")
            name = (node.get("name") or wrapper.get("name") or "").strip()
            if uri.endswith(":collection") or name == "Liked Songs":
                out.append({"id": "liked", "kind": "liked", "name": "Liked Songs",
                            "owner": "", "count": int(node.get("count") or 0),
                            "art": _spotify_image(node.get("image"))})
            elif uri.startswith("spotify:playlist:"):
                pid = uri.rsplit(":", 1)[-1]
                owner = ((node.get("ownerV2") or {}).get("data") or {}).get("name") or ""
                count = int((node.get("content") or {}).get("totalCount")
                            or node.get("count") or 0)
                out.append({"id": pid, "kind": "playlist", "name": name,
                            "owner": owner, "count": count,
                            "art": _spotify_image(node.get("images") or node.get("image"))})
        offset += len(page)
        if not page or offset >= total:
            break
    # Liked Songs first, then the library order.
    out.sort(key=lambda x: 0 if x["kind"] == "liked" else 1)
    return out


def _spotify_liked_items(token):
    items = []
    offset = 0
    total = 0
    while True:
        data = _spotify_query(token, "fetchLibraryTracks",
                              {"offset": offset, "limit": 50})
        tracks = ((data.get("me") or {}).get("library") or {}).get("tracks") or {}
        page = tracks.get("items") or []
        total = int(tracks.get("totalCount") or 0)
        items.extend(page)
        offset += len(page)
        if not page or offset >= total:
            break
    return items


def _spotify_playlist_items(token, playlist_id):
    items = []
    offset = 0
    total = 0
    name = ""
    while True:
        data = _spotify_query(token, "fetchPlaylist", {
            "uri": f"spotify:playlist:{playlist_id}", "offset": offset,
            "limit": 343, "enableWatchFeedEntrypoint": False,
        })
        playlist = data.get("playlistV2") or {}
        if not playlist or playlist.get("__typename") == "NotFound":
            raise RuntimeError("Spotify could not find that playlist")
        name = name or (playlist.get("name") or "Spotify playlist")
        content = playlist.get("content") or {}
        page = content.get("items") or []
        total = int(content.get("totalCount") or 0)
        items.extend(page)
        offset += len(page)
        if not page or offset >= total:
            break
    return name, items


def _spotify_track(item):
    track = item.get("itemV2") or item.get("item") or item.get("track") or item
    if isinstance(track, dict) and isinstance(track.get("data"), dict):
        track = track["data"]
    if not isinstance(track, dict):
        return None
    title = (track.get("name") or "").strip()
    artists = []
    raw_artists = track.get("artists")
    if isinstance(raw_artists, dict):
        raw_artists = raw_artists.get("items") or []
    if isinstance(raw_artists, list):
        for artist in raw_artists:
            if not isinstance(artist, dict):
                continue
            name = ((artist.get("profile") or {}).get("name")
                    or artist.get("name") or "")
            if name:
                artists.append(name)
    if not artists:
        artists = [x.strip() for x in (track.get("artistName") or "").split(",") if x.strip()]
    if not title or not artists:
        return None
    duration = 0
    for key in ("trackDuration", "duration"):
        value = track.get(key)
        if isinstance(value, dict) and value.get("totalMilliseconds"):
            duration = int(value["totalMilliseconds"]) // 1000
            break
    return {
        "title": title,
        "subtitle": ", ".join(artists),
        "duration_sec": duration,
        "thumbnail_url": _spotify_image((track.get("albumOfTrack") or {}).get("coverArt")),
    }


def _spotify_sources(link):
    """Return (kind, id_or_None, display_name)."""
    link = (link or "").strip()
    if link.lower() in ("liked", "collection", "spotify:collection:tracks") or \
            link.endswith("/collection/tracks"):
        return "liked", None, "Liked Songs"
    pid = _spotify_id(link)
    if not pid:
        raise RuntimeError("Paste a Spotify playlist link or ID")
    return "playlist", pid, ""


def _spotify_entity(link):
    playlist_id = _spotify_id(link)
    if not playlist_id:
        raise RuntimeError("Paste a Spotify playlist link or ID")
    headers = {
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36",
        "Accept-Language": "en-US,en;q=0.9",
    }
    sp_dc = _spotify_cookie()
    if sp_dc:
        headers["Cookie"] = f"sp_dc={sp_dc}"
    last = None
    for url in (f"https://open.spotify.com/embed/playlist/{playlist_id}",
                f"https://open.spotify.com/playlist/{playlist_id}"):
        try:
            req = urllib.request.Request(url, headers=headers)
            page = urllib.request.urlopen(req, timeout=20).read().decode("utf-8", "replace")
            m = re.search(r'<script id="__NEXT_DATA__" type="application/json">(.*?)</script>',
                          page, re.S)
            if not m:
                continue
            data = json.loads(m.group(1))
            entity = (data.get("props", {}).get("pageProps", {}).get("state", {})
                      .get("data", {}).get("entity"))
            if not entity:
                continue
            tracks = entity.get("trackList") or []
            if not tracks and "collection/tracks" not in link:
                continue
            return entity.get("title") or entity.get("name") or "Spotify playlist", tracks
        except Exception as e:
            last = e
    raise RuntimeError(f"Could not read the Spotify playlist: {last or 'no track data'}")


# ---------------------------------------------------------------------------
# Spotify -> YouTube Music matching
#
# Taking the first search hit imported karaoke tracks, covers, sped-up edits
# and unrelated songs by the same artist. A candidate now has to agree with
# Spotify on the title *and* the artist; the duration breaks ties and rejects
# live/extended versions. Anything below the confidence gate is reported back
# as "unmatched" instead of being added to the playlist.
# ---------------------------------------------------------------------------

_MATCH_NOISE = ("karaoke", "instrumental", "cover", "sped up", "speed up",
                "slowed", "nightcore", "8d", "reverb", "ringtone", "mashup",
                "tribute", "in the style of", "made famous")
_MATCH_VARIANTS = ("remix", "live", "acoustic", "demo", "extended", "edit",
                   "reprise", "orchestral", "piano", "8 bit")


def _match_text(value):
    """Case/diacritic/punctuation-insensitive form used for comparisons."""
    text = unicodedata.normalize("NFKD", value or "")
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    text = text.casefold().replace("&", " and ")
    text = re.sub(r"[\(\[][^\)\]]*[\)\]]", " ", text)
    text = re.sub(r"\b(feat|featuring|ft|with)\b.*$", " ", text)
    text = re.sub(r"[^0-9a-z]+", " ", text)
    return " ".join(text.split())


def _match_artists(value):
    """Split "A, B & C" into normalised artist names ("A - Topic" stripped)."""
    parts = re.split(r"\s*(?:,|&|/| feat\.? | featuring | with | x )\s*", value or "")
    names = []
    for part in parts:
        name = _match_text(part)
        if name.endswith(" topic"):
            name = name[: -len(" topic")]
        name = re.sub(r"^the ", "", name)
        if name and name not in names:
            names.append(name)
    return names


def _match_title_ratio(source, candidate):
    if not source or not candidate:
        return 0.0
    if source == candidate:
        return 1.0
    ratio = difflib.SequenceMatcher(None, source, candidate).ratio()
    short, long_ = (source, candidate) if len(source) <= len(candidate) else (candidate, source)
    # "Song" vs "Song remastered 2011": containment keeps a strong score.
    if short in long_ and len(short) >= 0.6 * len(long_):
        ratio = max(ratio, 0.9)
    return ratio


def _match_artist_agreement(source, candidate):
    best = 0.0
    for want in _match_artists(source):
        want_tokens = want.split()
        for got in _match_artists(candidate):
            if want == got:
                return 1.0
            got_tokens = got.split()
            if not want_tokens or not got_tokens:
                continue
            common = len(set(want_tokens) & set(got_tokens))
            overlap = common / max(len(want_tokens), len(got_tokens))
            if overlap >= 0.999:
                best = max(best, 1.0)
            elif overlap >= 0.5 or \
                    difflib.SequenceMatcher(None, want, got).ratio() >= 0.88:
                best = max(best, 0.7)
    return best


def _match_loose(value):
    text = unicodedata.normalize("NFKD", value or "")
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    text = re.sub(r"[^0-9a-z]+", " ", text.casefold())
    return " " + " ".join(text.split()) + " "


def _match_variant_factor(source_title, candidate_title):
    """1.0 when both titles name the same version, 0.0 for a different cut.

    A karaoke/cover upload, or a live/remix/acoustic take the Spotify title
    does not name, is a different recording: adding it is worse than skipping
    the track.
    """
    source = _match_loose(source_title)
    candidate = _match_loose(candidate_title)
    for phrase in _MATCH_NOISE + _MATCH_VARIANTS:
        if " %s " % phrase in candidate and " %s " % phrase not in source:
            return 0.0
    return 1.0


def _match_duration_factor(source_sec, candidate_sec):
    """None when either side has no duration; 0.0 when clearly another cut."""
    if not source_sec or not candidate_sec:
        return None
    diff = abs(int(source_sec) - int(candidate_sec))
    if diff <= 5:
        return 1.0
    if diff <= 12:
        return 0.8
    if diff <= max(20, int(source_sec) * 0.04):
        return 0.4
    return 0.0


def _spotify_match_score(item, candidate):
    """Confidence that a YouTube Music search hit is the Spotify track."""
    factor = _match_variant_factor(item.get("title"), candidate.get("title"))
    if factor <= 0.0:
        return 0.0
    duration = _match_duration_factor(item.get("duration_sec"),
                                      candidate.get("duration_sec"))
    if duration == 0.0:
        return 0.0
    title = _match_title_ratio(_match_text(item.get("title")),
                               _match_text(candidate.get("title")))
    artist = _match_artist_agreement(item.get("subtitle"), candidate.get("artist"))
    # Both signals must agree: the right title on someone else's track (a
    # cover, a re-upload, a different artist with the same song name) is
    # exactly what used to end up in the playlist.
    if title < 0.78 or artist < 0.7:
        return 0.0
    score = (0.60 * title + 0.40 * artist) * factor
    if duration is not None:
        score *= 0.85 + 0.15 * duration
    return score


def import_spotify(yt, link):
    _spotify_progress("loading", 0, 0, "")
    try:
        kind, pid, display = _spotify_sources(link)
        token, anonymous = _spotify_token(pid)
        if kind == "liked":
            if anonymous:
                raise RuntimeError("Sign in to Spotify to import Liked Songs")
            name = "Liked Songs"
            items = _spotify_liked_items(token)
        else:
            name, items = _spotify_playlist_items(token, pid)
        parsed = [x for x in (_spotify_track(i) for i in items) if x]
    except Exception as e:
        # Last-resort public embed reader (100 tracks, no sign-in needed).
        try:
            name, embed_items = _spotify_entity(link)
            parsed = []
            for x in embed_items:
                title = (x.get("title") or "").strip()
                if not title:
                    continue
                try:
                    duration = int(x.get("duration") or 0) // 1000
                except (TypeError, ValueError):
                    duration = 0
                parsed.append({"title": title,
                               "subtitle": (x.get("subtitle") or "").strip(),
                               "duration_sec": duration, "thumbnail_url": ""})
        except Exception:
            return _err(str(e))

    total = len(parsed)
    if not total:
        return _err("That Spotify playlist has no playable songs")
    _spotify_progress("matching", 0, total, name)

    def match(item):
        artists = _match_artists(item.get("subtitle"))
        query = " ".join((item.get("title") or "").split())
        if artists:
            query = (query + " " + artists[0]).strip()
        try:
            found = search_tracks(yt, query, limit=8)
        except Exception:
            return None, []
        if isinstance(found, dict) and "error" in found:
            return None, []
        scored = [(_spotify_match_score(item, candidate), candidate)
                  for candidate in (found or [])]
        scored.sort(key=lambda pair: pair[0], reverse=True)
        best = scored[0][1] if scored and scored[0][0] > 0.0 else None
        return best, scored[:3]

    tracks = [None] * total
    rejects = []
    done = 0
    workers = 4 if total >= 8 else 1
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for index, (found, candidates) in enumerate(pool.map(match, parsed)):
            tracks[index] = found
            if not found:
                item = parsed[index]
                rejects.append({
                    "title": item.get("title") or "",
                    "artist": item.get("subtitle") or "",
                    "duration_sec": item.get("duration_sec") or 0,
                    "candidates": [
                        {"title": c.get("title") or "",
                         "artist": c.get("artist") or "",
                         "duration_sec": c.get("duration_sec") or 0,
                         "score": round(score, 3)}
                        for score, c in candidates],
                })
            done += 1
            if done % 5 == 0 or done == total:
                _spotify_progress("matching", done, total, name)

    matched = [t for t in tracks if t]
    unmatched = [{"title": item.get("title") or "",
                  "artist": item.get("subtitle") or ""}
                 for item, track in zip(parsed, tracks) if not track]
    _spotify_progress("done", total, total, name)
    if rejects:
        _spotify_match_debug(name, total, rejects)
    if not matched:
        return _err("No Spotify tracks could be matched on YouTube Music")
    return {"name": name, "tracks": matched, "skipped": total - len(matched),
            "total": total, "unmatched": unmatched}

def get_episodes_later(yt):
    try:
        data = yt.get_saved_episodes(limit=100)
    except Exception as e:
        return _err(f"Could not load episodes: {e}")
    return [_podcast_episode(x) for x in (data.get("tracks") or [])]


def login_state(yt):
    """Whether YouTube actually sees this session as signed in.

    Returns True (signed in), False (the server answered and said the request
    is anonymous) or None (the check could not run: network/API error). A
    YTMusic object can be constructed from an incomplete or stale cookie file
    and then silently send anonymous requests; only this flag distinguishes
    that from a real sign-in.

    The parser lives in login.py so the import helper verifies sessions with
    exactly the same check the backend uses to pick guest or account mode.
    """
    from .login import server_login_state

    return server_login_state(yt)


def check_logged_in(yt):
    """
    Sanity check: confirms the server actually sees us as authenticated,
    rather than silently falling back to anonymous/generic behavior.
    Returns (True, None) or (False, error_message).
    """
    state = login_state(yt)
    if state is None:
        return False, "Could not find logged_in flag in response"
    if not state:
        return False, "YouTube Music reports this session as signed out"
    return True, None


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

    # Providers run on the background "meta" channel, but the panel still
    # waits for the reply: cap the whole cascade so one dead provider cannot
    # hold the request for a minute.
    deadline = time.time() + _tune('backend.lyrics_deadline_sec', 8)
    per_call = _tune('backend.lyrics_provider_timeout_sec', 4)

    providers = (
        ('ytm-timed', lambda: _ytm_timed_lyrics(browse_id, timeout=per_call)),
        ('lrclib', lambda: _lrclib_lyrics(artist_name, track_name, album_name,
                                          duration_sec, timeout=per_call)),
        ('netease', lambda: _netease_lyrics(track_name, artist_name, duration_sec,
                                            timeout=per_call)),
        ('kugou', lambda: _kugou_lyrics(track_name, artist_name, duration_sec,
                                        timeout=per_call)),
        ('lyrics-ovh', lambda: _lyrics_ovh(artist_name, track_name, timeout=per_call)),
        ('ytm-plain', lambda: _ytm_plain_lyrics(yt, browse_id)),
    )

    plain_fallback = None
    for name, provider in providers:
        remaining = deadline - time.time()
        if remaining <= 0:
            print(f"[lyrics] {video_id} {name} skipped (deadline)", file=sys.stderr,
                  flush=True)
            break
        started = time.perf_counter()
        try:
            result = provider()
        except Exception:
            result = None
        elapsed_ms = (time.perf_counter() - started) * 1000
        hit = bool(result)
        print(f"[lyrics] {video_id} {name} {elapsed_ms:.0f}ms "
              f"{'hit' if hit else 'miss'}", file=sys.stderr, flush=True)
        if not result:
            continue
        if result.get('synced'):
            return _translate_lyrics(result, timeout=per_call)
        if plain_fallback is None:
            plain_fallback = result
    if plain_fallback:
        return _translate_lyrics(plain_fallback, timeout=per_call)
    return {'none': True}


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


def _http_json(url, headers=None, data=None, timeout=None):
    if timeout is None:
        timeout = _tune('backend.http_timeout_sec', 10)
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


def _translate_lyrics(result, timeout=None):
    """
    Add English translations (and romanization) to a line-level payload.

    The native backend has its own translation pass, but lyrics served by the
    Python backend must get the same treatment or the immersive view has
    nothing to draw under the original line. The endpoint auto-detects the
    language; English lines come back without a translation, so they keep
    showing only the original text.
    """
    lines = (result or {}).get('lines') or []
    if not lines:
        return result
    unique = []
    by_text = {}
    for line in lines:
        text = (line.get('text') or '').strip()
        if not text:
            continue
        if text not in by_text:
            by_text[text] = len(unique)
            unique.append(text)
    # Keep a direct index for every line, including duplicates and blanks.
    owners = []
    for line in lines:
        text = (line.get('text') or '').strip()
        owners.append(by_text.get(text, -1) if text else -1)
    if not unique:
        return result

    body = json.dumps({'lines': unique, 'to': 'en'}).encode('utf-8')
    headers = {
        'User-Agent': _BROWSER_UA,
        'Content-Type': 'application/json',
        'Accept': 'application/json',
        'Origin': 'https://unison.betterlyrics.org',
    }
    req = urllib.request.Request('https://unison.betterlyrics.org/translate',
                                 data=body, headers=headers, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=timeout or 5) as resp:
            payload = json.loads(resp.read().decode('utf-8', 'replace'))
    except Exception:
        return result
    if (payload.get('detectedLang') or '').lower() == 'en':
        return result
    rows = payload.get('lines') or []
    for i, owner in enumerate(owners):
        if owner < 0 or owner >= len(rows):
            continue
        row = rows[owner] or {}
        translation = row.get('translation')
        if translation and translation != unique[owner]:
            lines[i]['translation'] = translation
        roman = row.get('romanization')
        if roman and roman != unique[owner]:
            lines[i]['roman'] = roman
    return result


def _synced_from_lrc(source, lrc):
    lines = _parse_lrc(lrc or '')
    if not lines or not any(l['t'] > 0 for l in lines):
        return None
    return {'synced': True, 'source': source, 'lines': lines}


_LRC_ANY = re.compile(r'\[[^\]]*\]')


def _strip_lrc_tags(text):
    return _LRC_ANY.sub('', text or '').strip()


# ---- Providers ----------------------------------------------------------

def _ytm_timed_lyrics(browse_id, timeout=None):
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
                      data=payload, timeout=timeout)
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


def _lrclib_lyrics(artist, track, album, duration_sec, timeout=None):
    if not (track and artist):
        return None
    data = _lrclib_lookup(artist, track, album, duration_sec, timeout=timeout)
    if not data or data.get('instrumental'):
        return None
    if data.get('syncedLyrics'):
        result = _synced_from_lrc('LRCLIB', data['syncedLyrics'])
        if result:
            return result
    if data.get('plainLyrics'):
        return _plain_result('LRCLIB', data['plainLyrics'])
    return None


def _netease_lyrics(track, artist, duration_sec, timeout=None):
    if not track:
        return None
    query = (track + ' ' + (artist or '')).strip()
    search = _http_json(
        'https://music.163.com/api/search/get?' +
        urllib.parse.urlencode({'s': query, 'type': 1, 'limit': 8}),
        headers={'Referer': 'https://music.163.com/'}, timeout=timeout)
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
        headers={'Referer': 'https://music.163.com/'}, timeout=timeout)
    lrc = ((lyric or {}).get('lrc') or {}).get('lyric') or ''
    return _synced_from_lrc('NetEase', lrc) or _plain_result('NetEase', _strip_lrc_tags(lrc))


def _kugou_lyrics(track, artist, duration_sec, timeout=None):
    if not track:
        return None
    query = (track + ' ' + (artist or '')).strip()
    search = _http_json(
        'https://songsearch.kugou.com/song_search_v2?' +
        urllib.parse.urlencode({'keyword': query, 'page': 1, 'pagesize': 8,
                                'platform': 'WebFilter', 'tag': 'em'}),
        timeout=timeout)
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
                                'keyword': query, 'hash': file_hash}),
        timeout=timeout)
    cand_list = (cands or {}).get('candidates') or []
    if not cand_list:
        return None
    cand = cand_list[0]
    dl = _http_json(
        'https://krcs.kugou.com/download?' +
        urllib.parse.urlencode({'ver': 1, 'client': 'mobi', 'id': cand.get('id'),
                                'accesskey': cand.get('accesskey'),
                                'fmt': 'lrc', 'charset': 'utf8'}),
        timeout=timeout)
    content = (dl or {}).get('content')
    if not content:
        return None
    try:
        lrc = base64.b64decode(content).decode('utf-8', 'replace')
    except Exception:
        return None
    return _synced_from_lrc('Kugou', lrc) or _plain_result('Kugou', _strip_lrc_tags(lrc))


def _lyrics_ovh(artist, track, timeout=None):
    if not (track and artist):
        return None
    data = _http_json('https://api.lyrics.ovh/v1/' +
                      urllib.parse.quote(artist) + '/' + urllib.parse.quote(track),
                      timeout=timeout)
    lyrics = (data or {}).get('lyrics')
    if not lyrics:
        return None
    return _plain_result('lyrics.ovh', lyrics)


_LRCLIB_UA = 'p100/0.1.0 (https://github.com/anomalyco/p100)'
_LRC_TAG = re.compile(r'\[(\d+):(\d+(?:\.\d+)?)\]')


def _lrclib_lookup(artist, track, album, duration_sec, timeout=None):
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
            with urllib.request.urlopen(req, timeout=timeout or 12) as resp:
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


def _url_playable(url, timeout=None, attempts=None):
    if timeout is None:
        timeout = _tune('backend.playable_timeout_sec', 8.0)
    if attempts is None:
        attempts = _tunei('backend.playable_attempts', 2)
    """Mimic mpv's initial open (open-ended Range) to see if the URL really
    serves audio. One quick retry: googlevideo occasionally 403s a perfectly
    good URL for a moment (rate limiting), which must not discard it."""
    for attempt in range(max(1, int(attempts))):
        try:
            req = urllib.request.Request(url, headers={
                'Range': 'bytes=0-',
                'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) '
                              'AppleWebKit/537.36 (KHTML, like Gecko) '
                              'Chrome/120.0.0.0 Safari/537.36',
            })
            with urllib.request.urlopen(req, timeout=float(timeout)) as resp:
                return bool(resp.read(2048))
        except Exception:
            if attempt + 1 < max(1, int(attempts)):
                time.sleep(0.15)
    return False


_WARM_EXTRACTOR = os.environ.get('P100_WARM_EXTRACTOR') == '1'

# yt-dlp needs a JavaScript runtime (deno/node) to run its YouTube challenge
# solver (yt-dlp-ejs). The app provisions one and passes "name:/abs/path";
# without it most clients return no audio formats at all.
_JS_RUNTIME = (os.environ.get('P100_JS_RUNTIME') or '').strip()

# Which worker this process is (main / play / prefetch / meta); set by the app.
_BACKEND_ROLE = os.environ.get('P100_BACKEND_ROLE', 'main')
# Concurrent command loop: the main channel answers many in-flight commands
# (album page, search, library) and tags each reply with a request id, so one
# slow HTTP request can no longer block everything. Other workers stay serial.
_BACKEND_CONCURRENT = os.environ.get('P100_BACKEND_CONCURRENT') == '1'
_TRACE_LOCK = threading.Lock()


def _js_runtime_opts():
    name, sep, path = _JS_RUNTIME.partition(':')
    if not sep or not name or not path:
        return {}
    return {'js_runtimes': {name: {'path': path}}}


def _js_runtime_args():
    return ['--js-runtimes', _JS_RUNTIME] if _JS_RUNTIME else []


def _js_runtime_available():
    if _JS_RUNTIME:
        return True
    return bool(shutil.which('deno') or shutil.which('node'))


# Errors that mean yt-dlp could not build any audio/video format (missing JS
# runtime, challenge failure, SABR-only reply). Retrying other clients or a
# second yt-dlp process gives the same answer and only wedges the command
# thread, so those stop the cascade early.
_FORMAT_DEAD_ENDS = (
    'requested format is not available',
    'signature solving failed',
    'n challenge solving failed',
    'no supported javascript runtime',
    'only images are available',
)


def _format_dead_end(error):
    text = (error or '').lower()
    return any(marker in text for marker in _FORMAT_DEAD_ENDS)


def _missing_runtime_hint(error):
    hint = ("YouTube needs a JavaScript runtime (Deno) to resolve streams. "
            "Run crossfade's installer to provision it, then restart.")
    if not error or _format_dead_end(error):
        return hint
    return error

# In-process extraction (P100_WARM_EXTRACTOR=1): run yt-dlp as a module so a
# resolve skips the per-subprocess Python interpreter startup (~1-3 s). The
# module is imported lazily; any failure falls back to the subprocess path.
_ydl_cls = None
_ydl_tried = False
_ydl_local = threading.local()


def _thread_ydl(client_arg, browser, opts):
    """Per-thread persistent YoutubeDL instance (yt-dlp caches player JS)."""
    cache = getattr(_ydl_local, 'instances', None)
    if cache is None:
        cache = {}
        _ydl_local.instances = cache
    key = (client_arg, browser)
    ydl = cache.get(key)
    if ydl is None:
        ydl = _ydl_cls(opts)
        cache[key] = ydl
    return ydl


def _in_process_extract(video_id, client_arg, browser):
    """One in-process yt-dlp extraction; returns (url, error).

    Cookies are tried *second*. A signed-in account can be enrolled in
    YouTube's SABR-only / "page needs to be reloaded" experiment, where every
    extraction that sends the account cookies fails the n-challenge while the
    anonymous one resolves normally. The subprocess path always had this
    order; the warm path used to send cookies unconditionally, so every
    uncached resolve failed and then skipped the fallback.
    """
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
    base = {
        'format': 'bestaudio/best',
        'quiet': True,
        'no_warnings': True,
        'skip_download': True,
        'noplaylist': True,
        # Bound the network work: this runs on the command thread.
        'socket_timeout': _tune('backend.resolve_socket_timeout_sec', 10),
        'retries': 1,
    }
    base.update(_js_runtime_opts())
    if client_arg:
        base['extractor_args'] = {'youtube': {'player_client': [client_arg]}}

    def attempt(with_cookies):
        opts = dict(base)
        if with_cookies and browser:
            # yt-dlp expects (browser, profile, keyring, container); a bare
            # string is unpacked character by character.
            opts['cookiesfrombrowser'] = (browser,)
        # The primary client instance is kept per thread: yt-dlp caches the
        # player JS and its signature solver on the instance (~1 s per next
        # resolve). Cookie and fallback-client variants stay ephemeral.
        persistent = client_arg in (None, 'web_music', 'visionos')
        try:
            ydl = (_thread_ydl(client_arg, browser if with_cookies else None, opts)
                   if persistent else _ydl_cls(opts))
            try:
                info = ydl.extract_info(
                    f'https://www.youtube.com/watch?v={video_id}', download=False)
            finally:
                if not persistent:
                    ydl.close()
        except Exception as e:
            return None, str(e)
        if not info:
            return None, 'yt-dlp module returned no info'
        url = info.get('url')
        if not url:
            # The requested format lives in the formats list; prefer audio.
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

    url, err = attempt(False)
    if url or not browser:
        return url, err
    return attempt(True)


def _extract_stream_url(video_id, client_arg, browser, deadline=None):
    """Resolve a stream URL. With the warm extractor enabled the in-process
    module path is tried first and the original yt-dlp subprocess (with its
    cookie variant) stays as the fallback."""
    def remaining(default):
        if deadline is None:
            return default
        return max(3.0, min(float(default), deadline - time.time()))

    last_error = "yt-dlp returned no URL"
    if _WARM_EXTRACTOR:
        url, err = _in_process_extract(video_id, client_arg, browser)
        if url:
            return url, None
        last_error = err
        # The same yt-dlp would give the same answer; spawning it again only
        # doubles the wait while the single command thread is blocked.
        if _format_dead_end(err):
            return None, err
    cmd = [YTDLP, "-f", "bestaudio", "-g", "--no-warnings"] + _js_runtime_args()
    if client_arg:
        cmd += ["--extractor-args", f"youtube:player_client={client_arg}"]
    cmd.append(f"https://www.youtube.com/watch?v={video_id}")
    attempts = [cmd]
    if browser:
        attempts.append(cmd[:3] + ["--cookies-from-browser", browser] + cmd[3:])
    for attempt in attempts:
        if deadline is not None and time.time() >= deadline:
            break
        try:
            # Bounded: this runs on the single command thread, so a child that
            # hangs (cookie/keyring prompt, stuck network) would wedge every
            # later command long after the C++ side stopped waiting.
            url = subprocess.check_output(
                attempt, text=True, timeout=remaining(12),
                creationflags=_NO_WINDOW).strip()
            if url:
                return url, None
        except Exception as e:
            last_error = str(e)
            if _format_dead_end(last_error):
                break
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


# Some YouTube Music ids point at a video that is available only to Premium
# members, so yt-dlp cannot extract any audio format for them. Resolve a
# public upload of the same song instead. The C++ side keys its offline cache
# by the *requested* id, so the stand-in URL is cached under the original id
# and the library keeps showing the correct title/artist.
_yt_client = None
_FALLBACK_BAD_WORDS = re.compile(
    r'parody|cover|karaoke|remix|sped[ -]?up|slowed|instrumental|reaction|'
    r'1 hour|loop|tribute|mashup|acoustic|extended', re.I)
_fallback_ids_lock = threading.RLock()
_fallback_ids = None


def _fallback_path():
    return os.path.join(_config_dir(), 'fallback_ids.json')


def _load_fallback_ids():
    global _fallback_ids
    with _fallback_ids_lock:
        if _fallback_ids is None:
            try:
                with open(_fallback_path(), encoding='utf-8') as f:
                    data = json.load(f)
                _fallback_ids = data if isinstance(data, dict) else {}
            except Exception:
                _fallback_ids = {}
        return dict(_fallback_ids)


def _save_fallback_id(video_id, source_id):
    with _fallback_ids_lock:
        _load_fallback_ids()
        _fallback_ids[video_id] = {'id': source_id, 'ts': time.time()}
        try:
            tmp = '%s.%d.tmp' % (_fallback_path(), os.getpid())
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(_fallback_ids, f, separators=(',', ':'))
            os.replace(tmp, _fallback_path())
        except Exception:
            pass


def _drop_fallback_id(video_id):
    with _fallback_ids_lock:
        _load_fallback_ids()
        if _fallback_ids.pop(video_id, None) is not None:
            try:
                with open(_fallback_path(), 'w', encoding='utf-8') as f:
                    json.dump(_fallback_ids, f, separators=(',', ':'))
            except Exception:
                pass


def _norm_title(s):
    return re.sub(r'[^a-z0-9]+', ' ', (s or '').lower()).strip()


def _find_public_equivalent(video_id, title, artist, duration):
    """Search public YouTube uploads for the same song, preferring a close
    duration match and an official/channel upload over fan edits."""
    query = ' '.join(x for x in (artist, title) if x).strip()[:120]
    if not query:
        return None
    cmd = [YTDLP, '--no-warnings', '--flat-playlist', '--print',
           '%(id)s\t%(duration)s\t%(title)s\t%(channel)s'] + _js_runtime_args()
    cmd.append('ytsearch8:' + query)
    try:
        out = subprocess.check_output(cmd, text=True, timeout=40,
                                      creationflags=_NO_WINDOW)
    except Exception:
        return None
    want = set(w for w in _norm_title(title).split() if len(w) > 2)
    best = None
    best_score = None
    for line in (out or '').splitlines():
        parts = line.split('\t')
        if len(parts) < 3:
            continue
        sid, sdur, stitle = parts[0].strip(), parts[1].strip(), parts[2].strip()
        channel = parts[3].strip() if len(parts) > 3 else ''
        if not re.fullmatch(r'[A-Za-z0-9_-]{11}', sid) or sid == video_id:
            continue
        norm = _norm_title(stitle)
        if not norm or _FALLBACK_BAD_WORDS.search(stitle):
            continue
        if want and not all(w in norm for w in want):
            continue
        try:
            dur = int(float(sdur))
        except (TypeError, ValueError):
            dur = 0
        if duration and dur and abs(dur - duration) > max(30, duration * 0.35):
            continue
        score = abs(dur - duration) if duration and dur else 600.0
        low = stitle.lower()
        if artist and artist.lower() in channel.lower():
            score -= 20
        if 'official' in low:
            score -= 10
        if 'audio' in low:
            score -= 5
        if best_score is None or score < best_score:
            best, best_score = sid, score
    return best


_fallback_resolve_lock = threading.Lock()


def _fallback_stream_url(video_id):
    """Best-effort URL for a Premium-only / blocked id."""
    global _yt_client
    if _yt_client is None:
        return None
    # One search + probe per id at a time; prefetch can ask for the same
    # Premium track from several worker threads at once.
    with _fallback_resolve_lock:
        entry = _load_fallback_ids().get(video_id) or {}
        source = entry.get('id') if isinstance(entry, dict) else None
        if not source:
            try:
                meta = get_track_meta(_yt_client, video_id) or {}
            except Exception:
                return None
            if not meta.get('ok'):
                return None
            source = _find_public_equivalent(video_id, meta.get('title') or '',
                                             meta.get('artist') or '',
                                             int(meta.get('durationSec') or 0))
            if not source:
                return None
            _save_fallback_id(video_id, source)
        browser = os.environ.get('P100_BROWSER')
        for client_arg in ('visionos', 'web_music', None) + _PLAYER_CLIENT_FALLBACKS:
            url, _ = _extract_stream_url(source, client_arg, browser)
            if url and _url_playable(url):
                return url
        return None


def get_stream_url(video_id, fresh=False):
    # Resolved googlevideo URLs stay valid for hours; reuse one for repeat
    # plays (and the player's prefetch) instead of re-running extraction.
    # `fresh` bypasses the cache after a player-side load failure.
    if fresh:
        with _stream_lock:
            _stream_cache.pop(video_id, None)
        _drop_fallback_id(video_id)
    else:
        _load_stream_disk()
        with _stream_lock:
            cached = _stream_cache.get(video_id)
        if cached and time.time() - cached[0] < _STREAM_TTL:
            return {'url': cached[1], 'id': video_id}
        # Another backend channel may have resolved this id after we loaded.
        url = _stream_disk_lookup(video_id)
        if url:
            return {'url': url, 'id': video_id}

    browser = os.environ.get('P100_BROWSER')
    last_error = "yt-dlp returned no URL"
    last_url = None
    # Warm client first: the one that last produced a playable URL this
    # session. After that: `visionos` and `web_music` are the profiles whose
    # URLs reliably survive both the probe and mpv (measured 1.4-2.1 s vs
    # ~3 s for the default order, whose URL is usually rejected at fetch).
    global _warm_client
    client_args = []
    if _warm_client:
        client_args.append(_warm_client)
    for c in ('visionos', 'web_music', None) + _PLAYER_CLIENT_FALLBACKS:
        if c not in client_args:
            client_args.append(c)
    # Without a JS runtime most clients cannot build audio formats at all.
    # visionos/mweb are the JS-free ones, so try those and stop instead of
    # walking the whole cascade for minutes.
    js_available = _js_runtime_available()
    if not js_available:
        client_args = [c for c in client_args if c in ('visionos', 'mweb')]
    # Hard stop for the whole resolve. Each client attempt can start a yt-dlp
    # process and probe a URL; an unbounded cascade blocks the command thread
    # long past the app's 45 s read timeout, which reads as "backend is not
    # responding" and starves every later command.
    deadline = time.time() + _tune('backend.resolve_deadline_sec', 25)
    for client_arg in client_args:
        if time.time() >= deadline:
            last_error = last_error or 'stream resolution timed out'
            break
        url, err = _extract_stream_url(video_id, client_arg, browser, deadline)
        if not url:
            last_error = err
            continue
        if _url_playable(url):
            _cache_stream_url(video_id, url)
            _warm_client = _client_from_url(url) or client_arg
            _save_stream_disk()
            return {'url': url, 'id': video_id}
        last_url = url
        last_error = ("resolved URL rejected at fetch "
                      f"(client: {client_arg or 'yt-dlp default'})")
    # Every client was rejected (e.g. googlevideo rate-limiting all probes).
    # Hand over the last extraction anyway rather than erroring out: mpv may
    # still accept it, and if not, the player's fresh-resolve retry fires.
    if last_url:
        return {'url': last_url, 'id': video_id}
    # "This video is only available to Music Premium members": use a public
    # upload of the same song. Cached under the requested id.
    fallback = _fallback_stream_url(video_id)
    if fallback:
        _cache_stream_url(video_id, fallback)
        _save_stream_disk()
        return {'url': fallback, 'id': video_id}
    if not js_available:
        last_error = _missing_runtime_hint(last_error)
    return {'error': last_error}


# ---------------------------------------------------------------------------
# Video streams (the main-area video surface)
#
# Same client-fallback + fetch-probe strategy as audio, but resolving the
# video+audio pair. mpv's own ytdl hook is not used: its separate-format
# fetches are routinely 403'd by googlevideo, while the client whose URLs are
# actually playable is exactly what the probe below discovers.
# ---------------------------------------------------------------------------
# Prefer a muxed (video+audio in one file) stream: it has a single timeline,
# so seeking and the audio/video sync are native. Most videos have a
# progressive AVC/AAC format (360p, sometimes 534p/720p). Only fall back to
# the separate pair when no muxed format exists.
# ---------------------------------------------------------------------------
# Music-video matching
#
# YouTube Music song ids are YouTube video ids, but the song entry is often a
# still/lyric upload. Searching the video catalogue for the same title+artist
# finds the official music video. Matches are cached for a month: the choice
# must be stable (and available offline), and a repeat open should be instant.
# ---------------------------------------------------------------------------
_video_map_lock = threading.Lock()
_video_map = None
_VIDEO_MAP_TTL = _tunei('backend.video_map_ttl_days', 30) * 86400


def _video_map_path():
    return os.path.join(_config_dir(), 'video_map.json')


def _load_video_map():
    global _video_map
    with _video_map_lock:
        if _video_map is not None:
            return _video_map
        try:
            with open(_video_map_path(), encoding='utf-8') as f:
                data = json.load(f)
        except Exception:
            data = {}
        _video_map = data if isinstance(data, dict) else {}
        return _video_map


def _save_video_map():
    data = _load_video_map()
    now = time.time()
    with _video_map_lock:
        pruned = {k: v for k, v in data.items()
                  if isinstance(v, dict) and now - (v.get('ts') or 0) < _VIDEO_MAP_TTL}
        data.clear()
        data.update(pruned)
        payload = dict(data)
    tmp = '%s.%d.tmp' % (_video_map_path(), os.getpid())
    try:
        os.makedirs(os.path.dirname(tmp), exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(payload, f, separators=(',', ':'))
        os.replace(tmp, _video_map_path())
    except Exception:
        pass


def _duration_seconds(value):
    if isinstance(value, (int, float)):
        return int(value)
    if not isinstance(value, str):
        return 0
    bits = value.strip().split(':')
    try:
        nums = [int(b) for b in bits]
    except ValueError:
        return 0
    total = 0
    for n in nums:
        total = total * 60 + n
    return total


def _candidate_artists(cand):
    out = []
    for a in (cand.get('artists') or []):
        name = a.get('name') if isinstance(a, dict) else a
        if name:
            out.append(str(name))
    if cand.get('author'):
        out.append(str(cand['author']))
    return out


def _music_video_match(yt, meta):
    """Score the video search hits for `meta` (title/artist/duration)."""
    title = meta.get('title') or ''
    artist = meta.get('artist') or ''
    duration = int(meta.get('durationSec') or 0)
    if not title:
        return None
    query = ('%s %s' % (artist, title)).strip()
    try:
        candidates = yt.search(query, filter='videos', limit=12) or []
    except Exception:
        return None
    want_title = _norm(title)
    want_artist = _norm(artist)
    best = None
    for cand in candidates:
        cid = cand.get('videoId') or ''
        if not cid or cid == meta.get('id'):
            continue
        ctitle = cand.get('title') or ''
        ntitle = _norm(ctitle)
        if not want_title or not (want_title in ntitle or ntitle in want_title):
            continue
        artists = _candidate_artists(cand)
        artist_ok = (not want_artist or
                     any(want_artist in _norm(a) or _norm(a) in want_artist for a in artists) or
                     want_artist in ntitle)
        cdur = _duration_seconds(cand.get('duration'))
        official = cand.get('videoType') == 'MUSIC_VIDEO_TYPE_OMV'
        score = 0.0
        if ntitle == want_title:
            score += 4
        else:
            score += 2
        if artist_ok:
            score += 3
        if official:
            score += 3
        if re.search(r'official\s+(music\s+)?video', ctitle, re.I):
            score += 1
        if duration and cdur:
            score += max(0.0, 3.0 - abs(cdur - duration) / 15.0)
        if best is None or score > best['score']:
            best = {
                'videoId': cid,
                'title': ctitle,
                'artist': (artists[0] if artists else artist),
                'durationSec': cdur,
                'score': score,
                'official': official,
                'artistOk': artist_ok,
            }
    if not best:
        return None
    # Require an artist hit (or an official music video) so a cover/lyric
    # re-upload with a similar title never wins by accident.
    if best['score'] < 5.0 or not (best['artistOk'] or best['official']):
        return None
    return best


def get_music_video(yt, video_id):
    """Official music video for a song id, cached on disk for a month."""
    vid = (video_id or '').strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{11}', vid):
        return _err('Invalid video id', kind='backend')
    cache = _load_video_map()
    hit = cache.get(vid)
    if isinstance(hit, dict) and time.time() - (hit.get('ts') or 0) < _VIDEO_MAP_TTL:
        return {'ok': True, **{k: hit.get(k) for k in
                               ('videoId', 'title', 'artist', 'durationSec', 'matched', 'self')}}
    meta = get_track_meta(yt, vid)
    if not isinstance(meta, dict) or 'error' in meta:
        return meta if isinstance(meta, dict) else _err('Metadata unavailable')

    # The track's own upload may already be a real video (official music
    # video, user upload, podcast episode). Searching then can only replace a
    # correct video with a worse one, so keep the id as-is and skip matching.
    own_type = meta.get('videoType') or ''
    if own_type and own_type != 'MUSIC_VIDEO_TYPE_ATV':
        entry = {
            'videoId': vid,
            'title': meta.get('title') or '',
            'artist': meta.get('artist') or '',
            'durationSec': int(meta.get('durationSec') or 0),
            'matched': True,
            'self': True,
            'ts': time.time(),
        }
        cache[vid] = entry
        _save_video_map()
        return {'ok': True, **{k: entry[k] for k in
                               ('videoId', 'title', 'artist', 'durationSec', 'matched', 'self')}}

    best = _music_video_match(yt, meta)
    if best:
        entry = {
            'videoId': best['videoId'],
            'title': best['title'],
            'artist': best['artist'],
            'durationSec': best['durationSec'],
            'matched': True,
            'self': False,
        }
    else:
        # Falling back to the song's own upload is always correct (just not
        # always a real video), so remember that decision too.
        entry = {
            'videoId': vid,
            'title': meta.get('title') or '',
            'artist': meta.get('artist') or '',
            'durationSec': int(meta.get('durationSec') or 0),
            'matched': False,
            'self': False,
        }
    entry['ts'] = time.time()
    cache[vid] = entry
    _save_video_map()
    return {'ok': True, **{k: entry[k] for k in
                           ('videoId', 'title', 'artist', 'durationSec', 'matched', 'self')}}


def _video_format(max_height=720):
    try:
        h = max(240, min(2160, int(max_height)))
    except (TypeError, ValueError):
        h = 720
    return (f"b[height<={h}][vcodec^=avc1]/b[height<={h}]/"
            f"bv*[vcodec^=avc1][height<={h}]+ba/bv*[height<={h}]+ba/b[height<={h}]")


_VIDEO_FORMAT = _video_format(720)


def _pick_pair(formats):
    """Split yt-dlp's requested formats into (video_url, audio_url)."""
    video = audio = None
    for f in formats or []:
        if not f.get('url'):
            continue
        vcodec = f.get('vcodec')
        acodec = f.get('acodec')
        if vcodec not in (None, 'none') and not video:
            video = f['url']
        elif acodec not in (None, 'none') and not audio:
            audio = f['url']
    return video, audio, None


def _in_process_extract_video(video_id, client_arg, browser, fmt=_VIDEO_FORMAT):
    """In-process video resolve; returns (video_url, audio_url, error).

    Same anonymous-first cookie order as the audio path (see above).
    """
    global _ydl_cls, _ydl_tried
    if not _ydl_tried:
        _ydl_tried = True
        try:
            from yt_dlp import YoutubeDL
            _ydl_cls = YoutubeDL
        except Exception:
            _ydl_cls = None
    if _ydl_cls is None:
        return None, None, 'yt_dlp module unavailable'
    base = {
        'format': fmt,
        'quiet': True,
        'no_warnings': True,
        'skip_download': True,
        'noplaylist': True,
        'socket_timeout': _tune('backend.resolve_socket_timeout_sec', 10),
        'retries': 1,
    }
    base.update(_js_runtime_opts())
    if client_arg:
        base['extractor_args'] = {'youtube': {'player_client': [client_arg]}}

    def attempt(with_cookies):
        opts = dict(base)
        if with_cookies and browser:
            opts['cookiesfrombrowser'] = (browser,)
        persistent = client_arg in (None, 'web_music', 'visionos')
        try:
            ydl = (_thread_ydl(client_arg, browser if with_cookies else None, opts)
                   if persistent else _ydl_cls(opts))
            try:
                info = ydl.extract_info(
                    f'https://www.youtube.com/watch?v={video_id}', download=False)
            finally:
                if not persistent:
                    ydl.close()
        except Exception as e:
            return None, None, str(e)
        if not info:
            return None, None, 'yt-dlp module returned no info'
        video, audio, _ = _pick_pair(info.get('requested_formats'))
        if not video and info.get('url') and info.get('vcodec') not in (None, 'none'):
            video = info['url']  # progressive single file
        if not video:
            return None, None, 'yt-dlp module returned no video URL'
        return video, audio, None

    video, audio, err = attempt(False)
    if video or not browser:
        return video, audio, err
    return attempt(True)


def _extract_video_urls(video_id, client_arg, browser, fmt=_VIDEO_FORMAT):
    """One video extraction attempt; returns (video_url, audio_url, error).
    `yt-dlp -g` prints one URL per requested format: video first, then audio."""
    if _WARM_EXTRACTOR:
        video, audio, err = _in_process_extract_video(video_id, client_arg, browser, fmt)
        if video:
            return video, audio, None
        last_error = err
    else:
        last_error = 'yt-dlp returned no URL'
    cmd = [YTDLP, '-f', fmt, '-g', '--no-warnings'] + _js_runtime_args()
    if client_arg:
        cmd += ['--extractor-args', f'youtube:player_client={client_arg}']
    cmd.append(f'https://www.youtube.com/watch?v={video_id}')
    attempts = [cmd]
    if browser:
        attempts.append(cmd[:3] + ['--cookies-from-browser', browser] + cmd[3:])
    for attempt in attempts:
        try:
            out = subprocess.check_output(attempt, text=True, timeout=20,
                                          creationflags=_NO_WINDOW).strip()
            lines = [line.strip() for line in out.splitlines() if line.strip()]
            if lines:
                return lines[0], (lines[1] if len(lines) > 1 else None), None
        except Exception as e:
            last_error = str(e)
            if _format_dead_end(last_error):
                break
    return None, None, last_error


_video_stream_cache = {}
_VIDEO_STREAM_TTL = _tunei('backend.video_stream_ttl_sec', 3600)
_VIDEO_DISK_MAX = _tunei('backend.video_disk_max', 64)
_video_stream_disk_loaded = False
_video_stream_disk_lock = threading.Lock()


def _video_stream_disk_path():
    return os.path.join(_config_dir(), 'video_streams.json')


def _load_video_stream_disk():
    global _video_stream_disk_loaded
    if _video_stream_disk_loaded:
        return
    _video_stream_disk_loaded = True
    try:
        with open(_video_stream_disk_path(), encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        return
    if not isinstance(data, dict):
        return
    now = time.time()
    for key, entry in data.items():
        if (isinstance(entry, list) and len(entry) == 2 and
                isinstance(entry[0], (int, float)) and isinstance(entry[1], dict)
                and now - entry[0] < _VIDEO_STREAM_TTL):
            _video_stream_cache.setdefault(key, (float(entry[0]), entry[1]))


def _video_stream_disk_lookup(key):
    try:
        with open(_video_stream_disk_path(), encoding='utf-8') as f:
            data = json.load(f)
    except Exception:
        return None
    entry = data.get(key) if isinstance(data, dict) else None
    if not (isinstance(entry, list) and len(entry) == 2):
        return None
    ts, pair = entry
    if (not isinstance(ts, (int, float)) or not isinstance(pair, dict)
            or time.time() - ts >= _VIDEO_STREAM_TTL):
        return None
    _video_stream_cache.setdefault(key, (float(ts), pair))
    return pair


def _save_video_stream_disk():
    now = time.time()
    items = [(k, v) for k, v in _video_stream_cache.items()
             if now - v[0] < _VIDEO_STREAM_TTL]
    items.sort(key=lambda kv: kv[1][0], reverse=True)
    payload = {k: [v[0], v[1]] for k, v in items[:_VIDEO_DISK_MAX]}
    tmp = '%s.%d.tmp' % (_video_stream_disk_path(), os.getpid())
    with _video_stream_disk_lock:
        try:
            os.makedirs(os.path.dirname(tmp), exist_ok=True)
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(payload, f, separators=(',', ':'))
            os.replace(tmp, _video_stream_disk_path())
        except Exception:
            pass



def get_video_urls(video_id, fresh=False, max_height=720, fast=False):
    """Resolve the video+audio pair for an id. Probes the video URL the same
    way the audio path probes its stream, so a client whose download 403s is
    skipped instead of surfacing as "no video"."""
    global _warm_client
    fmt = _video_format(max_height)
    cache_key = '%s|%d' % (video_id, int(max_height or 720))
    if fresh:
        _video_stream_cache.pop(cache_key, None)
    else:
        _load_video_stream_disk()
        hit = _video_stream_cache.get(cache_key)
        if hit and time.time() - hit[0] < _VIDEO_STREAM_TTL:
            return dict(hit[1])
        # Shared with the other channels and previous sessions (URLs live ~6 h;
        # a 1 h TTL is conservative and survives a restart).
        disk = _video_stream_disk_lookup(cache_key)
        if disk:
            return dict(disk)

    browser = os.environ.get('P100_BROWSER')
    last_error = 'yt-dlp returned no video URL'
    last_pair = None
    client_args = []
    if _warm_client:
        client_args.append(_warm_client)
    if fast:
        # Short path for opening the page: a couple of known-good clients and
        # a single short probe. The caller retries the full walk in the
        # background when this fails.
        for c in ('visionos', 'mweb', 'web_music'):
            if c not in client_args:
                client_args.append(c)
        client_args = client_args[:3]
    else:
        for c in ('visionos', 'web_music', None) + _PLAYER_CLIENT_FALLBACKS:
            if c not in client_args:
                client_args.append(c)
        if not _js_runtime_available():
            # Same reasoning as the audio path: without a JS runtime only the
            # JS-free clients have a chance.
            client_args = [c for c in client_args if c in ('visionos', 'mweb')]
    deadline = time.time() + _tune('backend.resolve_deadline_sec', 25)
    for client_arg in client_args:
        if time.time() >= deadline:
            last_error = last_error or 'video stream resolution timed out'
            break
        video, audio, err = _extract_video_urls(video_id, client_arg, browser, fmt)
        if not video:
            last_error = err or last_error
            continue
        pair = {'video': video}
        if audio:
            pair['audio'] = audio
        if _url_playable(video, timeout=3.0 if fast else 4.0, attempts=1):
            _video_stream_cache[cache_key] = (time.time(), pair)
            _save_video_stream_disk()
            return dict(pair)
        last_pair = pair
        last_error = ('video URL rejected at fetch '
                      f'(client: {client_arg or "yt-dlp default"})')
    if last_pair and not fast:
        return last_pair
    if fast:
        return {'error': last_error, 'retry': True}
    return {'error': last_error}


# ---------------------------------------------------------------------------
# A/V alignment for matched music videos
#
# The official video is a different edit of the same recording: usually a
# constant intro/outro offset, same tempo. One-time analysis: decode a short
# window of the song, decode a wider window of the video's audio, and
# cross-correlate their log-energy envelopes. The resulting constant offset
# (video_time - song_time) is cached for good, so playback only ever applies a
# seek offset. Analysis runs in the background channel and never in low power.
# ---------------------------------------------------------------------------
_SYNC_SR = _tunei('backend.sync_sr', 8000)
_SYNC_HOP = _tune('backend.sync_hop_sec', 0.05)
_SYNC_WINDOW = _tune('backend.sync_window_sec', 20.0)
_SYNC_SEARCH = _tune('backend.sync_search_sec', 90.0)
_SYNC_TTL = _tunei('backend.sync_ttl_days', 120) * 86400
_sync_map = None
_sync_lock = threading.Lock()


def _sync_map_path():
    return os.path.join(_config_dir(), 'video_sync.json')


def _load_sync_map():
    global _sync_map
    with _sync_lock:
        if _sync_map is not None:
            return _sync_map
        try:
            with open(_sync_map_path(), encoding='utf-8') as f:
                data = json.load(f)
        except Exception:
            data = {}
        _sync_map = data if isinstance(data, dict) else {}
        return _sync_map


def _save_sync_map():
    data = _load_sync_map()
    now = time.time()
    with _sync_lock:
        pruned = {k: v for k, v in data.items()
                  if isinstance(v, dict) and now - (v.get('ts') or 0) < _SYNC_TTL}
        data.clear()
        data.update(pruned)
        payload = dict(data)
    tmp = '%s.%d.tmp' % (_sync_map_path(), os.getpid())
    try:
        os.makedirs(os.path.dirname(tmp), exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(payload, f, separators=(',', ':'))
        os.replace(tmp, _sync_map_path())
    except Exception:
        pass


def _decode_features(url, start, duration):
    """Decode a remote segment; returns (log-energy envelope, onset envelope)."""
    ffmpeg = os.environ.get('P100_FFMPEG') or 'ffmpeg'
    ua = ('Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
          '(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36')
    base = [ffmpeg, '-v', 'error', '-user_agent', ua]
    # Output options must come after the input; keeping them before -i made
    # ffmpeg treat them as input options and produce no data.
    tail = ['-vn', '-ac', '1', '-ar', str(_SYNC_SR), '-f', 's16le', '-']

    def run(seek_first):
        cmd = list(base)
        if seek_first:
            cmd += ['-ss', f'{max(0.0, start):.3f}']
        cmd += ['-i', url]
        if not seek_first:
            cmd += ['-ss', f'{max(0.0, start):.3f}']
        cmd += ['-t', f'{max(1.0, duration):.3f}'] + tail
        try:
            proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, timeout=75,
                                  creationflags=_NO_WINDOW)
        except Exception:
            return b''
        return proc.stdout or b''

    data = run(True)
    # A container without cues gives little/no data for a fast seek; retry
    # with decoded-range seeking (slower, but bounded to ~3 minutes).
    if len(data) < _SYNC_SR:  # < 1 s decoded
        data = run(False)
    n = len(data) // 2
    if n < _SYNC_SR:
        return [], []
    samples = array.array('h')
    samples.frombytes(data[:n * 2])
    if sys.byteorder != 'little':
        samples.byteswap()

    win = max(1, int(0.02 * _SYNC_SR))
    hop = max(1, int(_SYNC_HOP * _SYNC_SR))
    rms = []
    for off in range(0, len(samples) - win, hop):
        s = 0
        for v in samples[off:off + win]:
            s += v * v
        rms.append(math.sqrt(float(s) / win))
    if len(rms) < 4:
        return [], []
    energy = [math.log1p(v) for v in rms]
    onset = [0.0]
    for i in range(1, len(rms)):
        d = math.log(rms[i] + 1.0) - math.log(rms[i - 1] + 1.0)
        onset.append(d if d > 0.0 else 0.0)
    return energy, onset


def _normalise(x):
    n = len(x)
    if n == 0:
        return []
    mean = sum(x) / n
    var = sum((v - mean) ** 2 for v in x) / n
    if var <= 1e-12:
        return [0.0] * n
    sd = math.sqrt(var)
    return [(v - mean) / sd for v in x]


def _correlate_best(song, video):
    """Pearson correlation of the normalised envelopes at every lag."""
    n = len(song)
    if n == 0 or len(video) < n:
        return None
    best_score = -2.0
    best_lag = 0
    for lag in range(0, len(video) - n + 1):
        acc = 0.0
        for i in range(n):
            acc += song[i] * video[lag + i]
        score = acc / n
        if score > best_score:
            best_score = score
            best_lag = lag
    return best_score, best_lag


def get_video_sync_cached(song_id, video_id):
    """Cached A/V offset only -- never analyses. Lets the player start the
    video immediately and apply a freshly computed offset later."""
    key = (song_id or '').strip() + '|' + (video_id or '').strip()
    hit = _load_sync_map().get(key)
    if isinstance(hit, dict) and time.time() - (hit.get('ts') or 0) < _SYNC_TTL:
        return {'ok': True, 'cached': True, 'offset': hit.get('offset', 0.0),
                'confidence': hit.get('confidence', 0.0),
                'confident': bool(hit.get('confident'))}
    return {'ok': True, 'cached': False, 'offset': 0.0, 'confidence': 0.0,
            'confident': False}


def get_video_sync(yt, song_id, video_id, song_duration=0):
    """Constant A/V offset for a matched music video, cached on disk."""
    song_id = (song_id or '').strip()
    video_id = (video_id or '').strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{11}', song_id or '') or \
       not re.fullmatch(r'[A-Za-z0-9_-]{11}', video_id or ''):
        return _err('Invalid video id', kind='backend')
    key = song_id + '|' + video_id
    cache = _load_sync_map()
    hit = cache.get(key)
    if isinstance(hit, dict) and time.time() - (hit.get('ts') or 0) < _SYNC_TTL:
        return {'ok': True, 'offset': hit.get('offset', 0.0),
                'confidence': hit.get('confidence', 0.0),
                'confident': bool(hit.get('confident')), 'cached': True}

    try:
        duration = int(song_duration or 0)
    except (TypeError, ValueError):
        duration = 0
    if duration <= 0:
        meta = get_track_meta(yt, song_id)
        if isinstance(meta, dict) and 'error' not in meta:
            duration = int(meta.get('durationSec') or 0)
    if duration <= 0:
        return _err('Song duration unavailable', kind='backend')

    song = get_stream_url(song_id)
    if not isinstance(song, dict) or 'error' in song:
        return song if isinstance(song, dict) else _err('Could not resolve song audio')
    # Analyse the matched video's audio-only adaptive stream: ~3 MB per
    # window instead of range-reading the muxed video file. Never downloads
    # the whole video.
    va = get_stream_url(video_id)
    if isinstance(va, dict) and va.get('url'):
        video_url = va['url']
    else:
        vid = get_video_urls(video_id)
        if not isinstance(vid, dict) or 'error' in vid:
            return vid if isinstance(vid, dict) else _err('Could not resolve video audio')
        video_url = vid.get('audio') or vid.get('video')

    window = 24.0
    estimates = []
    # Three independent windows. A single cross-correlation can latch onto a
    # repeated beat pattern and return a plausible-looking but wrong offset
    # (the video then jumps to an unrelated point). Requiring a majority to
    # agree, weighted by confidence, rejects those while accepting real pairs
    # where one window happens to be repetitive.
    for frac in (0.25, 0.50, 0.75):
        s_start = max(5.0, min(duration * frac, duration - window - 5.0))
        v_start = max(0.0, s_start - 45.0)
        v_dur = min(window + 90.0, 180.0)
        song_energy, song_onset = _decode_features(song.get('url'), s_start, window)
        video_energy, video_onset = _decode_features(video_url, v_start, v_dur)
        if not song_energy or not video_energy:
            return _err('Could not decode audio for sync', kind='backend')
        candidates = []
        for feat_song, feat_video in ((song_energy, video_energy),
                                      (song_onset, video_onset)):
            best = _correlate_best(_normalise(feat_song), _normalise(feat_video))
            if best:
                candidates.append(best)
        if not candidates:
            return _err('Could not align the streams', kind='backend')
        conf, lag = max(candidates, key=lambda c: c[0])
        estimates.append(((v_start + lag * _SYNC_HOP) - s_start, conf))

    best_cluster = []
    for entry in estimates:
        cluster = [e for e in estimates if abs(e[0] - entry[0]) <= 0.5]
        if len(cluster) > len(best_cluster):
            best_cluster = cluster
    confidence = min(e[1] for e in best_cluster) if best_cluster else 0.0
    confident = len(best_cluster) >= 2 and confidence >= 0.45
    if confident:
        weight = sum(e[1] for e in best_cluster) or 1.0
        offset = sum(e[0] * e[1] for e in best_cluster) / weight
    else:
        offset = 0.0
    cache[key] = {'offset': offset, 'confidence': confidence,
                  'confident': confident, 'ts': time.time()}
    _save_sync_map()
    return {'ok': True, 'offset': offset, 'confidence': confidence,
            'confident': confident, 'cached': False}


_prefetch_pool = None
_prefetch_pool_lock = threading.Lock()


def _get_prefetch_pool():
    """Shared pool so its threads (and their warm YoutubeDL instances) live
    across prefetch commands instead of being rebuilt per batch."""
    global _prefetch_pool
    with _prefetch_pool_lock:
        if _prefetch_pool is None:
            _prefetch_pool = ThreadPoolExecutor(max_workers=4,
                                                thread_name_prefix='cf-prefetch')
        return _prefetch_pool


def prefetch_stream_urls(video_ids):
    """Resolve several ids at once (speculative prefetch).

    The command pipe answers one command at a time, so the parallelism has to
    live here: four worker threads, each with its own persistent YoutubeDL
    instance, turn a 10-row prefetch from ~30 s into a few seconds -- and the
    first rows are ready in under three. A hard deadline keeps a single stuck
    extraction from holding the command (and therefore the whole channel) past
    the app's read timeout; partial results are still useful because each URL
    is written to the shared disk cache as it resolves.
    """
    ids = [v for v in video_ids if re.fullmatch(r'[A-Za-z0-9_-]{11}', v or '')]
    if not ids:
        return {'urls': []}

    def one(vid):
        try:
            result = get_stream_url(vid)
        except Exception:
            return None
        url = result.get('url') if isinstance(result, dict) else None
        return {'id': vid, 'url': url} if url else None

    from concurrent.futures import as_completed
    budget = max(1.0, _tune('backend.prefetch_deadline_sec', 20))
    pool = _get_prefetch_pool()
    futures = [pool.submit(one, vid) for vid in ids]
    rows = []
    deadline = time.time() + budget
    try:
        for fut in as_completed(futures, timeout=budget):
            try:
                row = fut.result()
            except Exception:
                continue
            if row:
                rows.append(row)
            if time.time() >= deadline:
                break
    except Exception:
        print(f"[prefetch] deadline {budget:.0f}s: {len(rows)}/{len(ids)} resolved",
              file=sys.stderr, flush=True)
    return {'urls': rows}


# Repeat/back-navigation searches hit YTM every time; a small TTL cache makes
# them instant. Errors are never cached (a transient failure must retry).
_SEARCH_TTL = _tunei('backend.search_ttl_sec', 900)  # seconds
_SEARCH_CACHE_MAX = _tunei('backend.search_cache_max', 100)
_search_cache = {}


def _cached_search(query, fn, scope=''):
    key = (scope, (query or '').strip().lower())
    if not key[1]:
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


# ---------------------------------------------------------------------------
# Studio: waveform + beat analysis
#
# ffmpeg decodes the file to mono float PCM; the waveform is a multi-resolution
# peak pyramid (so zooming never re-decodes) and beats come from a dependency-
# free spectral-flux-style onset detector + tempo autocorrelation. Results are
# written as a compact text file (see fetch_studio_analysis in ytdlp.cpp) so the
# C++ side needs no JSON array parser.
# ---------------------------------------------------------------------------

_STUDIO_SR = 11025


def _read_mono_pcm(path):
    ffmpeg = os.environ.get('P100_FFMPEG') or 'ffmpeg'
    cmd = [ffmpeg, '-v', 'error', '-i', path, '-t', '1800',
           '-f', 'f32le', '-ac', '1', '-ar', str(_STUDIO_SR), '-']
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=180, creationflags=_NO_WINDOW)
    except FileNotFoundError:
        return None, 'ffmpeg not found (install ffmpeg for waveform and beat analysis)'
    except Exception as e:
        return None, 'ffmpeg failed: ' + str(e)
    if proc.returncode != 0 or not proc.stdout:
        msg = (proc.stderr or b'').decode('utf-8', 'replace').strip()
        return None, 'Could not decode audio' + (': ' + msg if msg else '')
    raw = proc.stdout
    raw = raw[:len(raw) - (len(raw) % 4)]
    from array import array
    samples = array('f')
    samples.frombytes(raw)
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples, None


def _wave_pyramid(samples, levels):
    n = len(samples)
    out = []
    for target in levels:
        buckets = min(target, n) if n else 0
        if buckets <= 0:
            out.append((0, [], []))
            continue
        step = n / float(buckets)
        mins, maxs = [], []
        for b in range(buckets):
            a = int(b * step)
            c = int((b + 1) * step)
            if c <= a:
                c = min(n, a + 1)
            chunk = samples[a:c]
            if chunk:
                mins.append(min(chunk))
                maxs.append(max(chunk))
            else:
                mins.append(0.0)
                maxs.append(0.0)
        out.append((buckets, mins, maxs))
    return out


def _detect_beats(samples, sample_rate):
    n = len(samples)
    hop = 256
    frames = n // hop
    if frames < 8:
        return 0.0, 0.0, [], []

    energy = [0.0] * frames
    for i in range(frames):
        a = i * hop
        s = 0.0
        for v in samples[a:a + hop]:
            s += v * v
        energy[i] = s

    onset = [0.0] * frames
    for i in range(1, frames):
        d = energy[i] - energy[i - 1]
        onset[i] = d if d > 0.0 else 0.0

    win = max(3, int(0.5 * sample_rate / hop))
    peaks = []
    last_t = -999.0
    for i in range(1, frames - 1):
        lo = max(0, i - win)
        hi = min(frames, i + win + 1)
        m = sum(onset[lo:hi]) / float(max(1, hi - lo))
        if onset[i] > m * 1.4 and onset[i] >= onset[i - 1] and onset[i] >= onset[i + 1]:
            t = i * hop / float(sample_rate)
            if t - last_t >= 0.12:
                peaks.append(t)
                last_t = t

    if len(peaks) < 4:
        return 0.0, 0.0, peaks, []

    min_lag = max(1, int(0.30 * sample_rate / hop))
    max_lag = max(min_lag + 1, int(1.00 * sample_rate / hop))
    best_lag, best_score = 0, 0.0
    for lag in range(min_lag, max_lag + 1):
        s = 0.0
        for i in range(lag, frames):
            s += onset[i] * onset[i - lag]
        if s > best_score:
            best_score, best_lag = s, lag
    if best_lag <= 0:
        return 0.0, 0.0, peaks, []

    # Autocorrelation favours the half-tempo (a click every beat also lines up
    # every other beat). If the octave-up lag correlates strongly too, prefer
    # it so the grid is twice as fine.
    if best_lag > 2:
        lo = best_lag // 2
        hi = best_lag - lo
        best_half, best_half_score = 0, 0.0
        for half in ({lo, hi}):
            if half <= 0:
                continue
            sh = 0.0
            for i in range(half, frames):
                sh += onset[i] * onset[i - half]
            if sh > best_half_score:
                best_half, best_half_score = half, sh
        if best_half and best_half_score >= best_score * 0.55:
            best_lag, best_score = best_half, best_half_score

    period = best_lag * hop / float(sample_rate)
    bpm = 60.0 / period
    while bpm < 60.0:
        bpm *= 2.0
        period /= 2.0
    while bpm > 200.0:
        bpm /= 2.0
        period *= 2.0
    total = sum(o * o for o in onset) + 1e-9
    confidence = max(0.0, min(1.0, best_score / total))

    duration = n / float(sample_rate)
    steps = 24
    best_phase, best_phase_score = 0.0, -1.0
    for k in range(steps):
        phase = period * k / float(steps)
        s = 0.0
        t = phase
        while t < duration:
            fi = int(t * sample_rate / hop)
            if 0 <= fi < frames:
                s += onset[fi]
            t += period
        if s > best_phase_score:
            best_phase_score, best_phase = s, phase

    beats = []
    t = best_phase
    while t < duration:
        beats.append(round(t, 3))
        t += period
    return bpm, confidence, beats, beats[::4]


def _write_wave_cache(path, duration, bpm, confidence, beats, downbeats, pyramid):
    tmp = path + '.tmp'
    try:
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write('CFWAVE 1\n')
            f.write('duration %.6f\n' % duration)
            f.write('bpm %.4f\n' % bpm)
            f.write('confidence %.4f\n' % confidence)
            f.write('beats ' + ','.join('%.3f' % x for x in beats) + '\n')
            f.write('downbeats ' + ','.join('%.3f' % x for x in downbeats) + '\n')
            for buckets, mins, maxs in pyramid:
                f.write('wave %d\n' % buckets)
                for i in range(buckets):
                    f.write('%.5f,%.5f\n' % (mins[i], maxs[i]))
        os.replace(tmp, path)
        return True
    except Exception:
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False


def analyze_audio(payload):
    payload = payload or {}
    path = payload.get('path') or ''
    cache = payload.get('cache') or ''
    if not path or not cache:
        return _err('Missing path/cache', kind='backend')
    if not os.path.isfile(path):
        return _err('Audio file not found', kind='not_found')
    if os.path.isfile(cache) and os.path.getsize(cache) > 0:
        return {'ok': True, 'cache': cache, 'cached': True}

    samples, err = _read_mono_pcm(path)
    if err:
        return _err(err, kind='backend')
    n = len(samples)
    if n < 2048:
        return _err('Audio file is too short to analyse', kind='backend')

    duration = n / float(_STUDIO_SR)
    pyramid = _wave_pyramid(samples, (64, 256, 1024, 4096, 16384))
    bpm, confidence, beats, downbeats = _detect_beats(samples, _STUDIO_SR)
    if not _write_wave_cache(cache, duration, bpm, confidence, beats, downbeats,
                             pyramid):
        return _err('Could not write the analysis cache', kind='backend')
    return {'ok': True, 'cache': cache, 'duration': duration, 'bpm': bpm,
            'confidence': confidence, 'beats': len(beats)}


_GUEST_MESSAGES = {
    'recommendations':
        'Sign in to YouTube Music to get personalized recommendations.',
    'library_playlists':
        'Sign in to YouTube Music to see your playlists.',
    'rate_song':
        'Sign in to YouTube Music to like songs on your account.',
    'create_playlist':
        'Sign in to YouTube Music to create playlists.',
    'delete_playlist':
        'Sign in to YouTube Music to delete playlists.',
    'edit_playlist':
        'Sign in to YouTube Music to edit playlists.',
    'add_playlist_items':
        'Sign in to YouTube Music to change playlists.',
    'remove_playlist_items':
        'Sign in to YouTube Music to change playlists.',
    'podcasts_library':
        'Sign in to YouTube Music to see your saved podcasts.',
    'episodes_later':
        'Sign in to YouTube Music to see your saved episodes.',
}


def execute_command(yt, cmd, arg, guest=False):
    """Run one backend command and return its JSON-serialisable result."""
    started = time.perf_counter()

    try:
        if cmd == 'ping':
            # Resync probe: the C++ side sends this after a timed-out
            # command and discards stale replies until the nonce returns.
            try:
                nonce = int(arg) if arg else 0
            except ValueError:
                nonce = 0
            result = {'ok': True, 'nonce': nonce}
        elif guest and cmd in _GUEST_MESSAGES:
            # Anonymous YTMusic() can browse public content, but the
            # account surface stays behind a sign-in. The UI shows a
            # dedicated prompt for these.
            result = _err(_GUEST_MESSAGES[cmd], kind='auth')
        elif cmd == 'recommendations':
            limit = 40
            if arg:
                try:
                    limit = int(arg)
                except ValueError:
                    limit = 40
            result = get_home_tracks(yt, limit=limit)
        elif cmd == 'search':
            result = (_cached_search(arg, lambda: search_tracks(yt, arg), 'search')
                      if arg else {'error': 'Missing query'})
        elif cmd == 'suggest':
            result = get_search_suggestions(yt, arg or '')
        elif cmd == 'search_all':
            result = (_cached_search(arg, lambda: search_all(yt, arg), 'search_all')
                      if arg else {'error': 'Missing query'})
        elif cmd == 'album_search':
            result = (_cached_search(arg, lambda: search_albums(yt, arg), 'album_search')
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
            result = (_cached_search(arg, lambda: search_artists(yt, arg), 'artist_search')
                      if arg else {'error': 'Missing query'})
        elif cmd == 'play':
            result = get_stream_url(arg) if arg else {'error': 'Missing video_id'}
        elif cmd == 'play_video':
            bits = (arg or '').split()
            result = (get_video_urls(bits[0], max_height=int(bits[1]) if len(bits) > 1 else 720)
                      if bits else {'error': 'Missing video_id'})
        elif cmd == 'play_video_fast':
            bits = (arg or '').split()
            result = (get_video_urls(bits[0], max_height=int(bits[1]) if len(bits) > 1 else 720,
                                     fast=True)
                      if bits else {'error': 'Missing video_id'})
        elif cmd == 'play_video_fresh':
            bits = (arg or '').split()
            result = (get_video_urls(bits[0], fresh=True,
                                     max_height=int(bits[1]) if len(bits) > 1 else 720)
                      if bits else {'error': 'Missing video_id'})
        elif cmd == 'musicvideo':
            result = get_music_video(yt, arg) if arg else {'error': 'Missing video_id'}
        elif cmd == 'videosync_cached':
            bits = (arg or '').split()
            result = (get_video_sync_cached(bits[0], bits[1])
                      if len(bits) >= 2 else {'error': 'Usage: videosync_cached <song> <video>'})
        elif cmd == 'videosync':
            bits = (arg or '').split()
            result = (get_video_sync(yt, bits[0], bits[1],
                                     int(bits[2]) if len(bits) > 2 else 0)
                      if len(bits) >= 2 else {'error': 'Usage: videosync <song> <video> [dur]'})
        elif cmd == 'prefetch':
            result = prefetch_stream_urls((arg or '').split())
        elif cmd == 'play_fresh':
            result = get_stream_url(arg, fresh=True) if arg else {'error': 'Missing video_id'}
        elif cmd == 'lyrics':
            result = get_lyrics(yt, arg) if arg else {'error': 'Missing video_id'}
        elif cmd == 'analyze':
            result = analyze_audio(_json_arg(arg))
        elif cmd == 'meta':
            result = get_track_meta(yt, arg) if arg else {'error': 'Missing video_id'}
        elif cmd == 'library_playlists':
            result = yt.get_library_playlists(limit=100)
        elif cmd == 'playlist':
            result = yt.get_playlist(arg, limit=1000) if arg else {'error': 'Missing playlist_id'}
        elif cmd == 'spotify_import':
            result = import_spotify(yt, arg) if arg else {'error': 'Missing Spotify playlist'}
        elif cmd == 'spotify_library':
            try:
                result = _spotify_library()
            except Exception as e:
                result = _err(str(e))
        elif cmd == 'podcasts_library':
            result = get_podcast_library(yt)
        elif cmd == 'podcasts_search':
            result = (_cached_search(arg, lambda: search_podcasts(yt, arg), 'podcasts_search')
                      if arg else {'error': 'Missing query'})
        elif cmd == 'podcast':
            result = get_podcast_details(yt, arg) if arg else {'error': 'Missing podcast_id'}
        elif cmd == 'episodes_later':
            result = get_episodes_later(yt)
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
        else:
            result = {'error': f'Unknown command: {cmd}'}
    except Exception as e:
        result = {'error': f'Unhandled error in command {cmd!r}: {str(e)}'}

    if isinstance(result, dict) and 'error' in result and 'kind' not in result:
        result['kind'] = _classify_error(result['error'])

    # One line per command in backend.log (the diagnostics bundle and the
    # Settings log tail both read it), with the channel role and timing so
    # a slow provider or a queue is visible without a debugger.
    elapsed_ms = (time.perf_counter() - started) * 1000
    trace_arg = ''
    if arg:
        trace_arg = ' ' + (arg if len(arg) <= 48 else arg[:48] + '...')
    status = 'ok'
    if isinstance(result, dict) and result.get('error'):
        status = 'error'
    with _TRACE_LOCK:
        print(f"[{_BACKEND_ROLE}] {cmd}{trace_arg} {elapsed_ms:.0f}ms {status}",
              file=sys.stderr, flush=True)
    return result


def _run_command_loop_concurrent(yt, stdin, stdout, guest, session_factory):
    """Id-tagged, multi-threaded command loop used by the main channel.

    Requests arrive as `@<id> <command> [arg]` and each reply is written as
    `@<id> <json>` when its worker finishes, so a slow album/HTTP call cannot
    delay a search (or anything else) behind it. Each worker gets its own
    YTMusic instance because ytmusicapi's requests session is not safe to share
    across threads. Ordering is irrelevant: the client routes by id.
    """
    from concurrent.futures import ThreadPoolExecutor

    workers = max(1, int(os.environ.get('P100_BACKEND_WORKERS', '4')))
    pool = ThreadPoolExecutor(max_workers=workers)
    local = threading.local()
    write_lock = threading.Lock()

    def thread_yt():
        instance = getattr(local, 'yt', None)
        if instance is None:
            try:
                instance = session_factory() if session_factory else yt
            except Exception as e:
                print(f"[{_BACKEND_ROLE}] per-thread session failed: {e}",
                      file=sys.stderr, flush=True)
                instance = yt
            local.yt = instance
        return instance

    def run(req_id, cmd, arg):
        try:
            result = execute_command(thread_yt(), cmd, arg, guest)
        except Exception as e:
            result = {'error': f'Unhandled error in command {cmd!r}: {e}',
                      'kind': _classify_error(str(e))}
        line = '@%s %s' % (req_id, json.dumps(result, ensure_ascii=False))
        with write_lock:
            stdout.write(line + '\n')
            stdout.flush()

    for line in stdin:
        line = line.strip()
        if not line:
            continue
        if line == 'quit':
            break
        if line.startswith('@'):
            req_id, _, rest = line[1:].partition(' ')
            if not rest:
                continue
            cmd, _, arg = rest.partition(' ')
            pool.submit(run, req_id, cmd, arg or None)
        else:
            # Mixed-protocol client (tests, manual tools): answer inline.
            parts = line.split(' ', 1)
            result = execute_command(yt, parts[0],
                                     parts[1] if len(parts) > 1 else None, guest)
            with write_lock:
                stdout.write(json.dumps(result, ensure_ascii=False) + '\n')
                stdout.flush()
    pool.shutdown(wait=True)


def run_command_loop(yt, stdin=None, stdout=None, guest=False,
                     session_factory=None):
    """Read newline-delimited commands from stdin until EOF/quit.

    Every reply is one JSON object per line. Errors are stamped with a
    machine-readable kind so the C++ UI can distinguish auth, network and
    backend failures without parsing the message text.
    """
    stdin = stdin if stdin is not None else sys.stdin
    stdout = stdout if stdout is not None else sys.stdout
    # The C++ side speaks UTF-8. Reconfigure our own pipes as well, so a
    # backend started without PYTHONIOENCODING (manual run, unusual locale)
    # still decodes queries and encodes titles correctly.
    for stream in (stdin, stdout):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(encoding="utf-8")
            except (ValueError, OSError):
                pass
    global _yt_client
    _yt_client = yt
    if os.environ.get('P100_BACKEND_CONCURRENT') == '1':
        _run_command_loop_concurrent(yt, stdin, stdout, guest, session_factory)
        return
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        parts = line.split(' ', 1)
        cmd = parts[0]
        arg = parts[1] if len(parts) > 1 else None
        if cmd == 'quit':
            break
        result = execute_command(yt, cmd, arg, guest)
        print(json.dumps(result, ensure_ascii=False), file=stdout)
        stdout.flush()
