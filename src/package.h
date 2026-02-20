#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace pmt {

enum class PackageSource {
    Sync,
    Local,
    AUR,
};

struct PackageInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string repo;
    std::string url;
    std::string packager;
    std::string arch;
    std::vector<std::string> licenses;
    std::vector<std::string> groups;
    std::vector<std::string> depends;
    std::vector<std::string> optdepends;
    std::vector<std::string> conflicts;
    std::vector<std::string> provides;
    std::vector<std::string> makedepends;
    int64_t download_size = 0;
    int64_t install_size = 0;
    int64_t build_date = 0;
    int64_t install_date = 0;
    PackageSource source = PackageSource::Sync;
    bool installed = false;
    std::string installed_version;
    bool has_update = false;

    std::string pkgbase;
    int aur_votes = 0;
    std::string aur_maintainer;
    bool aur_out_of_date = false;
};

std::string format_size(int64_t bytes);
std::string format_date(int64_t timestamp);

}
