#pragma once
#include "package.h"
#include "aur.h"
#include "alpm_wrapper.h"
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>

namespace pmt {

struct DepResolution {
    bool ok = false;
    std::string error;
    std::vector<PackageInfo> aur_build_order;
    std::vector<std::string> repo_deps;
    std::vector<std::string> satisfied_deps;
};

using LogCallback = std::function<void(const std::string&)>;

class DepResolver {
public:
    DepResolver(AurClient& aur, AlpmWrapper& alpm);

    DepResolution resolve(const std::string& name, LogCallback log = nullptr);

private:
    AurClient& aur_;
    AlpmWrapper& alpm_;
    LogCallback log_;

    std::set<std::string> visited_;
    std::set<std::string> in_stack_;
    std::vector<PackageInfo> build_order_;
    std::vector<std::string> repo_deps_;
    std::vector<std::string> satisfied_deps_;
    std::string error_;

    std::map<std::string, PackageInfo> aur_cache_;
    std::map<std::string, std::string> provides_map_;

    bool dfs(const std::string& name);
    std::string find_provider(const std::string& dep_name);
    static std::string strip_version(const std::string& depstring);
};

}
