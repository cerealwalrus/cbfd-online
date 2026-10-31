#pragma once
// (0.11.4) The game's log files, each run's in a folder of its own: logs\<when it started>\ beside the game (log_files.cpp).
#include <filesystem>
#include <string>
#include <vector>

namespace conker {
    namespace logs {
        // logs\<this run>\ (made the first time it's asked for; at most 10 runs' folders are kept, the oldest deleted)
        std::filesystem::path folder();
        // a file of this run's: folder() / name
        std::filesystem::path path(const char* name);
        // this run's folder's name (when it started: 2026-10-31_01-52-07)
        std::string session_name();
        // the folders of the runs before this one, newest first
        std::vector<std::filesystem::path> earlier_sessions();
    }
}
