#pragma once

#include <string>

// In-app backup/restore to a single cross-platform ZIP. The heavy lifting is
// done by py/cf_backend/transfer.py (stdlib zipfile); this module prepares a
// consistent library snapshot, spawns the helper, and reports progress.
namespace data_transfer
{
struct Sections
{
    bool settings = true;
    bool auth = true;
    bool library = true;
    bool album_cache = true;
    bool artist_cache = true;
    bool lyrics = true;
    bool thumbs = true;
    bool audio = false;
    bool logs = false;
};

std::string default_export_path();
std::string default_import_path();

bool export_async(const std::string &zip_path, const Sections &sections, std::string &error);
bool export_running();
// Returns and clears the last export result, or "" when there is none.
std::string take_export_result();

// Starts the detached import helper. It waits for mark_shutdown_ready() before
// touching any files, so the app must request shutdown after this returns true.
bool start_import(const std::string &zip_path, std::string &error);
void mark_shutdown_ready();

// Reads and removes <config>/transfer-result.json (from the previous import).
std::string take_result_message();

std::string helper_script_path();
} // namespace data_transfer
