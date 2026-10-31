// (0.11.4) The game's log files, each run's in a folder of its own: logs\<when the run started>\ beside the game (characters.log,
// netplay_host.txt, crash.log, killfeed.log and the rest). At most 10 runs' folders are kept: when a run makes its folder, the oldest
// ones past 10 are deleted. The logs other players send the host (net_session.cpp) go in the host's own run's folder, under players\,
// so they count in the same 10.
#include "log_files.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <system_error>

namespace {
    constexpr size_t runs_kept = 10;
    std::mutex folder_mutex;
    std::filesystem::path run_folder;
    std::string run_name;

    std::filesystem::path logs_root() {
        std::error_code ec;
        return std::filesystem::absolute(std::filesystem::path("logs"), ec);   // (main.cpp makes the game's folder the working one)
    }

    bool is_run_folder(const std::filesystem::directory_entry& entry) {
        std::error_code ec;
        const std::string name = entry.path().filename().string();
        return entry.is_directory(ec) && name.size() >= 19 && name[4] == '-' && name[7] == '-' && name[10] == '_';
    }

    std::vector<std::filesystem::path> runs_newest_first() {
        std::vector<std::filesystem::path> runs;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(logs_root(), std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (is_run_folder(entry)) {
                runs.push_back(entry.path());
            }
        }
        // (the names are when each run started: they sort in time order)
        std::sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) { return a.filename().string() > b.filename().string(); });
        return runs;
    }

    void make_run_folder() {
        if (!run_folder.empty()) {
            return;
        }
        std::error_code ec;
        const std::filesystem::path root = logs_root();
        std::filesystem::create_directories(root, ec);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
        // (a second window of the game started in the same second: _2, _3...)
        for (int k = 1; k < 100; k++) {
            const std::string name = k == 1 ? std::string(stamp) : std::string(stamp) + "_" + std::to_string(k);
            if (std::filesystem::create_directory(root / name, ec) && !ec) {
                run_folder = root / name;
                run_name = name;
                break;
            }
        }
        if (run_folder.empty()) {
            run_folder = root;   // (couldn't make one: the logs folder itself)
            run_name = "logs";
            return;
        }
        // the oldest past 10, this one counted
        const std::vector<std::filesystem::path> runs = runs_newest_first();
        for (size_t i = runs_kept; i < runs.size(); i++) {
            if (runs[i] != run_folder) {
                std::filesystem::remove_all(runs[i], ec);
            }
        }
    }
}

std::filesystem::path conker::logs::folder() {
    std::lock_guard lock{ folder_mutex };
    make_run_folder();
    return run_folder;
}

std::filesystem::path conker::logs::path(const char* name) {
    return folder() / name;
}

std::string conker::logs::session_name() {
    std::lock_guard lock{ folder_mutex };
    make_run_folder();
    return run_name;
}

std::vector<std::filesystem::path> conker::logs::earlier_sessions() {
    const std::filesystem::path mine = folder();
    std::vector<std::filesystem::path> runs = runs_newest_first();
    runs.erase(std::remove(runs.begin(), runs.end(), mine), runs.end());
    return runs;
}
