#include "pacman_conf.h"
#include <fstream>
#include <sstream>
#include <sys/utsname.h>

namespace pmt {

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/* parses /etc/pacman.conf into structured config */
bool PacmanConfig::parse(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    std::string current_section;
    RepoConfig* current_repo = nullptr;

    siglevel = (1 << 0) | (1 << 11);

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            if (current_section != "options") {
                repos.push_back({current_section, {}, -1});
                current_repo = &repos.back();
            } else {
                current_repo = nullptr;
            }
            continue;
        }

        size_t eq = line.find('=');
        std::string key, value;
        if (eq != std::string::npos) {
            key = trim(line.substr(0, eq));
            value = trim(line.substr(eq + 1));
        } else {
            key = trim(line);
        }

        if (current_section == "options") {
            if (key == "RootDir") root_dir = value;
            else if (key == "DBPath") db_path = value;
            else if (key == "LogFile") log_file = value;
            else if (key == "GPGDir") gpg_dir = value;
            else if (key == "Architecture") architecture = value;
            else if (key == "SigLevel") siglevel = parse_siglevel(value);
        } else if (current_repo) {
            if (key == "Include") {
                parse_mirrorlist(value, current_repo->servers);
            } else if (key == "Server") {
                std::string url = value;
                size_t pos;
                while ((pos = url.find("$repo")) != std::string::npos)
                    url.replace(pos, 5, current_repo->name);
                resolve_architecture();
                while ((pos = url.find("$arch")) != std::string::npos)
                    url.replace(pos, 5, architecture);
                current_repo->servers.push_back(url);
            } else if (key == "SigLevel") {
                current_repo->siglevel = parse_siglevel(value);
            }
        }
    }

    resolve_architecture();
    return true;
}

bool PacmanConfig::parse_mirrorlist(const std::string& path, std::vector<std::string>& servers) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));
            if (key == "Server") {
                servers.push_back(value);
            }
        }
    }
    return true;
}

void PacmanConfig::resolve_architecture() {
    if (architecture == "auto") {
        struct utsname un{};
        if (uname(&un) == 0) {
            architecture = un.machine;
        } else {
            architecture = "x86_64";
        }
    }
}

/* converts SigLevel string tokens to alpm bitmask */
int PacmanConfig::parse_siglevel(const std::string& value) {
    int level = 0;
    std::istringstream ss(value);
    std::string token;
    while (ss >> token) {
        if (token == "Required") level |= (1 << 0);
        else if (token == "Optional") level |= (1 << 1);
        else if (token == "DatabaseRequired") level |= (1 << 10);
        else if (token == "DatabaseOptional") level |= (1 << 11);
        else if (token == "PackageRequired") level |= (1 << 0);
        else if (token == "PackageOptional") level |= (1 << 1);
        else if (token == "PackageTrustedOnly") {}
        else if (token == "PackageTrustAll") level |= (1 << 2) | (1 << 3);
        else if (token == "DatabaseTrustedOnly") {}
        else if (token == "DatabaseTrustAll") level |= (1 << 12) | (1 << 13);
    }
    return level;
}

}
