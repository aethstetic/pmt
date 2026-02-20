#include "app.h"
#include "pacman_conf.h"
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace pmt {

volatile bool App::resize_pending_ = false;

void App::sigwinch_handler(int) {
    resize_pending_ = true;
}

App::App() : ui_(terminal_) {}

App::~App() {
    if (search_thread_.joinable()) search_thread_.join();
    if (aur_search_thread_.joinable()) aur_search_thread_.join();

    terminal_.show_cursor();
    terminal_.exit_alt_screen();
    terminal_.flush();
    terminal_.exit_raw_mode();
}

/* initializes alpm, terminal, and background AUR connection */
bool App::init() {
    ui_.color_disabled = color_disabled;
    if (!accent_hex.empty()) {
        const char* hex = accent_hex.c_str();
        if (hex[0] == '#') hex++;
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
            ui_.accent_code = Terminal::fg_rgb(r, g, b);
        }
    }

    PacmanConfig config;
    if (!config.parse()) {
        fprintf(stderr, "Error: Failed to parse /etc/pacman.conf\n");
        return false;
    }

    if (!alpm_.init(config)) {
        fprintf(stderr, "Error: %s\n", alpm_.last_error().c_str());
        return false;
    }

    std::thread([this]() { aur_.preconnect(); }).detach();

    alpm_.set_progress_callback([this](const std::string& label, double fraction) {
        ui_.progress.active = true;
        ui_.progress.label = label;
        ui_.progress.fraction = fraction;
        ui_.draw(packages_);
    });

    alpm_.set_event_callback([this](const std::string& msg) {
        set_status(msg);
        ui_.draw(packages_);
    });

    terminal_.enter_raw_mode();
    terminal_.enter_alt_screen();
    terminal_.hide_cursor();
    terminal_.flush();

    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, nullptr);

    refresh_packages();

    if (!alpm_.is_root()) {
        set_status("Running without root - install/remove/upgrade requires sudo");
    }

    return true;
}

/* main event loop */
void App::run() {
    while (running_) {
        if (resize_pending_) {
            resize_pending_ = false;
            terminal_.update_size();
            needs_redraw_ = true;
        }

        if (!ui_.status_message.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - status_set_time_).count();
            if (elapsed >= STATUS_TIMEOUT_MS) {
                ui_.status_message.clear();
                needs_redraw_ = true;
            }
        }

        poll_search_results();

        if (!pending_search_.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_search_time_).count();
            if (elapsed >= DEBOUNCE_MS) {
                std::string query = pending_search_;
                pending_search_.clear();
                start_search(query);
            }
        }

        if (needs_redraw_) {
            ui_.draw(packages_);
            needs_redraw_ = false;
        }

        auto ev = input_.read_key_timeout(16);
        if (ev.key != Key::None) {
            handle_key(ev);
            drain_input();
            needs_redraw_ = true;
        }
    }
}

void App::drain_input() {
    for (;;) {
        auto ev = input_.read_key_timeout(0);
        if (ev.key == Key::None) break;
        handle_key(ev);
    }
}

void App::start_search(const std::string& query) {
    if (query.empty()) {
        refresh_packages();
        return;
    }

    if (search_thread_.joinable()) search_thread_.join();
    if (aur_search_thread_.joinable()) aur_search_thread_.join();

    uint64_t gen = ++current_search_gen_;
    search_ready_ = false;
    bool want_aur = ui_.show_aur;

    set_status("Searching...");

    search_thread_ = std::thread([this, query, gen]() {
        auto results = alpm_.search(query);
        {
            std::lock_guard<std::mutex> lock(search_mutex_);
            search_results_buf_ = std::move(results);
        }
        search_gen_ = gen;
        search_ready_ = true;
    });

    if (want_aur) {
        uint64_t aur_gen = ++current_aur_gen_;
        aur_ready_ = false;
        aur_search_thread_ = std::thread([this, query, aur_gen]() {
            auto aur_results = aur_.search(query);
            {
                std::lock_guard<std::mutex> lock(search_mutex_);
                aur_results_buf_ = std::move(aur_results);
            }
            aur_search_gen_ = aur_gen;
            aur_ready_ = true;
        });
    }
}

void App::start_aur_search(const std::string& query) {
    if (aur_search_thread_.joinable()) aur_search_thread_.join();

    uint64_t gen = ++current_aur_gen_;
    aur_ready_ = false;

    set_status("Searching AUR...");

    aur_search_thread_ = std::thread([this, query, gen]() {
        auto results = aur_.search(query);
        {
            std::lock_guard<std::mutex> lock(search_mutex_);
            aur_results_buf_ = std::move(results);
        }
        aur_search_gen_ = gen;
        aur_ready_ = true;
    });
}

void App::poll_search_results() {
    if (search_ready_.load()) {
        search_ready_ = false;
        if (search_gen_.load() == current_search_gen_) {
            std::lock_guard<std::mutex> lock(search_mutex_);
            repo_results_ = std::move(search_results_buf_);
            if (!ui_.show_aur) {
                packages_ = repo_results_;
                ui_.selected = 0;
                ui_.list_scroll = 0;
                ui_.detail_scroll = 0;
                ui_.status_message.clear();
            }
            needs_redraw_ = true;
        }
    }

    if (aur_ready_.load()) {
        aur_ready_ = false;
        if (aur_search_gen_.load() == current_aur_gen_) {
            std::lock_guard<std::mutex> lock(search_mutex_);
            aur_results_ = std::move(aur_results_buf_);
            for (auto& pkg : aur_results_)
                alpm_.mark_installed(pkg);
            if (ui_.show_aur) {
                packages_ = aur_results_;
                ui_.selected = 0;
                ui_.list_scroll = 0;
                ui_.detail_scroll = 0;
                if (aur_results_.empty() && !aur_.last_error().empty()) {
                    ui_.status_message = "AUR: " + aur_.last_error();
                } else {
                    ui_.status_message.clear();
                }
            }
            needs_redraw_ = true;
        }
    }
}

void App::handle_key(const KeyEvent& ev) {
    if (ev.key == Key::CtrlC) {
        running_ = false;
        return;
    }

    switch (ui_.focus) {
        case Focus::SearchBar:
            handle_search_key(ev);
            break;
        case Focus::PackageList:
            handle_list_key(ev);
            break;
        case Focus::DetailPane:
            handle_detail_key(ev);
            break;
    }
}

void App::handle_search_key(const KeyEvent& ev) {
    switch (ev.key) {
        case Key::Escape:
            ui_.focus = Focus::PackageList;
            break;

        case Key::Enter:
            ui_.focus = Focus::PackageList;
            if (!ui_.search_text.empty()) {
                pending_search_.clear();
                start_search(ui_.search_text);
            }
            break;

        case Key::Backspace:
            if (ui_.search_cursor > 0 && !ui_.search_text.empty()) {
                ui_.search_text.erase(ui_.search_cursor - 1, 1);
                ui_.search_cursor--;
                pending_search_ = ui_.search_text;
                last_search_time_ = std::chrono::steady_clock::now();
            }
            if (ui_.search_text.empty()) {
                pending_search_.clear();
                refresh_packages();
            }
            break;

        case Key::Left:
            if (ui_.search_cursor > 0) ui_.search_cursor--;
            break;

        case Key::Right:
            if (ui_.search_cursor < static_cast<int>(ui_.search_text.size()))
                ui_.search_cursor++;
            break;

        case Key::Home:
            ui_.search_cursor = 0;
            break;

        case Key::End:
            ui_.search_cursor = static_cast<int>(ui_.search_text.size());
            break;

        case Key::Char:
            ui_.search_text.insert(ui_.search_cursor, 1, ev.ch);
            ui_.search_cursor++;
            pending_search_ = ui_.search_text;
            last_search_time_ = std::chrono::steady_clock::now();
            break;

        case Key::Tab:
            ui_.show_aur = !ui_.show_aur;
            if (!ui_.search_text.empty()) {
                if (ui_.show_aur) {
                    if (aur_results_.empty()) {
                        start_aur_search(ui_.search_text);
                    } else {
                        update_display_list();
                    }
                } else {
                    update_display_list();
                }
            }
            break;

        default:
            break;
    }
}

void App::handle_list_key(const KeyEvent& ev) {
    int total = static_cast<int>(packages_.size());

    switch (ev.key) {
        case Key::Char:
            switch (ev.ch) {
                case 'q':
                    running_ = false;
                    break;
                case '/':
                    ui_.focus = Focus::SearchBar;
                    break;
                case 'j':
                    if (ui_.selected < total - 1) {
                        ui_.selected++;
                        ui_.detail_scroll = 0;
                    }
                    break;
                case 'k':
                    if (ui_.selected > 0) {
                        ui_.selected--;
                        ui_.detail_scroll = 0;
                    }
                    break;
                case 'g':
                    ui_.selected = 0;
                    ui_.detail_scroll = 0;
                    break;
                case 'G':
                    ui_.selected = std::max(0, total - 1);
                    ui_.detail_scroll = 0;
                    break;
                case 'i':
                    do_install();
                    break;
                case 'r':
                    do_remove();
                    break;
                case 'd':
                    do_downgrade();
                    break;
                case 'u':
                    do_upgrade();
                    break;
                case 'S':
                    do_sync();
                    break;
                case 'I':
                    do_filter_installed();
                    break;
                case 'U':
                    do_filter_updates();
                    break;
                case 'a':
                case 'A':
                    do_aur_upgrade();
                    break;
                case 'c':
                    do_clear_cache();
                    break;
                default:
                    break;
            }
            break;

        case Key::Up:
            if (ui_.selected > 0) {
                ui_.selected--;
                ui_.detail_scroll = 0;
            }
            break;

        case Key::Down:
            if (ui_.selected < total - 1) {
                ui_.selected++;
                ui_.detail_scroll = 0;
            }
            break;

        case Key::PageUp:
            ui_.selected = std::max(0, ui_.selected - ui_.content_height());
            ui_.detail_scroll = 0;
            break;

        case Key::PageDown:
            ui_.selected = std::min(total - 1, ui_.selected + ui_.content_height());
            if (ui_.selected < 0) ui_.selected = 0;
            ui_.detail_scroll = 0;
            break;

        case Key::Home:
            ui_.selected = 0;
            ui_.detail_scroll = 0;
            break;

        case Key::End:
            ui_.selected = std::max(0, total - 1);
            ui_.detail_scroll = 0;
            break;

        case Key::Enter:
            ui_.focus = Focus::DetailPane;
            break;

        case Key::Tab:
            ui_.show_aur = !ui_.show_aur;
            ui_.filter_installed = false;
            ui_.filter_updates = false;
            if (!ui_.search_text.empty()) {
                if (ui_.show_aur && aur_results_.empty()) {
                    start_aur_search(ui_.search_text);
                } else {
                    update_display_list();
                }
            }
            break;

        case Key::CtrlL:
            needs_redraw_ = true;
            break;

        default:
            break;
    }

    ui_.ensure_visible();
}

void App::handle_detail_key(const KeyEvent& ev) {
    switch (ev.key) {
        case Key::Escape:
        case Key::Enter:
            ui_.focus = Focus::PackageList;
            break;

        case Key::Char:
            if (ev.ch == 'q') {
                ui_.focus = Focus::PackageList;
            } else if (ev.ch == 'j') {
                ui_.detail_scroll++;
            } else if (ev.ch == 'k') {
                if (ui_.detail_scroll > 0) ui_.detail_scroll--;
            } else if (ev.ch == 'g') {
                ui_.detail_scroll = 0;
            }
            break;

        case Key::Up:
            if (ui_.detail_scroll > 0) ui_.detail_scroll--;
            break;

        case Key::Down:
            ui_.detail_scroll++;
            break;

        case Key::PageUp:
            ui_.detail_scroll = std::max(0, ui_.detail_scroll - ui_.content_height());
            break;

        case Key::PageDown:
            ui_.detail_scroll += ui_.content_height();
            break;

        case Key::Home:
            ui_.detail_scroll = 0;
            break;

        default:
            break;
    }
}

void App::do_search_sync(const std::string& query) {
    if (query.empty()) {
        refresh_packages();
        return;
    }

    repo_results_ = alpm_.search(query);
    aur_results_.clear();

    if (ui_.show_aur) {
        aur_results_ = aur_.search(query);
        for (auto& pkg : aur_results_)
            alpm_.mark_installed(pkg);
    }

    update_display_list();
    ui_.selected = 0;
    ui_.list_scroll = 0;
    ui_.detail_scroll = 0;
    ui_.status_message.clear();
}

/* installs a repo or AUR package with dependency resolution */
void App::do_install() {
    if (packages_.empty()) return;
    const auto& pkg = packages_[ui_.selected];

    if (!alpm_.is_root()) {
        set_status("Root privileges required for install. Run with sudo.");
        return;
    }

    if (search_thread_.joinable()) search_thread_.join();

    bool ok;
    if (pkg.source == PackageSource::AUR) {
        set_status("Resolving AUR dependencies...");
        ui_.draw(packages_);

        DepResolver resolver(aur_, alpm_);
        std::vector<std::string> resolve_log;
        auto dep_result = resolver.resolve(pkg.name, [&](const std::string& msg) {
            resolve_log.push_back(msg);
        });

        if (!dep_result.ok) {
            set_status("Dependency resolution failed: " + dep_result.error);
            ui_.progress.active = false;
            return;
        }

        const auto& build_order = dep_result.aur_build_order;
        int total_builds = static_cast<int>(build_order.size());

        if (total_builds == 0) {
            set_status(pkg.name + " is already up to date");
            ui_.progress.active = false;
            return;
        }

        std::vector<std::string> confirm_lines;
        if (total_builds > 1) {
            confirm_lines.push_back("AUR packages to build (" + std::to_string(total_builds) + "):");
            for (const auto& p : build_order)
                confirm_lines.push_back("  " + p.name + " " + p.version);
        } else {
            confirm_lines.push_back("Package: aur/" + pkg.name + " " + pkg.version);
        }
        if (!dep_result.repo_deps.empty())
            confirm_lines.push_back("Repo deps (handled by makepkg): " + std::to_string(dep_result.repo_deps.size()));
        if (!dep_result.satisfied_deps.empty())
            confirm_lines.push_back("Already installed: " + std::to_string(dep_result.satisfied_deps.size()));

        if (!ui_.draw_confirm_dialog("Install AUR Package", confirm_lines)) {
            set_status("Install cancelled");
            ui_.progress.active = false;
            return;
        }

        ok = run_aur_builds(build_order, dep_result.repo_deps, pkg.name);
    } else {
        std::vector<std::string> lines;
        lines.push_back("Package: " + pkg.repo + "/" + pkg.name + " " + pkg.version);
        if (pkg.download_size > 0)
            lines.push_back("Download: " + format_size(pkg.download_size));
        if (pkg.install_size > 0)
            lines.push_back("Install size: " + format_size(pkg.install_size));

        if (!ui_.draw_confirm_dialog("Install Package", lines)) {
            set_status("Install cancelled");
            return;
        }

        set_status("Installing " + pkg.name + "...");
        ui_.progress.active = true;
        ui_.draw(packages_);

        ok = alpm_.install_package(pkg.name);
    }

    ui_.progress.active = false;
    if (ok) {
        set_status("Successfully installed " + pkg.name);
        if (!ui_.search_text.empty()) {
            do_search_sync(ui_.search_text);
        } else {
            refresh_packages();
        }
    } else {
        set_status("Install failed: " + alpm_.last_error());
    }
}

/* removes an installed package via alpm transaction */
void App::do_remove() {
    if (packages_.empty()) return;
    const auto& pkg = packages_[ui_.selected];

    if (!alpm_.is_root()) {
        set_status("Root privileges required for remove. Run with sudo.");
        return;
    }

    if (!pkg.installed) {
        set_status(pkg.name + " is not installed");
        return;
    }

    std::vector<std::string> lines;
    lines.push_back("Package: " + pkg.name + " " + (pkg.installed_version.empty() ? pkg.version : pkg.installed_version));

    if (!ui_.draw_confirm_dialog("Remove Package", lines)) {
        set_status("Remove cancelled");
        return;
    }

    if (search_thread_.joinable()) search_thread_.join();

    set_status("Removing " + pkg.name + "...");
    ui_.progress.active = true;
    ui_.draw(packages_);

    bool ok = alpm_.remove_package(pkg.name);
    ui_.progress.active = false;

    if (ok) {
        set_status("Successfully removed " + pkg.name);
        if (!ui_.search_text.empty()) {
            do_search_sync(ui_.search_text);
        } else {
            refresh_packages();
        }
    } else {
        set_status("Remove failed: " + alpm_.last_error());
    }
}

void App::do_downgrade() {
    if (packages_.empty()) return;
    const auto& pkg = packages_[ui_.selected];

    if (!pkg.installed) {
        set_status(pkg.name + " is not installed — nothing to downgrade");
        return;
    }

    if (!alpm_.is_root()) {
        set_status("Root privileges required for downgrade. Run with sudo.");
        return;
    }

    if (search_thread_.joinable()) search_thread_.join();

    auto cached = alpm_.list_cached_versions(pkg.name);
    if (cached.empty()) {
        set_status("No cached versions found for " + pkg.name);
        return;
    }

    std::string current_ver = pkg.installed_version.empty() ? pkg.version : pkg.installed_version;
    std::vector<std::string> options;
    for (const auto& [ver, path] : cached) {
        std::string label = ver;
        if (ver == current_ver) label += "  (current)";
        options.push_back(label);
    }

    int choice = ui_.draw_selection_dialog("Downgrade " + pkg.name, options);
    if (choice < 0 || choice >= static_cast<int>(cached.size())) {
        set_status("Downgrade cancelled");
        return;
    }

    const auto& [chosen_ver, chosen_path] = cached[choice];
    if (chosen_ver == current_ver) {
        set_status("Already at version " + current_ver);
        return;
    }

    std::vector<std::string> lines;
    lines.push_back("Package: " + pkg.name);
    lines.push_back("Current: " + current_ver);
    lines.push_back("Target:  " + chosen_ver);

    if (!ui_.draw_confirm_dialog("Downgrade Package", lines)) {
        set_status("Downgrade cancelled");
        return;
    }

    set_status("Downgrading " + pkg.name + " to " + chosen_ver + "...");
    ui_.progress.active = true;
    ui_.draw(packages_);

    bool ok = alpm_.downgrade_package(chosen_path);
    ui_.progress.active = false;

    if (ok) {
        set_status("Successfully downgraded " + pkg.name + " to " + chosen_ver);
        if (!ui_.search_text.empty()) {
            do_search_sync(ui_.search_text);
        } else {
            refresh_packages();
        }
    } else {
        set_status("Downgrade failed: " + alpm_.last_error());
    }
}

/* syncs databases and performs full system upgrade */
void App::do_upgrade() {
    if (!alpm_.is_root()) {
        set_status("Root privileges required for upgrade. Run with sudo.");
        return;
    }

    if (search_thread_.joinable()) search_thread_.join();

    set_status("Syncing databases...");
    ui_.draw(packages_);
    alpm_.sync_databases(false);
    ui_.progress.active = false;

    auto updates = alpm_.list_updates();
    if (updates.empty()) {
        set_status("System is up to date");
        return;
    }

    std::vector<std::string> lines;
    lines.push_back(std::to_string(updates.size()) + " package(s) to upgrade:");
    int shown = 0;
    for (const auto& upd : updates) {
        if (shown++ < 10) {
            lines.push_back("  " + upd.name + " " + upd.installed_version + " -> " + upd.version);
        }
    }
    if (static_cast<int>(updates.size()) > 10) {
        lines.push_back("  ... and " + std::to_string(updates.size() - 10) + " more");
    }

    if (!ui_.draw_confirm_dialog("System Upgrade", lines)) {
        set_status("Upgrade cancelled");
        return;
    }

    set_status("Upgrading system...");
    ui_.progress.active = true;
    ui_.draw(packages_);

    bool ok = alpm_.system_upgrade();
    ui_.progress.active = false;

    if (ok) {
        set_status("System upgrade complete");
        refresh_packages();
    } else {
        set_status("Upgrade failed: " + alpm_.last_error());
    }
}

void App::do_sync() {
    if (!alpm_.is_root()) {
        set_status("Root privileges required for sync. Run with sudo.");
        return;
    }

    if (search_thread_.joinable()) search_thread_.join();

    set_status("Syncing databases...");
    ui_.draw(packages_);

    bool ok = alpm_.sync_databases(true);
    ui_.progress.active = false;
    if (ok) {
        set_status("Database sync complete");
    } else {
        set_status("Sync failed: " + alpm_.last_error());
    }
}

void App::do_filter_installed() {
    ui_.filter_installed = !ui_.filter_installed;
    ui_.filter_updates = false;
    ui_.show_aur = false;

    if (ui_.filter_installed) {
        packages_ = alpm_.list_installed();
        std::sort(packages_.begin(), packages_.end(),
                  [](const PackageInfo& a, const PackageInfo& b) { return a.name < b.name; });
    } else {
        if (!ui_.search_text.empty()) {
            start_search(ui_.search_text);
        } else {
            refresh_packages();
        }
    }

    ui_.selected = 0;
    ui_.list_scroll = 0;
    ui_.detail_scroll = 0;
}

void App::do_filter_updates() {
    ui_.filter_updates = !ui_.filter_updates;
    ui_.filter_installed = false;
    ui_.show_aur = false;

    if (ui_.filter_updates) {
        packages_ = alpm_.list_updates();
        if (packages_.empty()) {
            set_status("No updates available");
        }
    } else {
        if (!ui_.search_text.empty()) {
            start_search(ui_.search_text);
        } else {
            refresh_packages();
        }
    }

    ui_.selected = 0;
    ui_.list_scroll = 0;
    ui_.detail_scroll = 0;
}

/* clears build cache, reviewed PKGBUILDs, and/or temp logs */
void App::do_clear_cache() {
    namespace fs = std::filesystem;

    std::string cache_dir = AurClient::default_cache_dir();
    std::string reviewed_dir = AurClient::reviewed_cache_dir();
    std::vector<std::string> temp_logs = {
        "/tmp/pmt_build.log",
        "/tmp/pmt_aur_debug.log",
        "/tmp/pmt_vcs_check.log",
    };

    std::vector<std::string> options = {
        "Clean build cache (keep 2 latest)",
        "Clear reviewed PKGBUILDs",
        "Clear temp logs",
        "Clear ALL (remove everything)",
    };

    int choice = ui_.draw_selection_dialog("Clear Cache", options);
    if (choice < 0) {
        set_status("Cache clear cancelled");
        return;
    }

    std::error_code ec;
    uintmax_t total_freed = 0;
    int files_removed = 0;

    if (choice == 0) {
        if (!fs::exists(cache_dir, ec) || !fs::is_directory(cache_dir, ec)) {
            set_status("Nothing to clear — build cache is empty");
            return;
        }

        struct PkgFile {
            fs::path path;
            fs::file_time_type mtime;
            uintmax_t size;
        };

        std::vector<PkgFile> to_delete;

        for (auto& pkg_entry : fs::directory_iterator(cache_dir, ec)) {
            if (!pkg_entry.is_directory(ec)) continue;

            std::vector<PkgFile> pkg_files;
            for (auto& f : fs::directory_iterator(pkg_entry.path(), ec)) {
                if (!f.is_regular_file(ec)) continue;
                std::string name = f.path().filename().string();
                if (name.find(".pkg.tar") == std::string::npos) continue;
                pkg_files.push_back({f.path(), f.last_write_time(ec), f.file_size(ec)});
            }

            if (pkg_files.size() <= 2) continue;

            std::sort(pkg_files.begin(), pkg_files.end(),
                      [](const PkgFile& a, const PkgFile& b) { return a.mtime > b.mtime; });

            for (size_t i = 2; i < pkg_files.size(); i++) {
                total_freed += pkg_files[i].size;
                to_delete.push_back(pkg_files[i]);
            }
        }

        if (to_delete.empty()) {
            set_status("Nothing to clear — build cache is already clean");
            return;
        }

        std::vector<std::string> confirm_lines;
        confirm_lines.push_back("Files to remove: " + std::to_string(to_delete.size()));
        confirm_lines.push_back("Space to free: " + format_size(static_cast<int64_t>(total_freed)));
        confirm_lines.push_back("Keeps 2 newest packages per directory");

        if (!ui_.draw_confirm_dialog("Clean Build Cache", confirm_lines)) {
            set_status("Cache clear cancelled");
            return;
        }

        for (const auto& f : to_delete) {
            if (fs::remove(f.path, ec))
                files_removed++;
        }

        set_status("Cleared " + std::to_string(files_removed) + " files (" + format_size(static_cast<int64_t>(total_freed)) + " freed)");

    } else if (choice == 1) {
        if (!fs::exists(reviewed_dir, ec) || fs::is_empty(reviewed_dir, ec)) {
            set_status("Nothing to clear — no reviewed PKGBUILDs");
            return;
        }

        for (auto& entry : fs::recursive_directory_iterator(reviewed_dir, ec)) {
            if (entry.is_regular_file(ec))
                total_freed += entry.file_size(ec);
        }

        std::vector<std::string> confirm_lines;
        confirm_lines.push_back("Remove all reviewed PKGBUILDs");
        confirm_lines.push_back("Space to free: " + format_size(static_cast<int64_t>(total_freed)));

        if (!ui_.draw_confirm_dialog("Clear Reviewed PKGBUILDs", confirm_lines)) {
            set_status("Cache clear cancelled");
            return;
        }

        fs::remove_all(reviewed_dir, ec);
        set_status("Cleared reviewed PKGBUILDs (" + format_size(static_cast<int64_t>(total_freed)) + " freed)");

    } else if (choice == 2) {
        for (const auto& log : temp_logs) {
            if (fs::exists(log, ec)) {
                total_freed += fs::file_size(log, ec);
                if (fs::remove(log, ec))
                    files_removed++;
            }
        }

        if (files_removed == 0) {
            set_status("Nothing to clear — no temp logs");
            return;
        }

        set_status("Cleared " + std::to_string(files_removed) + " log file(s) (" + format_size(static_cast<int64_t>(total_freed)) + " freed)");

    } else if (choice == 3) {
        uintmax_t cache_size = 0, reviewed_size = 0, logs_size = 0;

        if (fs::exists(cache_dir, ec)) {
            for (auto& entry : fs::recursive_directory_iterator(cache_dir, ec)) {
                if (entry.is_regular_file(ec))
                    cache_size += entry.file_size(ec);
            }
        }
        if (fs::exists(reviewed_dir, ec)) {
            for (auto& entry : fs::recursive_directory_iterator(reviewed_dir, ec)) {
                if (entry.is_regular_file(ec))
                    reviewed_size += entry.file_size(ec);
            }
        }
        for (const auto& log : temp_logs) {
            if (fs::exists(log, ec))
                logs_size += fs::file_size(log, ec);
        }

        total_freed = cache_size + reviewed_size + logs_size;

        if (total_freed == 0) {
            set_status("Nothing to clear — everything is already clean");
            return;
        }

        std::vector<std::string> confirm_lines;
        confirm_lines.push_back("Build cache:  " + format_size(static_cast<int64_t>(cache_size)));
        confirm_lines.push_back("Reviewed:     " + format_size(static_cast<int64_t>(reviewed_size)));
        confirm_lines.push_back("Temp logs:    " + format_size(static_cast<int64_t>(logs_size)));
        confirm_lines.push_back("Total:        " + format_size(static_cast<int64_t>(total_freed)));

        if (!ui_.draw_confirm_dialog("Clear ALL Cache", confirm_lines)) {
            set_status("Cache clear cancelled");
            return;
        }

        fs::remove_all(cache_dir, ec);
        fs::remove_all(reviewed_dir, ec);
        for (const auto& log : temp_logs)
            fs::remove(log, ec);

        set_status("Cleared all cache (" + format_size(static_cast<int64_t>(total_freed)) + " freed)");
    }
}

/* shared AUR build pipeline: PKGBUILD review, dep install, build loop */
bool App::run_aur_builds(const std::vector<PackageInfo>& build_order,
                         const std::vector<std::string>& repo_deps,
                         const std::string& summary_name) {
    int total_builds = static_cast<int>(build_order.size());

    namespace fs = std::filesystem;
    std::string reviewed_dir = AurClient::reviewed_cache_dir();

    for (const auto& p : build_order) {
        set_status("Fetching PKGBUILD for " + p.name + "...");
        ui_.draw(packages_);

        std::string pkgbuild = aur_.fetch_pkgbuild(p.name, p.pkgbase);
        if (pkgbuild.empty()) {
            set_status("Failed to fetch PKGBUILD for " + p.name);
            ui_.progress.active = false;
            return false;
        }

        std::string base = (!p.pkgbase.empty() && p.pkgbase != p.name) ? p.pkgbase : p.name;
        std::string reviewed_path = reviewed_dir + "/" + base + "/PKGBUILD";
        std::string old_pkgbuild;
        {
            std::ifstream f(reviewed_path);
            if (f) {
                old_pkgbuild = std::string((std::istreambuf_iterator<char>(f)),
                                            std::istreambuf_iterator<char>());
            }
        }
        if (old_pkgbuild == pkgbuild)
            old_pkgbuild.clear();

        if (!ui_.draw_pkgbuild_review(p.name, pkgbuild, old_pkgbuild)) {
            set_status("Build cancelled (PKGBUILD rejected for " + p.name + ")");
            ui_.progress.active = false;
            return false;
        }

        try {
            fs::create_directories(reviewed_dir + "/" + base);
            std::ofstream out(reviewed_path);
            if (out) out << pkgbuild;

            const char* sudo_user = getenv("SUDO_USER");
            if (geteuid() == 0 && sudo_user && sudo_user[0]) {
                std::string cmd = "chown -R '" + std::string(sudo_user) + "' '"
                                  + reviewed_dir + "/" + base + "'";
                system(cmd.c_str());
            }
        } catch (...) {
        }
    }

    std::string log_file = "/tmp/pmt_build.log";
    { FILE* f = fopen(log_file.c_str(), "w"); if (f) fclose(f); }

    FILE* log_fp = fopen(log_file.c_str(), "r");
    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::string> log_lines;

    auto tail_log = [&]() {
        if (!log_fp) return;
        clearerr(log_fp);
        char buf[4096];
        while (fgets(buf, sizeof(buf), log_fp)) {
            std::string line(buf);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            log_lines.push_back(std::move(line));
        }
    };

    auto elapsed = [&]() -> int {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count());
    };

    auto run_tailed = [&](const std::string& cmd, const std::string& title) -> int {
        std::atomic<bool> done{false};
        int rc = 0;
        std::thread t([&]() {
            rc = system(cmd.c_str());
            done = true;
        });
        while (!done.load()) {
            tail_log();
            ui_.draw_build_log(title, log_lines, false, elapsed());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        t.join();
        tail_log();
        return rc;
    };

    if (!repo_deps.empty()) {
        log_lines.push_back("=== Installing repo dependencies ===");

        std::string dep_list;
        for (const auto& d : repo_deps) {
            if (!dep_list.empty()) dep_list += " ";
            dep_list += d;
        }
        std::string cmd = "pacman -S --needed --noconfirm --asdeps " + dep_list +
                          " >> '" + log_file + "' 2>&1";
        int rc = run_tailed(cmd, "Installing dependencies");
        if (rc != 0) {
            log_lines.push_back("FAILED to install repo dependencies");
            ui_.draw_build_log("Installing dependencies - FAILED", log_lines, true, elapsed());
            drain_input();
            for (;;) {
                auto ev = input_.read_key_timeout(100);
                if (ev.key != Key::None) break;
            }
            if (log_fp) fclose(log_fp);
            ui_.progress.active = false;
            return false;
        }
    }

    for (int idx = 0; idx < total_builds; idx++) {
        const auto& p = build_order[idx];

        std::string title = "Building " + p.name;
        if (total_builds > 1)
            title += " [" + std::to_string(idx + 1) + "/" + std::to_string(total_builds) + "]";

        log_lines.push_back("");
        log_lines.push_back("=== Building " + p.name + " ===");

        std::atomic<bool> build_done{false};
        std::string built_path;
        std::thread build_thread([&]() {
            built_path = aur_.build_package(p.name, log_file, "", p.pkgbase);
            build_done = true;
        });

        while (!build_done.load()) {
            tail_log();
            ui_.draw_build_log(title, log_lines, false, elapsed());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        build_thread.join();
        tail_log();

        if (built_path.empty()) {
            log_lines.push_back("");
            log_lines.push_back("BUILD FAILED for " + p.name);
            ui_.draw_build_log(title + " - FAILED", log_lines, true, elapsed());
            drain_input();
            for (;;) {
                auto ev = input_.read_key_timeout(100);
                if (ev.key != Key::None) break;
            }
            if (log_fp) fclose(log_fp);
            return false;
        }

        log_lines.push_back("");
        log_lines.push_back("=== Installing " + p.name + " ===");

        std::string pkg_dir = built_path.substr(0, built_path.rfind('/'));
        std::string install_cmd = "pacman -U --noconfirm --overwrite '*' " +
                                  pkg_dir + "/*.pkg.tar* >> '" + log_file + "' 2>&1";
        int rc = run_tailed(install_cmd, title + " (installing)");
        if (rc != 0) {
            log_lines.push_back("INSTALL FAILED for " + p.name);
            ui_.draw_build_log(title + " - FAILED", log_lines, true, elapsed());
            drain_input();
            for (;;) {
                auto ev = input_.read_key_timeout(100);
                if (ev.key != Key::None) break;
            }
            if (log_fp) fclose(log_fp);
            return false;
        }

        alpm_.reload();
    }

    if (log_fp) fclose(log_fp);

    terminal_.enter_raw_mode();
    terminal_.hide_cursor();

    log_lines.push_back("");
    log_lines.push_back("=== Build complete ===");
    std::string success_msg = "Successfully built and installed " + summary_name;
    if (total_builds > 1)
        success_msg += " (" + std::to_string(total_builds) + " AUR packages)";
    log_lines.push_back(success_msg);
    log_lines.push_back("");
    log_lines.push_back("Press any key to continue...");

    std::string final_title = "Build complete: " + summary_name;
    ui_.draw_build_log(final_title, log_lines, true, elapsed());

    drain_input();
    for (;;) {
        auto ev = input_.read_key_timeout(100);
        if (ev.key != Key::None) break;
    }

    return true;
}

/* checks all foreign packages against AUR for updates including VCS */
void App::do_aur_upgrade() {
    FILE* dbg = fopen("/tmp/pmt_aur_debug.log", "w");
    auto dbglog = [&](const char* msg) {
        if (dbg) { fprintf(dbg, "%s\n", msg); fflush(dbg); }
    };

    dbglog("do_aur_upgrade called");

    if (!alpm_.is_root()) {
        dbglog("not root, aborting");
        if (dbg) fclose(dbg);
        ui_.draw_message("AUR Upgrade", "Root privileges required. Run with sudo.");
        return;
    }

    dbglog("is root, joining search threads...");

    if (search_thread_.joinable()) search_thread_.join();
    dbglog("search_thread joined");
    if (aur_search_thread_.joinable()) aur_search_thread_.join();
    dbglog("aur_search_thread joined");

    set_status("Checking foreign packages...");
    ui_.draw(packages_);

    dbglog("calling list_foreign...");
    auto foreign = alpm_.list_foreign();
    fprintf(dbg, "list_foreign returned %zu packages\n", foreign.size());
    fflush(dbg);

    if (foreign.empty()) {
        dbglog("no foreign packages");
        if (dbg) fclose(dbg);
        ui_.draw_message("AUR Upgrade", "No foreign (AUR) packages installed");
        return;
    }

    set_status("Querying AUR for " + std::to_string(foreign.size()) + " packages...");
    ui_.draw(packages_);

    std::vector<std::string> names;
    for (const auto& pkg : foreign) {
        names.push_back(pkg.name);
        fprintf(dbg, "  foreign: %s %s\n", pkg.name.c_str(), pkg.version.c_str());
    }
    fflush(dbg);

    dbglog("calling info_batch...");
    auto aur_info = aur_.info_batch(names);
    fprintf(dbg, "info_batch returned %zu packages\n", aur_info.size());
    fflush(dbg);

    std::map<std::string, PackageInfo> aur_map;
    for (auto& p : aur_info)
        aur_map[p.name] = std::move(p);

    std::vector<std::pair<PackageInfo, PackageInfo>> upgrades;
    for (const auto& local : foreign) {
        auto it = aur_map.find(local.name);
        if (it == aur_map.end()) {
            fprintf(dbg, "  skip %s (not in AUR)\n", local.name.c_str());
            continue;
        }

        const auto& aur_pkg = it->second;
        int cmp = alpm_pkg_vercmp(aur_pkg.version.c_str(), local.version.c_str());
        fprintf(dbg, "  %s: local=%s aur=%s cmp=%d\n",
                local.name.c_str(), local.version.c_str(),
                aur_pkg.version.c_str(), cmp);
        if (cmp > 0) {
            upgrades.push_back({local, aur_pkg});
        }
    }
    fflush(dbg);

    fprintf(dbg, "upgrades found: %zu\n", upgrades.size());
    fflush(dbg);

    std::vector<std::pair<PackageInfo, PackageInfo>> vcs_candidates;
    for (const auto& local : foreign) {
        if (!AurClient::is_vcs_package(local.name)) continue;
        auto it = aur_map.find(local.name);
        if (it == aur_map.end()) continue;
        const auto& aur_pkg = it->second;
        int cmp = alpm_pkg_vercmp(aur_pkg.version.c_str(), local.version.c_str());
        if (cmp <= 0) {
            vcs_candidates.push_back({local, aur_pkg});
            fprintf(dbg, "  vcs candidate: %s (local=%s aur=%s)\n",
                    local.name.c_str(), local.version.c_str(), aur_pkg.version.c_str());
        }
    }
    fflush(dbg);

    if (!vcs_candidates.empty()) {
        fprintf(dbg, "checking %zu VCS package(s)...\n", vcs_candidates.size());
        fflush(dbg);

        std::string vcs_log_file = "/tmp/pmt_vcs_check.log";
        { FILE* f = fopen(vcs_log_file.c_str(), "w"); if (f) fclose(f); }

        FILE* vcs_log_fp = fopen(vcs_log_file.c_str(), "r");
        auto vcs_start_time = std::chrono::steady_clock::now();
        std::vector<std::string> vcs_log_lines;
        vcs_log_lines.push_back("Checking " + std::to_string(vcs_candidates.size()) + " VCS package(s)...");

        auto vcs_tail_log = [&]() {
            if (!vcs_log_fp) return;
            clearerr(vcs_log_fp);
            char buf[4096];
            while (fgets(buf, sizeof(buf), vcs_log_fp)) {
                std::string line(buf);
                if (!line.empty() && line.back() == '\n') line.pop_back();
                vcs_log_lines.push_back(std::move(line));
            }
        };

        auto vcs_elapsed = [&]() -> int {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - vcs_start_time).count());
        };

        for (size_t vi = 0; vi < vcs_candidates.size(); vi++) {
            const auto& [local, aur_pkg] = vcs_candidates[vi];
            std::string vcs_title = "Checking VCS: " + local.name
                + " [" + std::to_string(vi + 1) + "/" + std::to_string(vcs_candidates.size()) + "]";

            vcs_log_lines.push_back("");
            vcs_log_lines.push_back("=== Checking " + local.name + " ===");

            std::atomic<bool> vcs_done{false};
            std::string real_version;
            std::string pkg_name = local.name;
            std::string pkgbase = aur_pkg.pkgbase;
            std::thread vcs_thread([&, pkg_name, pkgbase]() {
                real_version = aur_.check_vcs_version(pkg_name, pkgbase, vcs_log_file);
                vcs_done = true;
            });

            while (!vcs_done.load()) {
                vcs_tail_log();
                ui_.draw_build_log(vcs_title, vcs_log_lines, false, vcs_elapsed());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            vcs_thread.join();
            vcs_tail_log();

            if (real_version.empty()) {
                vcs_log_lines.push_back(local.name + ": skipped (check failed)");
                fprintf(dbg, "  vcs %s: check failed, skipping\n", local.name.c_str());
                continue;
            }

            int cmp = alpm_pkg_vercmp(real_version.c_str(), local.version.c_str());
            fprintf(dbg, "  vcs %s: local=%s real=%s cmp=%d\n",
                    local.name.c_str(), local.version.c_str(), real_version.c_str(), cmp);
            if (cmp > 0) {
                vcs_log_lines.push_back(local.name + ": " + local.version + " -> " + real_version + " (UPDATE AVAILABLE)");
                PackageInfo real_aur = aur_pkg;
                real_aur.version = real_version;
                upgrades.push_back({local, real_aur});
            } else {
                vcs_log_lines.push_back(local.name + ": up to date (" + real_version + ")");
            }
        }

        if (vcs_log_fp) fclose(vcs_log_fp);

        terminal_.enter_raw_mode();
        terminal_.hide_cursor();
    }

    fflush(dbg);
    fprintf(dbg, "total upgrades after VCS check: %zu\n", upgrades.size());
    fflush(dbg);

    if (upgrades.empty()) {
        dbglog("no upgrades available");
        if (dbg) fclose(dbg);
        ui_.draw_message("AUR Upgrade",
            "All " + std::to_string(foreign.size()) + " AUR packages are up to date");
        return;
    }

    if (dbg) fclose(dbg);

    std::vector<std::string> confirm_lines;
    confirm_lines.push_back(std::to_string(upgrades.size()) + " AUR package(s) to upgrade:");
    int shown = 0;
    for (const auto& [local, aur_pkg] : upgrades) {
        if (shown++ < 15) {
            confirm_lines.push_back("  " + local.name + " " + local.version + " -> " + aur_pkg.version);
        }
    }
    if (static_cast<int>(upgrades.size()) > 15)
        confirm_lines.push_back("  ... and " + std::to_string(upgrades.size() - 15) + " more");

    if (!ui_.draw_confirm_dialog("AUR Upgrade", confirm_lines)) {
        set_status("AUR upgrade cancelled");
        return;
    }

    set_status("Resolving dependencies...");
    ui_.progress.active = true;
    ui_.draw(packages_);

    DepResolver resolver(aur_, alpm_);
    std::vector<PackageInfo> merged_build_order;
    std::vector<std::string> merged_repo_deps;
    std::set<std::string> seen_bases;

    for (const auto& [local, aur_pkg] : upgrades) {
        auto dep_result = resolver.resolve(aur_pkg.name, [&](const std::string& msg) {
            set_status(msg);
            ui_.draw(packages_);
        });

        if (!dep_result.ok) {
            set_status("Dependency resolution failed for " + aur_pkg.name + ": " + dep_result.error);
            ui_.progress.active = false;
            return;
        }

        for (auto& p : dep_result.aur_build_order) {
            std::string base = p.pkgbase.empty() ? p.name : p.pkgbase;
            if (seen_bases.insert(base).second) {
                merged_build_order.push_back(std::move(p));
            }
        }

        merged_repo_deps.insert(merged_repo_deps.end(),
                                dep_result.repo_deps.begin(),
                                dep_result.repo_deps.end());
    }

    std::sort(merged_repo_deps.begin(), merged_repo_deps.end());
    merged_repo_deps.erase(
        std::unique(merged_repo_deps.begin(), merged_repo_deps.end()),
        merged_repo_deps.end());

    if (merged_build_order.empty()) {
        set_status("All AUR packages are already up to date");
        ui_.progress.active = false;
        return;
    }

    bool ok = run_aur_builds(merged_build_order, merged_repo_deps,
                             std::to_string(upgrades.size()) + " AUR packages");

    ui_.progress.active = false;
    if (ok) {
        set_status("AUR upgrade complete");
        if (!ui_.search_text.empty()) {
            do_search_sync(ui_.search_text);
        } else {
            refresh_packages();
        }
    } else {
        set_status("AUR upgrade failed");
    }
}

void App::refresh_packages() {
    packages_ = alpm_.list_installed();
    std::sort(packages_.begin(), packages_.end(),
              [](const PackageInfo& a, const PackageInfo& b) { return a.name < b.name; });
    repo_results_.clear();
    aur_results_.clear();
}

void App::update_display_list() {
    if (ui_.show_aur) {
        packages_ = aur_results_;
    } else {
        packages_ = repo_results_;
    }
    ui_.selected = 0;
    ui_.list_scroll = 0;
    ui_.detail_scroll = 0;
}

void App::set_status(const std::string& msg) {
    ui_.status_message = msg;
    status_set_time_ = std::chrono::steady_clock::now();
    needs_redraw_ = true;
}

}
