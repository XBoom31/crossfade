#pragma once
#include <string>
#include <GL/gl.h>

// GL 1.2 enum used for texture wrapping; the Windows GL/gl.h only covers 1.1.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

struct TextureInfo
{
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

// Dominant, display-friendly colour extracted from a decoded thumbnail.
// `ready` means the image has been decoded and analysed; `valid` means the
// result is colourful enough to use (grey/monochrome covers fall back to the
// app accent). Both are false while the thumbnail is still loading.
struct TextureColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    bool ready = false;
    bool valid = false;
};

GLuint get_texture(const std::string &url);
// Returns id + pixel dimensions for a URL, kicking off a background load
// if it isn't cached yet. id == 0 while loading / after a failed fetch.
TextureInfo texture_info(const std::string &url);
// Cached dominant colour for a thumbnail. Cheap to call every frame: the
// analysis happens once, in the loader, while the image is already decoded.
TextureColor texture_dominant_color(const std::string &url);
// Cover-fit UVs: fills a box of box_w x box_h with the texture, cropping
// the longer axis so the aspect ratio is preserved (i.e. no stretching).
// Returns false if the box or texture is degenerate.
bool cover_uv(const TextureInfo &info, float box_w, float box_h,
              float &u0, float &v0, float &u1, float &v1);
void process_texture_queue();
// True when at least one decoded thumbnail is waiting for a main-thread GPU
// upload. The main loop uses it to trigger an immediate repaint instead of
// waiting out the idle frame interval.
bool has_pending_textures();
void clear_texture_cache();
void ensure_cache_dir();

// Returns the local file path of a cached thumbnail, downloads if missing.
std::string get_cached_thumbnail_path(const std::string &url);
// Same lookup without touching the network: "" when not already cached.
std::string thumbnail_cache_path(const std::string &url);
// True if the thumbnail is already present in the on-disk cache (no download).
bool thumbnail_is_cached(const std::string &url);