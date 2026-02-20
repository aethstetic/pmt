#pragma once
#include "package.h"
#include "pacman_conf.h"
#include <alpm.h>
#include <string>
#include <vector>
#include <functional>

namespace pmt {

using ProgressCallback = std::function<void(const std::string& label, double fraction)>;
using EventCallback = std::function<void(const std::string& message)>;

class AlpmWrapper {
public:
    AlpmWrapper();
    ~AlpmWrapper();

    AlpmWrapper(const AlpmWrapper&) = delete;
    AlpmWrapper& operator=(const AlpmWrapper&) = delete;

    bool init(const PacmanConfig& config);
    bool reload();
    std::string last_error() const { return last_error_; }

    std::vector<PackageInfo> search(const std::string& query);
    std::vector<PackageInfo> list_installed();
    std::vector<PackageInfo> list_updates();
    bool install_package(const std::string& name);
    bool remove_package(const std::string& name);
    bool system_upgrade();
    bool sync_databases(bool force = false);
    std::vector<std::pair<std::string, std::string>> list_cached_versions(const std::string& name);
    bool downgrade_package(const std::string& filepath);

    void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }
    void set_event_callback(EventCallback cb) { event_cb_ = std::move(cb); }

    bool is_root() const { return is_root_; }
    void mark_installed(PackageInfo& info);
    std::vector<PackageInfo> list_foreign();
    bool is_dep_satisfied(const std::string& depstring);
    bool is_dep_in_repos(const std::string& depstring);

private:
    alpm_handle_t* handle_ = nullptr;
    std::string last_error_;
    bool is_root_ = false;
    ProgressCallback progress_cb_;
    EventCallback event_cb_;
    PacmanConfig saved_config_;

    PackageInfo pkg_to_info(alpm_pkg_t* pkg, const std::string& repo);
    static std::vector<std::string> list_to_strings(alpm_list_t* list);
    static std::vector<std::string> deplist_to_strings(alpm_list_t* list);

    static void progress_callback(void* ctx, alpm_progress_t type, const char* pkgname,
                                  int percent, size_t howmany, size_t current);
    static void download_callback(void* ctx, const char* filename,
                                  alpm_download_event_type_t event, void* data);
    static void event_callback(void* ctx, alpm_event_t* event);
    static void question_callback(void* ctx, alpm_question_t* question);
};

}
