#include "alpm_wrapper.h"
#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <algorithm>

namespace pmt {

AlpmWrapper::AlpmWrapper() {
    is_root_ = (geteuid() == 0);
}

AlpmWrapper::~AlpmWrapper() {
    if (handle_) {
        alpm_release(handle_);
        handle_ = nullptr;
    }
}

/* initializes libalpm handle with pacman.conf settings */
bool AlpmWrapper::init(const PacmanConfig& config) {
    if (handle_) {
        alpm_release(handle_);
        handle_ = nullptr;
    }

    saved_config_ = config;

    alpm_errno_t err;
    handle_ = alpm_initialize(config.root_dir.c_str(), config.db_path.c_str(), &err);
    if (!handle_) {
        last_error_ = "Failed to initialize alpm: " + std::string(alpm_strerror(err));
        return false;
    }

    alpm_option_set_logfile(handle_, config.log_file.c_str());
    alpm_option_set_gpgdir(handle_, config.gpg_dir.c_str());

    alpm_option_set_progresscb(handle_, progress_callback, this);
    alpm_option_set_dlcb(handle_, download_callback, this);
    alpm_option_set_eventcb(handle_, event_callback, this);
    alpm_option_set_questioncb(handle_, question_callback, this);

    for (const auto& repo : config.repos) {
        int level = repo.siglevel >= 0 ? repo.siglevel : config.siglevel;
        alpm_db_t* db = alpm_register_syncdb(handle_, repo.name.c_str(), level);
        if (!db) {
            last_error_ = "Failed to register db: " + repo.name;
            continue;
        }

        for (const auto& server_template : repo.servers) {
            std::string url = server_template;
            size_t pos;
            while ((pos = url.find("$repo")) != std::string::npos)
                url.replace(pos, 5, repo.name);
            while ((pos = url.find("$arch")) != std::string::npos)
                url.replace(pos, 5, config.architecture);
            alpm_db_add_server(db, url.c_str());
        }
    }

    return true;
}

bool AlpmWrapper::reload() {
    return init(saved_config_);
}

std::vector<PackageInfo> AlpmWrapper::search(const std::string& query) {
    std::vector<PackageInfo> results;
    if (!handle_ || query.empty()) return results;

    alpm_list_t* needles = nullptr;
    needles = alpm_list_add(needles, const_cast<char*>(query.c_str()));

    alpm_list_t* syncdbs = alpm_get_syncdbs(handle_);
    for (alpm_list_t* i = syncdbs; i; i = alpm_list_next(i)) {
        alpm_db_t* db = static_cast<alpm_db_t*>(i->data);
        alpm_list_t* ret = nullptr;
        if (alpm_db_search(db, needles, &ret) == 0) {
            for (alpm_list_t* j = ret; j; j = alpm_list_next(j)) {
                alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(j->data);
                auto info = pkg_to_info(pkg, alpm_db_get_name(db));
                mark_installed(info);
                results.push_back(std::move(info));
            }
            alpm_list_free(ret);
        }
    }

    alpm_list_free(needles);
    return results;
}

std::vector<PackageInfo> AlpmWrapper::list_installed() {
    std::vector<PackageInfo> results;
    if (!handle_) return results;

    alpm_db_t* localdb = alpm_get_localdb(handle_);
    alpm_list_t* pkgs = alpm_db_get_pkgcache(localdb);

    for (alpm_list_t* i = pkgs; i; i = alpm_list_next(i)) {
        alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(i->data);
        auto info = pkg_to_info(pkg, "local");
        info.installed = true;
        info.installed_version = info.version;
        info.source = PackageSource::Local;
        results.push_back(std::move(info));
    }

    return results;
}

std::vector<PackageInfo> AlpmWrapper::list_updates() {
    std::vector<PackageInfo> results;
    if (!handle_) return results;

    alpm_db_t* localdb = alpm_get_localdb(handle_);
    alpm_list_t* syncdbs = alpm_get_syncdbs(handle_);
    alpm_list_t* pkgs = alpm_db_get_pkgcache(localdb);

    for (alpm_list_t* i = pkgs; i; i = alpm_list_next(i)) {
        alpm_pkg_t* local_pkg = static_cast<alpm_pkg_t*>(i->data);
        alpm_pkg_t* new_pkg = alpm_sync_get_new_version(local_pkg, syncdbs);
        if (new_pkg) {
            alpm_db_t* db = alpm_pkg_get_db(new_pkg);
            auto info = pkg_to_info(new_pkg, db ? alpm_db_get_name(db) : "unknown");
            info.installed = true;
            info.installed_version = alpm_pkg_get_version(local_pkg);
            info.has_update = true;
            results.push_back(std::move(info));
        }
    }

    return results;
}

/* installs a sync repo package via alpm transaction */
bool AlpmWrapper::install_package(const std::string& name) {
    if (!handle_) { last_error_ = "Not initialized"; return false; }
    if (!is_root_) { last_error_ = "Root privileges required"; return false; }

    alpm_pkg_t* pkg = nullptr;
    alpm_list_t* syncdbs = alpm_get_syncdbs(handle_);
    for (alpm_list_t* i = syncdbs; i; i = alpm_list_next(i)) {
        alpm_db_t* db = static_cast<alpm_db_t*>(i->data);
        pkg = alpm_db_get_pkg(db, name.c_str());
        if (pkg) break;
    }

    if (!pkg) {
        last_error_ = "Package not found: " + name;
        return false;
    }

    int ret = alpm_trans_init(handle_, ALPM_TRANS_FLAG_NEEDED);
    if (ret != 0) {
        last_error_ = "Failed to init transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        return false;
    }

    ret = alpm_add_pkg(handle_, pkg);
    if (ret != 0) {
        last_error_ = "Failed to add package: " + std::string(alpm_strerror(alpm_errno(handle_)));
        alpm_trans_release(handle_);
        return false;
    }

    alpm_list_t* data = nullptr;
    ret = alpm_trans_prepare(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to prepare transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    ret = alpm_trans_commit(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to commit transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    FREELIST(data);
    alpm_trans_release(handle_);
    return true;
}

/* removes an installed package via alpm transaction */
bool AlpmWrapper::remove_package(const std::string& name) {
    if (!handle_) { last_error_ = "Not initialized"; return false; }
    if (!is_root_) { last_error_ = "Root privileges required"; return false; }

    alpm_db_t* localdb = alpm_get_localdb(handle_);
    alpm_pkg_t* pkg = alpm_db_get_pkg(localdb, name.c_str());
    if (!pkg) {
        last_error_ = "Package not installed: " + name;
        return false;
    }

    int ret = alpm_trans_init(handle_, ALPM_TRANS_FLAG_RECURSE);
    if (ret != 0) {
        last_error_ = "Failed to init transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        return false;
    }

    ret = alpm_remove_pkg(handle_, pkg);
    if (ret != 0) {
        last_error_ = "Failed to add package for removal: " + std::string(alpm_strerror(alpm_errno(handle_)));
        alpm_trans_release(handle_);
        return false;
    }

    alpm_list_t* data = nullptr;
    ret = alpm_trans_prepare(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to prepare transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    ret = alpm_trans_commit(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to commit transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    FREELIST(data);
    alpm_trans_release(handle_);
    return true;
}

/* performs full system upgrade via alpm */
bool AlpmWrapper::system_upgrade() {
    if (!handle_) { last_error_ = "Not initialized"; return false; }
    if (!is_root_) { last_error_ = "Root privileges required"; return false; }

    int ret = alpm_trans_init(handle_, 0);
    if (ret != 0) {
        last_error_ = "Failed to init transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        return false;
    }

    ret = alpm_sync_sysupgrade(handle_, 0);
    if (ret != 0) {
        last_error_ = "Failed to prepare sysupgrade: " + std::string(alpm_strerror(alpm_errno(handle_)));
        alpm_trans_release(handle_);
        return false;
    }

    alpm_list_t* data = nullptr;
    ret = alpm_trans_prepare(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to prepare transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    alpm_list_t* pkgs = alpm_trans_get_add(handle_);
    if (!pkgs) {
        last_error_ = "System is up to date";
        alpm_trans_release(handle_);
        return true;
    }

    ret = alpm_trans_commit(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to commit transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    FREELIST(data);
    alpm_trans_release(handle_);
    return true;
}

bool AlpmWrapper::sync_databases(bool force) {
    if (!handle_) { last_error_ = "Not initialized"; return false; }
    if (!is_root_) { last_error_ = "Root privileges required"; return false; }

    alpm_list_t* syncdbs = alpm_get_syncdbs(handle_);
    int ret = alpm_db_update(handle_, syncdbs, force ? 1 : 0);
    if (ret < 0) {
        last_error_ = "Failed to sync databases: " + std::string(alpm_strerror(alpm_errno(handle_)));
        return false;
    }
    return true;
}

/* scans pacman cache dirs for available package versions */
std::vector<std::pair<std::string, std::string>> AlpmWrapper::list_cached_versions(const std::string& name) {
    std::vector<std::pair<std::string, std::string>> results;
    if (!handle_) return results;

    alpm_list_t* cachedirs = alpm_option_get_cachedirs(handle_);
    std::string prefix = name + "-";

    for (alpm_list_t* i = cachedirs; i; i = alpm_list_next(i)) {
        const char* dir = static_cast<const char*>(i->data);
        DIR* d = opendir(dir);
        if (!d) continue;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string fname = entry->d_name;
            if (fname.find(prefix) != 0) continue;

            size_t pkg_pos = fname.find(".pkg.tar.");
            if (pkg_pos == std::string::npos) continue;

            std::string after_name = fname.substr(prefix.size(), pkg_pos - prefix.size());
            size_t last_dash = after_name.rfind('-');
            if (last_dash == std::string::npos) continue;
            std::string arch = after_name.substr(last_dash + 1);
            if (arch != "any" && arch != "x86_64") continue;

            std::string version = after_name.substr(0, last_dash);
            if (version.empty()) continue;

            std::string filepath = std::string(dir) + fname;
            results.emplace_back(version, filepath);
        }
        closedir(d);
    }

    std::sort(results.begin(), results.end(),
              [this](const auto& a, const auto& b) {
                  return alpm_pkg_vercmp(a.first.c_str(), b.first.c_str()) > 0;
              });

    auto it = std::unique(results.begin(), results.end(),
                          [](const auto& a, const auto& b) { return a.first == b.first; });
    results.erase(it, results.end());

    return results;
}

/* installs an older cached package with NODEPS flag */
bool AlpmWrapper::downgrade_package(const std::string& filepath) {
    if (!handle_) { last_error_ = "Not initialized"; return false; }
    if (!is_root_) { last_error_ = "Root privileges required"; return false; }

    alpm_pkg_t* pkg = nullptr;
    int siglevel = ALPM_SIG_PACKAGE_OPTIONAL;
    int ret = alpm_pkg_load(handle_, filepath.c_str(), 1, siglevel, &pkg);
    if (ret != 0 || !pkg) {
        last_error_ = "Failed to load package: " + filepath;
        return false;
    }

    ret = alpm_trans_init(handle_, ALPM_TRANS_FLAG_NODEPS);
    if (ret != 0) {
        last_error_ = "Failed to init transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        return false;
    }

    ret = alpm_add_pkg(handle_, pkg);
    if (ret != 0) {
        last_error_ = "Failed to add package: " + std::string(alpm_strerror(alpm_errno(handle_)));
        alpm_trans_release(handle_);
        return false;
    }

    alpm_list_t* data = nullptr;
    ret = alpm_trans_prepare(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to prepare transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    ret = alpm_trans_commit(handle_, &data);
    if (ret != 0) {
        last_error_ = "Failed to commit transaction: " + std::string(alpm_strerror(alpm_errno(handle_)));
        FREELIST(data);
        alpm_trans_release(handle_);
        return false;
    }

    FREELIST(data);
    alpm_trans_release(handle_);
    return true;
}

PackageInfo AlpmWrapper::pkg_to_info(alpm_pkg_t* pkg, const std::string& repo) {
    PackageInfo info;
    const char* s;

    s = alpm_pkg_get_name(pkg);
    if (s) info.name = s;
    s = alpm_pkg_get_version(pkg);
    if (s) info.version = s;
    s = alpm_pkg_get_desc(pkg);
    if (s) info.description = s;
    s = alpm_pkg_get_url(pkg);
    if (s) info.url = s;
    s = alpm_pkg_get_packager(pkg);
    if (s) info.packager = s;
    s = alpm_pkg_get_arch(pkg);
    if (s) info.arch = s;

    info.repo = repo;
    info.download_size = alpm_pkg_get_size(pkg);
    info.install_size = alpm_pkg_get_isize(pkg);
    info.build_date = alpm_pkg_get_builddate(pkg);
    info.install_date = alpm_pkg_get_installdate(pkg);
    info.source = PackageSource::Sync;

    info.licenses = list_to_strings(alpm_pkg_get_licenses(pkg));
    info.groups = list_to_strings(alpm_pkg_get_groups(pkg));
    info.depends = deplist_to_strings(alpm_pkg_get_depends(pkg));
    info.optdepends = deplist_to_strings(alpm_pkg_get_optdepends(pkg));
    info.conflicts = deplist_to_strings(alpm_pkg_get_conflicts(pkg));
    info.provides = deplist_to_strings(alpm_pkg_get_provides(pkg));
    return info;
}

std::vector<PackageInfo> AlpmWrapper::list_foreign() {
    std::vector<PackageInfo> results;
    if (!handle_) return results;

    alpm_db_t* localdb = alpm_get_localdb(handle_);
    alpm_list_t* pkgs = alpm_db_get_pkgcache(localdb);
    alpm_list_t* syncdbs = alpm_get_syncdbs(handle_);

    for (alpm_list_t* i = pkgs; i; i = alpm_list_next(i)) {
        alpm_pkg_t* pkg = static_cast<alpm_pkg_t*>(i->data);
        const char* name = alpm_pkg_get_name(pkg);

        bool in_sync = false;
        for (alpm_list_t* j = syncdbs; j; j = alpm_list_next(j)) {
            alpm_db_t* db = static_cast<alpm_db_t*>(j->data);
            if (alpm_db_get_pkg(db, name)) {
                in_sync = true;
                break;
            }
        }

        if (!in_sync) {
            auto info = pkg_to_info(pkg, "local");
            info.installed = true;
            info.installed_version = info.version;
            info.source = PackageSource::Local;
            results.push_back(std::move(info));
        }
    }

    return results;
}

bool AlpmWrapper::is_dep_satisfied(const std::string& depstring) {
    if (!handle_) return false;
    alpm_depend_t* dep = alpm_dep_from_string(depstring.c_str());
    if (!dep) return false;
    alpm_db_t* localdb = alpm_get_localdb(handle_);
    alpm_list_t* pkgs = alpm_db_get_pkgcache(localdb);
    alpm_pkg_t* found = alpm_find_satisfier(pkgs, depstring.c_str());
    alpm_dep_free(dep);
    return found != nullptr;
}

bool AlpmWrapper::is_dep_in_repos(const std::string& depstring) {
    if (!handle_) return false;
    alpm_depend_t* dep = alpm_dep_from_string(depstring.c_str());
    if (!dep) return false;
    alpm_list_t* syncdbs = alpm_get_syncdbs(handle_);
    alpm_pkg_t* found = alpm_find_dbs_satisfier(handle_, syncdbs, depstring.c_str());
    alpm_dep_free(dep);
    return found != nullptr;
}

void AlpmWrapper::mark_installed(PackageInfo& info) {
    if (!handle_) return;
    alpm_db_t* localdb = alpm_get_localdb(handle_);
    alpm_pkg_t* local_pkg = alpm_db_get_pkg(localdb, info.name.c_str());
    if (local_pkg) {
        info.installed = true;
        info.installed_version = alpm_pkg_get_version(local_pkg);
        if (info.version != info.installed_version) {
            info.has_update = true;
        }
    }
}

std::vector<std::string> AlpmWrapper::list_to_strings(alpm_list_t* list) {
    std::vector<std::string> result;
    for (alpm_list_t* i = list; i; i = alpm_list_next(i)) {
        const char* s = static_cast<const char*>(i->data);
        if (s) result.emplace_back(s);
    }
    return result;
}

std::vector<std::string> AlpmWrapper::deplist_to_strings(alpm_list_t* list) {
    std::vector<std::string> result;
    for (alpm_list_t* i = list; i; i = alpm_list_next(i)) {
        alpm_depend_t* dep = static_cast<alpm_depend_t*>(i->data);
        if (dep) {
            char* str = alpm_dep_compute_string(dep);
            if (str) {
                result.emplace_back(str);
                free(str);
            }
        }
    }
    return result;
}

void AlpmWrapper::progress_callback(void* ctx, alpm_progress_t, const char* pkgname,
                                    int percent, size_t, size_t) {
    auto* self = static_cast<AlpmWrapper*>(ctx);
    if (self->progress_cb_) {
        std::string label = pkgname ? pkgname : "Processing";
        self->progress_cb_(label, percent / 100.0);
    }
}

void AlpmWrapper::download_callback(void* ctx, const char* filename,
                                    alpm_download_event_type_t event, void* data) {
    auto* self = static_cast<AlpmWrapper*>(ctx);
    if (!self->progress_cb_) return;

    std::string label = filename ? filename : "Downloading";

    if (event == ALPM_DOWNLOAD_PROGRESS) {
        auto* progress = static_cast<alpm_download_event_progress_t*>(data);
        if (progress && progress->total > 0) {
            self->progress_cb_("Downloading " + label,
                             static_cast<double>(progress->downloaded) / progress->total);
        }
    }
}

void AlpmWrapper::event_callback(void* ctx, alpm_event_t* event) {
    auto* self = static_cast<AlpmWrapper*>(ctx);
    if (!self->event_cb_ || !event) return;

    switch (event->type) {
        case ALPM_EVENT_CHECKDEPS_START:
            self->event_cb_("Checking dependencies...");
            break;
        case ALPM_EVENT_RESOLVEDEPS_START:
            self->event_cb_("Resolving dependencies...");
            break;
        case ALPM_EVENT_INTERCONFLICTS_START:
            self->event_cb_("Checking for conflicts...");
            break;
        case ALPM_EVENT_TRANSACTION_START:
            self->event_cb_("Processing transaction...");
            break;
        case ALPM_EVENT_INTEGRITY_START:
            self->event_cb_("Checking integrity...");
            break;
        case ALPM_EVENT_KEYRING_START:
            self->event_cb_("Checking keyring...");
            break;
        case ALPM_EVENT_KEY_DOWNLOAD_START:
            self->event_cb_("Downloading keys...");
            break;
        case ALPM_EVENT_LOAD_START:
            self->event_cb_("Loading packages...");
            break;
        case ALPM_EVENT_DISKSPACE_START:
            self->event_cb_("Checking disk space...");
            break;
        case ALPM_EVENT_DB_RETRIEVE_START:
            self->event_cb_("Retrieving packages...");
            break;
        default:
            break;
    }
}

void AlpmWrapper::question_callback(void*, alpm_question_t* question) {
    if (question) {
        question->any.answer = 1;
    }
}

}
