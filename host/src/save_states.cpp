// Save states (issue #94; 0.11.4: their keys taken out, see init): F5 saved the game as it is right now to the current slot, F7 loaded it back,
// F6 picks the next slot (1 to 9). A state is a file in the savestates folder beside the settings.
//
// How a recompiled game can be saved at all is told in the runtime (ultramodern/save_states.hpp): a
// state is taken at a safe point, when every game thread but the running one waits (and the running
// one is at a known call), and loaded at a safe point where every thread is where it was in the state.
// So a key only asks for a save or a load: it's done at the next such moment (within a frame or so),
// and the result is shown in the messages' corner, top left.
//
// A load can only be done where the game's threads wait as they did when the state was taken (the
// runtime checks it: where each is in the game's code, so a state from an earlier version loads too,
// unless that version's game code differs where its threads wait). The game's threads wait at the same
// few places all through the game, so that's within a frame or so almost always; should they not, it
// keeps trying for a few seconds, then says it couldn't.
//
// Each state holds the runtime's (the game's memory, each thread's registers, the clock and the timers,
// the VI's state). The game's own save file (the EEPROM) isn't in a state: loading one never takes back
// what the game has saved.
//
// The file: "CBFDSTAT", the file's version (u32), the program's version (u32 length, text), when it was
// taken (u64, seconds since 1970), a block for achievements' progress (u32 length, bytes: empty now; it
// held RetroAchievements', which was removed, and stays so the files keep their layout), then the
// runtime's state (u32 length, bytes; librecomp/save_states.hpp).

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

#include "librecomp/game.hpp"
#include "librecomp/save_states.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/save_states.hpp"

#include "conker.hpp"

namespace {
    using clock = std::chrono::steady_clock;

    constexpr char file_magic[8] = { 'C', 'B', 'F', 'D', 'S', 'T', 'A', 'T' };
    constexpr uint32_t file_version = 1;
    constexpr int slot_count = 9;
    // How long a save or a load keeps trying for a moment it can be done at.
    constexpr auto give_up_after = std::chrono::seconds(5);

    enum class Request { None, Save, Load };

    std::atomic<Request> request = Request::None;
    std::atomic<int> current_slot = 1;
    // The request's, set before it's made (and read only by the safe point callback until it's done).
    int request_slot = 1;
    clock::time_point requested_at;
    std::vector<uint8_t> load_state;
    std::vector<uint8_t> load_progress;
    std::string load_version;
    bool load_same_build = true;
    bool logged_not_now = false;
    std::string last_failure;
    uint32_t attempts = 0;

    std::filesystem::path folder() {
        return recomp::get_config_path() / "savestates";
    }

    std::filesystem::path slot_path(int slot) {
        return folder() / ("slot" + std::to_string(slot) + ".state");
    }

    // savestates_log.txt beside the settings, while save states are new (docs/save-states.md, Debugging): each
    // save and load, where each thread waited, and why one couldn't be done. Written from the first save or load.
    std::mutex log_mutex;
    void log_line(const char* format, ...) {
        static FILE* file = std::fopen((recomp::get_config_path() / "savestates_log.txt").string().c_str(), "w");
        if (file == nullptr) {
            return;
        }
        std::lock_guard lock{log_mutex};
        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);
        std::fputc('\n', file);
        std::fflush(file);
    }

    // --- The file ---

    template <typename T> void put(std::vector<uint8_t>& out, const T& value) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    void put_block(std::vector<uint8_t>& out, const void* data, size_t size) {
        put(out, (uint32_t)size);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }

    // Written on a thread of its own (a state is 8MB), to a temporary file first, so a slot is never
    // left half written.
    std::mutex write_mutex;
    void write_state(int slot, std::vector<uint8_t> state, std::vector<uint8_t> progress) {
        std::thread([slot, state = std::move(state), progress = std::move(progress)]() {
            std::lock_guard lock{write_mutex};
            std::vector<uint8_t> out;
            out.reserve(state.size() + progress.size() + 256);
            out.insert(out.end(), file_magic, file_magic + sizeof(file_magic));
            put(out, file_version);
            const std::string version = recomp::get_project_version().to_string();
            put_block(out, version.data(), version.size());
            put(out, (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            put_block(out, progress.data(), progress.size());
            put_block(out, state.data(), state.size());

            std::error_code ec;
            std::filesystem::create_directories(folder(), ec);
            const std::filesystem::path path = slot_path(slot);
            std::filesystem::path temporary = path;
            temporary += ".new";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
                if (!file.good()) {
                    log_line("[save] slot %d: couldn't write %s", slot, temporary.string().c_str());
                    conker::notices::show("Couldn't write the state to slot " + std::to_string(slot));
                    return;
                }
            }
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                log_line("[save] slot %d: couldn't replace %s (%s)", slot, path.string().c_str(), ec.message().c_str());
                conker::notices::show("Couldn't write the state to slot " + std::to_string(slot));
                return;
            }
            log_line("[save] slot %d written (%zu bytes)", slot, out.size());
        }).detach();
    }

    // Reads a slot's file into its parts. False, with why, if there's none or it isn't one.
    bool read_state(int slot, std::vector<uint8_t>& state, std::vector<uint8_t>& progress, std::string& version, std::string& error) {
        std::ifstream file(slot_path(slot), std::ios::binary);
        if (!file) {
            error = "Slot " + std::to_string(slot) + " is empty";
            return false;
        }
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t at = 0;
        auto get = [&](void* out, size_t size) {
            if (at + size > data.size()) {
                return false;
            }
            std::memcpy(out, data.data() + at, size);
            at += size;
            return true;
        };
        auto get_block = [&](std::vector<uint8_t>& out) {
            uint32_t size = 0;
            if (!get(&size, sizeof(size)) || at + size > data.size()) {
                return false;
            }
            out.assign(data.begin() + at, data.begin() + at + size);
            at += size;
            return true;
        };
        char magic[sizeof(file_magic)] = {};
        uint32_t version_number = 0;
        uint64_t taken = 0;
        std::vector<uint8_t> version_bytes;
        if (!get(magic, sizeof(magic)) || std::memcmp(magic, file_magic, sizeof(magic)) != 0 ||
            !get(&version_number, sizeof(version_number)) || version_number != file_version ||
            !get_block(version_bytes) || !get(&taken, sizeof(taken)) || !get_block(progress) || !get_block(state)) {
            error = "Slot " + std::to_string(slot) + " isn't a state this version can load";
            return false;
        }
        version.assign(version_bytes.begin(), version_bytes.end());
        return true;
    }

    // --- The online session's states (net_session.cpp) ---
    // A state taken for another machine, or one from another machine's game, loaded: one job at a time, done at
    // the next safe point like a key's. The job's callback is called without the mutex held.

    struct NetJob {
        bool save = false;
        std::vector<uint8_t> state;
        std::function<void(bool, std::vector<uint8_t>&&)> save_done;
        std::function<void(bool)> load_done;
        clock::time_point at;
        uint32_t attempts = 0;
    };
    std::mutex net_mutex;
    std::unique_ptr<NetJob> net_job;
    std::atomic<bool> net_pending = false;
    constexpr auto net_gives_up_after = std::chrono::seconds(8);

    void update_safe_points_wanted() {
        ultramodern::save_states::set_safe_points_wanted(request.load() != Request::None || net_pending.load());
    }

    // (possess-test) what of the extra camera is kept outside the game's memory (possess_test.cpp), at the end of an online state: its
    // bytes, then their size and this mark. A state without them (an older build's) is the runtime's alone.
    constexpr uint32_t extras_mark = 0x58533550u;   // "P5SX"
    void append_extras(uint8_t* rdram, std::vector<uint8_t>& state) {
        std::vector<uint8_t> extras;
        conker::possess_test::net_state_save(rdram, extras);
        const uint32_t size = (uint32_t)extras.size();
        state.insert(state.end(), extras.begin(), extras.end());
        const uint8_t* s = reinterpret_cast<const uint8_t*>(&size);
        state.insert(state.end(), s, s + 4);
        const uint8_t* m = reinterpret_cast<const uint8_t*>(&extras_mark);
        state.insert(state.end(), m, m + 4);
    }
    // the runtime's part of a state (its size), and the extras after it, if it has them
    size_t runtime_part(const std::vector<uint8_t>& state, const uint8_t** extras, size_t* extras_size) {
        *extras = nullptr;
        *extras_size = 0;
        if (state.size() < 8) {
            return state.size();
        }
        uint32_t size = 0, mark = 0;
        std::memcpy(&mark, state.data() + state.size() - 4, 4);
        std::memcpy(&size, state.data() + state.size() - 8, 4);
        if (mark != extras_mark || (uint64_t)size + 8 > state.size()) {
            return state.size();
        }
        *extras = state.data() + state.size() - 8 - size;
        *extras_size = size;
        return state.size() - 8 - size;
    }

    void run_net_job(uint8_t* rdram) {
        std::unique_ptr<NetJob> job;
        bool ok = false;
        {
            std::lock_guard lock{ net_mutex };
            if (!net_job) {
                net_pending = false;
                return;
            }
            net_job->attempts++;
            const bool timed_out = clock::now() - net_job->at > net_gives_up_after;
            bool done = false;
            // (possess-test: nor between a frame's start and its extra camera's view, which is drawn from a copy of the game taken then)
            if (ultramodern::save_states::is_quiet() && !conker::netview::busy() && conker::possess_test::net_state_ready()) {
                std::lock_guard<std::mutex> render_lock(conker::netview::render_mutex());   // (0.10.79) not while a list is drawn
                std::string error;
                if (net_job->save) {
                    ok = recomp::save_states::capture(rdram, net_job->state, error);
                    done = ok;
                    if (ok) {
                        append_extras(rdram, net_job->state);
                    }
                }
                else {
                    const uint8_t* extras = nullptr;
                    size_t extras_size = 0;
                    const size_t runtime_size = runtime_part(net_job->state, &extras, &extras_size);
                    const auto result = recomp::save_states::restore(rdram, std::span<const uint8_t>(net_job->state.data(), runtime_size), error);
                    ok = result == recomp::save_states::LoadResult::Loaded;
                    done = ok || result == recomp::save_states::LoadResult::Invalid;
                    if (ok && extras != nullptr) {
                        conker::possess_test::net_state_load(rdram, extras, extras_size);
                    }
                }
                if (!done) {
                    log_line("[net state] try %u: %s", net_job->attempts, error.c_str());
                }
            }
            if (!done && !timed_out) {
                return;
            }
            log_line("[net state] %s %s after %u tries", net_job->save ? "save" : "load", ok ? "done" : "failed", net_job->attempts);
            job = std::move(net_job);
            net_pending = false;
            update_safe_points_wanted();
        }
        // Called back without the mutex: the session takes its own, and asks for jobs holding it.
        if (job->save) {
            job->save_done(ok, ok ? std::move(job->state) : std::vector<uint8_t>());
        }
        else {
            job->load_done(ok);
        }
    }

    // --- The safe points ---

    void finish() {
        load_state.clear();
        load_state.shrink_to_fit();
        load_progress.clear();
        request = Request::None;
        update_safe_points_wanted();
    }

    // Whether the request has waited too long for a moment it can be done at.
    bool gave_up(const char* what) {
        if (clock::now() - requested_at < give_up_after) {
            return false;
        }
        log_line("[%s] slot %d: gave up after %u tries: %s", what, request_slot, attempts, last_failure.c_str());
        return true;
    }

    void on_safe_point(uint8_t* rdram) {
        if (net_pending.load()) {
            run_net_job(rdram);
        }
        const Request pending = request.load();
        if (pending == Request::None) {
            return;
        }
        attempts++;
        // A state is only taken (or loaded) with nothing on its way to the game; that's checked
        // again, with the timers held, as it's done.
        if (!ultramodern::save_states::is_quiet()) {
            last_failure = "the game was busy";
            if (gave_up(pending == Request::Save ? "save" : "load")) {
                conker::notices::show("Couldn't save now: the game stayed busy. Try again");
                finish();
            }
            return;
        }

        if (pending == Request::Save) {
            std::vector<uint8_t> state;
            std::string error;
            if (!recomp::save_states::capture(rdram, state, error)) {
                last_failure = error;
                if (gave_up("save")) {
                    conker::notices::show("Couldn't save now (" + error + "). Try again");
                    finish();
                }
                return;
            }
            log_line("[save] slot %d taken after %u tries, thread %d running\n%s", request_slot, attempts,
                (int)osGetThreadId(rdram, NULLPTR),
                recomp::save_states::describe_current_threads(rdram).c_str());
            write_state(request_slot, std::move(state), {});
            conker::notices::show("State saved to slot " + std::to_string(request_slot));
            finish();
            return;
        }

        std::string error;
        std::unique_lock<std::mutex> render_lock(conker::netview::render_mutex());   // (0.10.79) not while a list is drawn
        const recomp::save_states::LoadResult result = recomp::save_states::restore(rdram, load_state, error);
        render_lock.unlock();
        switch (result) {
            case recomp::save_states::LoadResult::Loaded:
                log_line("[load] slot %d loaded after %u tries", request_slot, attempts);
                conker::local_achievements::state_loaded();
                conker::notices::show("Loaded the state in slot " + std::to_string(request_slot));
                finish();
                break;
            case recomp::save_states::LoadResult::NotNow:
                // Where the threads differ, the first time (it's tried at each safe point).
                if (!logged_not_now) {
                    logged_not_now = true;
                    log_line("[load] slot %d not now: %s\n%s%s", request_slot, error.c_str(),
                        recomp::save_states::describe_threads(rdram, load_state).c_str(),
                        recomp::save_states::describe_current_threads(rdram).c_str());
                }
                last_failure = error;
                if (gave_up("load")) {
                    // A state from another version loads as long as the game's code its threads wait in
                    // is the same; one that never fits most likely comes from a version that changed it.
                    // One from this build was most likely asked for in the middle of something.
                    if (!load_same_build) {
                        conker::notices::show("Couldn't load slot " + std::to_string(request_slot) +
                            ": it was saved by another version of the program (" + load_version + "), which changed the game's code");
                    }
                    else {
                        conker::notices::show("Couldn't load slot " + std::to_string(request_slot) +
                            " right now. Try again in a moment");
                    }
                    finish();
                }
                break;
            case recomp::save_states::LoadResult::Invalid:
                log_line("[load] slot %d: %s", request_slot, error.c_str());
                conker::notices::show("Couldn't load slot " + std::to_string(request_slot) + ": " + error);
                finish();
                break;
        }
    }

    // --- The keys ---

    void start(Request what) {
        request_slot = current_slot;
        requested_at = clock::now();
        attempts = 0;
        last_failure.clear();
        logged_not_now = false;
        request = what;
        ultramodern::save_states::set_safe_points_wanted(true);
    }

    void ask_to_save() {
        if (request.load() != Request::None) {
            return;
        }
        start(Request::Save);
    }

    void ask_to_load() {
        if (request.load() != Request::None) {
            return;
        }
        // The file is read here (once the safe point callback is done with any request, nothing else touches
        // these), then the load is asked for.
        const int slot = current_slot;
        std::string error;
        if (!read_state(slot, load_state, load_progress, load_version, error)) {
            conker::notices::show(error);
            return;
        }
        load_same_build = recomp::save_states::same_build(load_state);
        log_line("[load] slot %d read: from version %s, %s", slot, load_version.c_str(),
            load_same_build ? "this build" : "another build");
        start(Request::Load);
    }

    int SDLCALL watch_keys(void*, SDL_Event* event) {
        if (event->type != SDL_KEYDOWN || event->key.repeat != 0 || !ultramodern::is_game_started()) {
            return 0;
        }
        switch (event->key.keysym.sym) {
            case SDLK_F5:
                ask_to_save();
                break;
            case SDLK_F7:
                ask_to_load();
                break;
            case SDLK_F6:
                current_slot = current_slot % slot_count + 1;
                conker::notices::show("Save state slot " + std::to_string(current_slot.load()), true);
                break;
            default:
                break;
        }
        return 0;
    }
}

bool conker::save_states::runtime_quiet() {
    return ultramodern::save_states::is_quiet();
}

bool conker::save_states::net_capture(std::function<void(bool, std::vector<uint8_t>&&)> done) {
    auto job = std::make_unique<NetJob>();
    job->save = true;
    job->save_done = std::move(done);
    job->at = clock::now();
    {
        std::lock_guard lock{ net_mutex };
        if (net_job) {
            return false;
        }
        net_job = std::move(job);
        net_pending = true;
    }
    ultramodern::save_states::set_safe_points_wanted(true);
    return true;
}

bool conker::save_states::net_restore(std::vector<uint8_t>&& state, std::function<void(bool)> done) {
    auto job = std::make_unique<NetJob>();
    job->save = false;
    job->state = std::move(state);
    job->load_done = std::move(done);
    job->at = clock::now();
    {
        std::lock_guard lock{ net_mutex };
        if (net_job) {
            return false;
        }
        net_job = std::move(job);
        net_pending = true;
    }
    ultramodern::save_states::set_safe_points_wanted(true);
    return true;
}

void conker::save_states::init_headless() {
    ultramodern::save_states::set_safe_point_callback(on_safe_point);
}

void conker::save_states::init() {
    ultramodern::save_states::set_safe_point_callback(on_safe_point);
    // (0.11.4) No save state keys: F5, F6 and F7 do nothing, offline or online, so no one can save or load a state on demand. The
    // states themselves stay for the online session's own snapshots and resyncs (net_capture, net_restore).
    (void)watch_keys;
}
