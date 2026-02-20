#pragma once
#include <string>
#include <vector>

namespace pmt {

struct RepoConfig {
    std::string name;
    std::vector<std::string> servers;
    int siglevel = -1;
};

struct PacmanConfig {
    std::string root_dir = "/";
    std::string db_path = "/var/lib/pacman/";
    std::string log_file = "/var/log/pacman.log";
    std::string gpg_dir = "/etc/pacman.d/gnupg/";
    std::string architecture = "auto";
    int siglevel = 0;
    std::vector<RepoConfig> repos;

    bool parse(const std::string& path = "/etc/pacman.conf");

private:
    bool parse_mirrorlist(const std::string& path, std::vector<std::string>& servers);
    void resolve_architecture();
    int parse_siglevel(const std::string& value);
};

}
