#include "dep_resolver.h"

namespace pmt {

DepResolver::DepResolver(AurClient& aur, AlpmWrapper& alpm)
    : aur_(aur), alpm_(alpm) {}

/* resolves full AUR dependency tree into topological build order */
DepResolution DepResolver::resolve(const std::string& name, LogCallback log) {
    log_ = log;
    visited_.clear();
    in_stack_.clear();
    build_order_.clear();
    repo_deps_.clear();
    satisfied_deps_.clear();
    error_.clear();
    aur_cache_.clear();
    provides_map_.clear();

    if (log_) log_("Resolving dependencies for " + name + "...");

    bool ok = dfs(name);

    std::set<std::string> seen_bases;
    std::vector<PackageInfo> deduped;
    for (auto& p : build_order_) {
        std::string base = p.pkgbase.empty() ? p.name : p.pkgbase;
        if (seen_bases.insert(base).second) {
            deduped.push_back(std::move(p));
        } else {
            if (log_) log_("Skipping " + p.name + " (split package, already building " + base + ")");
        }
    }

    DepResolution result;
    result.ok = ok;
    result.error = error_;
    result.aur_build_order = std::move(deduped);
    result.repo_deps = std::move(repo_deps_);
    result.satisfied_deps = std::move(satisfied_deps_);
    return result;
}

/* recursive DFS: fetches AUR info, categorizes deps as repo/aur/satisfied */
bool DepResolver::dfs(const std::string& name) {
    if (visited_.count(name)) return true;

    if (in_stack_.count(name)) {
        error_ = "Circular dependency detected: " + name;
        return false;
    }

    in_stack_.insert(name);

    if (log_) log_("Fetching AUR info for " + name + "...");

    PackageInfo pkg;
    auto it = aur_cache_.find(name);
    if (it != aur_cache_.end()) {
        pkg = it->second;
    } else {
        pkg = aur_.info(name);
        if (pkg.name.empty()) {
            in_stack_.erase(name);
            error_ = "Package not found in AUR: " + name;
            return false;
        }
        aur_cache_[name] = pkg;
    }

    if (alpm_.is_dep_satisfied(pkg.name + "=" + pkg.version)) {
        if (log_) log_("Skipping " + name + " (" + pkg.version + " already installed)");
        visited_.insert(name);
        in_stack_.erase(name);
        return true;
    }

    std::vector<std::string> all_deps;
    all_deps.insert(all_deps.end(), pkg.depends.begin(), pkg.depends.end());
    all_deps.insert(all_deps.end(), pkg.makedepends.begin(), pkg.makedepends.end());

    std::vector<std::string> unknown_names;
    for (const auto& dep : all_deps) {
        std::string dep_name = strip_version(dep);
        if (visited_.count(dep_name) || in_stack_.count(dep_name)) continue;
        if (alpm_.is_dep_satisfied(dep)) continue;
        if (alpm_.is_dep_in_repos(dep)) continue;
        if (aur_cache_.count(dep_name)) continue;
        unknown_names.push_back(dep_name);
    }

    if (!unknown_names.empty()) {
        if (log_) log_("Batch-fetching " + std::to_string(unknown_names.size()) + " AUR dependencies...");
        auto batch = aur_.info_batch(unknown_names);
        for (auto& p : batch) {
            aur_cache_[p.name] = std::move(p);
        }
    }

    for (const auto& dep : all_deps) {
        std::string dep_name = strip_version(dep);

        if (alpm_.is_dep_satisfied(dep)) {
            satisfied_deps_.push_back(dep);
            continue;
        }

        if (alpm_.is_dep_in_repos(dep)) {
            repo_deps_.push_back(dep);
            continue;
        }

        if (aur_cache_.count(dep_name) && !aur_cache_[dep_name].name.empty()) {
            if (!dfs(dep_name)) return false;
            continue;
        }

        std::string provider = find_provider(dep_name);
        if (!provider.empty()) {
            if (!dfs(provider)) return false;
            continue;
        }

        error_ = "Dependency not found anywhere: " + dep + " (required by " + name + ")";
        return false;
    }

    visited_.insert(name);
    in_stack_.erase(name);
    build_order_.push_back(pkg);

    if (log_) log_("Resolved: " + name);
    return true;
}

/* searches AUR for packages that provide a virtual dependency */
std::string DepResolver::find_provider(const std::string& dep_name) {
    auto it = provides_map_.find(dep_name);
    if (it != provides_map_.end())
        return it->second;

    if (log_) log_("Searching AUR for provider of " + dep_name + "...");

    auto providers = aur_.search_provides(dep_name);

    for (auto& p : providers) {
        for (const auto& prov : p.provides) {
            std::string prov_name = strip_version(prov);
            if (prov_name == dep_name) {
                if (log_) log_("Found: " + p.name + " provides " + dep_name);
                provides_map_[dep_name] = p.name;
                aur_cache_[p.name] = std::move(p);
                return provides_map_[dep_name];
            }
        }
    }

    provides_map_[dep_name] = "";
    return "";
}

std::string DepResolver::strip_version(const std::string& depstring) {
    size_t pos = depstring.find_first_of("><=");
    if (pos != std::string::npos)
        return depstring.substr(0, pos);
    return depstring;
}

}
