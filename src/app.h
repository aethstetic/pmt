#pragma once
#include "terminal.h"
#include "input.h"
#include "ui.h"
#include "alpm_wrapper.h"
#include "aur.h"
#include "dep_resolver.h"
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>

namespace pmt {

class App {
public:
    App();
    ~App();

    bool color_disabled = false;
    std::string accent_hex;

    bool init();
    void run();

private:
    Terminal terminal_;
    Input input_;
    UI ui_;
    AlpmWrapper alpm_;
    AurClient aur_;

    std::vector<PackageInfo> packages_;
    std::vector<PackageInfo> repo_results_;
    std::vector<PackageInfo> aur_results_;

    bool running_ = true;
    bool needs_redraw_ = true;
    std::string pending_search_;
    std::chrono::steady_clock::time_point last_search_time_;
    std::chrono::steady_clock::time_point status_set_time_;
    static constexpr int DEBOUNCE_MS = 150;
    static constexpr int STATUS_TIMEOUT_MS = 3000;

    std::thread search_thread_;
    std::thread aur_search_thread_;
    std::mutex search_mutex_;
    std::vector<PackageInfo> search_results_buf_;
    std::vector<PackageInfo> aur_results_buf_;
    std::atomic<bool> search_ready_{false};
    std::atomic<bool> aur_ready_{false};
    std::atomic<uint64_t> search_gen_{0};
    std::atomic<uint64_t> aur_search_gen_{0};
    uint64_t current_search_gen_ = 0;
    uint64_t current_aur_gen_ = 0;
    void start_search(const std::string& query);
    void start_aur_search(const std::string& query);
    void poll_search_results();

    void handle_key(const KeyEvent& ev);
    void handle_search_key(const KeyEvent& ev);
    void handle_list_key(const KeyEvent& ev);
    void handle_detail_key(const KeyEvent& ev);

    void do_search_sync(const std::string& query);
    void do_install();
    void do_remove();
    void do_upgrade();
    void do_aur_upgrade();
    void do_sync();
    void do_downgrade();
    void do_filter_installed();
    void do_filter_updates();
    void do_clear_cache();

    bool run_aur_builds(const std::vector<PackageInfo>& build_order,
                        const std::vector<std::string>& repo_deps,
                        const std::string& summary_name);

    void refresh_packages();
    void update_display_list();
    void apply_sort();
    void set_status(const std::string& msg);
    void drain_input();

    static void sigwinch_handler(int sig);
    static volatile bool resize_pending_;
};

}
