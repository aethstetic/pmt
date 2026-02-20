#include "aur.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>

namespace pmt {

static constexpr const char* AUR_HOST = "aur.archlinux.org";

AurClient::AurClient() {
    SSL_library_init();
    SSL_load_error_strings();
    ctx_ = SSL_CTX_new(TLS_client_method());
}

AurClient::~AurClient() {
    if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
    if (sockfd_ >= 0) close(sockfd_);
    if (ctx_) SSL_CTX_free(ctx_);
}

/* establishes persistent TLS connection to aur.archlinux.org */
bool AurClient::ensure_connected() {
    if (ssl_) return true;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_ret = getaddrinfo(AUR_HOST, "443", &hints, &res);
    if (gai_ret != 0) {
        last_error_ = "DNS resolution failed: " + std::string(gai_strerror(gai_ret));
        return false;
    }

    sockfd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd_ < 0) {
        last_error_ = "Socket creation failed";
        freeaddrinfo(res);
        return false;
    }

    if (connect(sockfd_, res->ai_addr, res->ai_addrlen) < 0) {
        last_error_ = "Connection failed";
        close(sockfd_); sockfd_ = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    if (!ctx_) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            last_error_ = "SSL context creation failed";
            close(sockfd_); sockfd_ = -1;
            return false;
        }
    }

    ssl_ = SSL_new(ctx_);
    SSL_set_fd(ssl_, sockfd_);
    SSL_set_tlsext_host_name(ssl_, AUR_HOST);

    if (SSL_connect(ssl_) <= 0) {
        last_error_ = "SSL handshake failed";
        SSL_free(ssl_); ssl_ = nullptr;
        close(sockfd_); sockfd_ = -1;
        return false;
    }

    reset_rbuf();
    return true;
}

void AurClient::disconnect() {
    if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
    if (sockfd_ >= 0) { close(sockfd_); sockfd_ = -1; }
    reset_rbuf();
}

void AurClient::preconnect() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    ensure_connected();
}

void AurClient::reset_rbuf() {
    rbuf_len_ = 0;
    rbuf_pos_ = 0;
}

int AurClient::ssl_read_byte() {
    if (rbuf_pos_ >= rbuf_len_) {
        rbuf_len_ = SSL_read(ssl_, rbuf_, RBUF_SIZE);
        rbuf_pos_ = 0;
        if (rbuf_len_ <= 0) return -1;
    }
    return static_cast<unsigned char>(rbuf_[rbuf_pos_++]);
}

/* reads HTTP response with content-length and chunked encoding support */
std::string AurClient::read_http_response() {
    std::string headers;
    headers.reserve(512);
    for (;;) {
        int b = ssl_read_byte();
        if (b < 0) { last_error_ = "Connection closed during headers"; return ""; }
        headers += static_cast<char>(b);
        size_t len = headers.size();
        if (len >= 4 &&
            headers[len-4] == '\r' && headers[len-3] == '\n' &&
            headers[len-2] == '\r' && headers[len-1] == '\n') {
            headers.resize(len - 4);
            break;
        }
    }

    int content_length = -1;
    bool chunked = false;

    std::string hdrs_lower = headers;
    for (auto& c : hdrs_lower) c = static_cast<char>(tolower(c));

    size_t cl_pos = hdrs_lower.find("content-length:");
    if (cl_pos != std::string::npos) {
        content_length = atoi(headers.c_str() + cl_pos + 15);
    }
    if (hdrs_lower.find("transfer-encoding: chunked") != std::string::npos) {
        chunked = true;
    }

    std::string body;
    if (content_length >= 0) {
        body.reserve(content_length);
        for (int i = 0; i < content_length; ++i) {
            int b = ssl_read_byte();
            if (b < 0) break;
            body += static_cast<char>(b);
        }
    } else if (chunked) {
        for (;;) {
            std::string size_line;
            bool ok = true;
            for (;;) {
                int b = ssl_read_byte();
                if (b < 0) { ok = false; break; }
                if (b == '\n') break;
                if (b != '\r') size_line += static_cast<char>(b);
            }
            if (!ok) break;
            long chunk_size = strtol(size_line.c_str(), nullptr, 16);
            if (chunk_size <= 0) break;

            for (long i = 0; i < chunk_size; ++i) {
                int b = ssl_read_byte();
                if (b < 0) { ok = false; break; }
                body += static_cast<char>(b);
            }
            if (!ok) break;
            ssl_read_byte();
            ssl_read_byte();
        }
    }

    return body;
}

/* HTTPS GET with keep-alive and auto-reconnect on failure */
std::string AurClient::https_get(const std::string& path) {
    std::lock_guard<std::mutex> lock(conn_mutex_);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected()) return "";

        std::string request = "GET " + path + " HTTP/1.1\r\n"
                              "Host: " + AUR_HOST + "\r\n"
                              "Connection: keep-alive\r\n"
                              "User-Agent: pmt/1.0\r\n"
                              "\r\n";

        int written = SSL_write(ssl_, request.c_str(), static_cast<int>(request.size()));
        if (written <= 0) {
            disconnect();
            continue;
        }

        reset_rbuf();
        std::string body = read_http_response();
        if (!body.empty()) return body;

        disconnect();
    }

    if (last_error_.empty()) last_error_ = "HTTPS request failed";
    return "";
}

std::string AurClient::url_encode(const std::string& s) {
    std::string result;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            result += buf;
        }
    }
    return result;
}

std::vector<PackageInfo> AurClient::search(const std::string& query) {
    std::string path = "/rpc/v5/search/" + url_encode(query);
    std::string body = https_get(path);
    if (body.empty()) return {};
    return parse_results(body);
}

PackageInfo AurClient::info(const std::string& name) {
    std::string path = "/rpc/v5/info?arg[]=" + url_encode(name);
    std::string body = https_get(path);
    if (body.empty()) return {};
    auto results = parse_results(body);
    if (results.empty()) return {};
    return results[0];
}

std::vector<PackageInfo> AurClient::search_provides(const std::string& name) {
    std::string path = "/rpc/v5/search/" + url_encode(name) + "?by=provides";
    std::string body = https_get(path);
    if (body.empty()) return {};
    return parse_results(body);
}

std::vector<PackageInfo> AurClient::info_batch(const std::vector<std::string>& names) {
    if (names.empty()) return {};

    std::vector<PackageInfo> all_results;
    static constexpr size_t MAX_URL_LEN = 4000;
    static const std::string base_path = "/rpc/v5/info?";

    size_t i = 0;
    while (i < names.size()) {
        std::string path = base_path;
        bool first = true;
        size_t batch_start = i;

        while (i < names.size()) {
            std::string param = (first ? "" : "&");
            param += "arg[]=" + url_encode(names[i]);
            if (path.size() + param.size() > MAX_URL_LEN && !first)
                break;
            path += param;
            first = false;
            ++i;
        }

        if (i == batch_start) break;

        std::string body = https_get(path);
        if (body.empty()) continue;
        auto results = parse_results(body);
        for (auto& pkg : results)
            all_results.push_back(std::move(pkg));
    }

    return all_results;
}

void AurClient::log_msg(const std::string& log_file, const std::string& msg) {
    if (log_file.empty()) return;
    FILE* f = fopen(log_file.c_str(), "a");
    if (f) {
        fprintf(f, "%s\n", msg.c_str());
        fclose(f);
    }
}

int AurClient::run_cmd(const std::string& cmd, const std::string& log_file) {
    std::string full_cmd;
    if (!log_file.empty()) {
        full_cmd = cmd + " >> '" + log_file + "' 2>&1";
    } else {
        full_cmd = cmd + " 2>&1";
    }
    int ret = system(full_cmd.c_str());
    if (ret == -1) return -1;
    return WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;
}

/* resolves real user home directory, handling SUDO_USER */
std::string AurClient::resolve_home_dir() {
    const char* home = nullptr;
    if (geteuid() == 0) {
        const char* sudo_user = getenv("SUDO_USER");
        if (sudo_user && sudo_user[0]) {
            std::string cmd = std::string("getent passwd '") + sudo_user + "' | cut -d: -f6";
            FILE* fp = popen(cmd.c_str(), "r");
            if (fp) {
                static char buf[256];
                if (fgets(buf, sizeof(buf), fp)) {
                    size_t len = strlen(buf);
                    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
                    home = buf;
                }
                pclose(fp);
            }
        }
    }
    if (!home || !home[0]) home = getenv("HOME");
    if (!home || !home[0]) return "/tmp";
    return std::string(home);
}

std::string AurClient::default_cache_dir() {
    return resolve_home_dir() + "/.cache/pmt/aur";
}

std::string AurClient::reviewed_cache_dir() {
    return resolve_home_dir() + "/.cache/pmt/reviewed";
}

std::string AurClient::parse_pkgbuild_version(const std::string& pkgbuild_path) {
    std::ifstream f(pkgbuild_path);
    if (!f) return "";

    std::string pkgver, pkgrel;
    std::string line;
    while (std::getline(f, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;

        if (line.compare(start, 7, "pkgver=") == 0) {
            pkgver = line.substr(start + 7);
            if (!pkgver.empty() && (pkgver.front() == '\'' || pkgver.front() == '"'))
                pkgver = pkgver.substr(1);
            if (!pkgver.empty() && (pkgver.back() == '\'' || pkgver.back() == '"'))
                pkgver.pop_back();
        } else if (line.compare(start, 7, "pkgrel=") == 0) {
            pkgrel = line.substr(start + 7);
            if (!pkgrel.empty() && (pkgrel.front() == '\'' || pkgrel.front() == '"'))
                pkgrel = pkgrel.substr(1);
            if (!pkgrel.empty() && (pkgrel.back() == '\'' || pkgrel.back() == '"'))
                pkgrel.pop_back();
        }
        if (!pkgver.empty() && !pkgrel.empty()) break;
    }

    if (pkgver.empty()) return "";
    if (pkgrel.empty()) return pkgver;
    return pkgver + "-" + pkgrel;
}

bool AurClient::is_vcs_package(const std::string& name) {
    static const char* suffixes[] = {"-git", "-svn", "-hg", "-bzr", "-fossil", "-cvs"};
    for (const char* suffix : suffixes) {
        size_t slen = strlen(suffix);
        if (name.size() >= slen && name.compare(name.size() - slen, slen, suffix) == 0)
            return true;
    }
    return false;
}

/* runs makepkg --nobuild to determine real VCS package version */
std::string AurClient::check_vcs_version(const std::string& name,
                                          const std::string& pkgbase,
                                          const std::string& log_file) {
    namespace fs = std::filesystem;

    std::string base = (!pkgbase.empty() && pkgbase != name) ? pkgbase : name;
    std::string actual_dir = default_cache_dir();
    std::string pkg_dir = actual_dir + "/" + base;

    const char* sudo_user = getenv("SUDO_USER");
    std::string as_user;
    if (geteuid() == 0 && sudo_user && sudo_user[0])
        as_user = std::string("sudo -H -u ") + sudo_user + " ";

    fs::create_directories(actual_dir);
    if (!as_user.empty()) {
        std::string cmd = "chown '" + std::string(sudo_user) + "' '" + actual_dir + "'";
        system(cmd.c_str());
    }

    if (fs::exists(pkg_dir + "/.git")) {
        log_msg(log_file, "Resetting local changes in " + base + "...");
        std::string reset_cmd = as_user + "git -C '" + pkg_dir + "' checkout -- .";
        run_cmd(reset_cmd, log_file);

        log_msg(log_file, "Updating existing clone of " + base + "...");
        std::string cmd = as_user + "git -C '" + pkg_dir + "' pull --ff-only";
        if (run_cmd(cmd, log_file) != 0) {
            log_msg(log_file, "Pull failed, re-cloning...");
            fs::remove_all(pkg_dir);
        }
    }

    if (!fs::exists(pkg_dir)) {
        log_msg(log_file, "Cloning https://aur.archlinux.org/" + base + ".git ...");
        std::string cmd = as_user + "git clone --depth 1 'https://aur.archlinux.org/"
                          + base + ".git' '" + pkg_dir + "'";
        if (run_cmd(cmd, log_file) != 0) {
            log_msg(log_file, "Failed to clone " + base + ", skipping VCS check");
            return "";
        }
    }

    if (!as_user.empty() && fs::exists(pkg_dir)) {
        std::string cmd = "chown -R '" + std::string(sudo_user) + "' '" + pkg_dir + "'";
        system(cmd.c_str());
    }

    std::string pkgbuild_path = pkg_dir + "/PKGBUILD";
    if (!fs::exists(pkgbuild_path)) {
        log_msg(log_file, "No PKGBUILD found for " + base);
        return "";
    }

    {
        std::ifstream f(pkgbuild_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        if (content.find("pkgver()") == std::string::npos) {
            log_msg(log_file, base + ": no pkgver() function, using static version");
            return parse_pkgbuild_version(pkgbuild_path);
        }
    }

    log_msg(log_file, "Running makepkg --nobuild for " + base + " (fetching VCS sources)...");
    std::string cmd = as_user + "bash -c 'cd \"" + pkg_dir
                      + "\" && timeout 120 makepkg --nobuild --nocheck -f'";
    int rc = run_cmd(cmd, log_file);

    if (rc != 0 && rc != 13) {
        log_msg(log_file, "makepkg --nobuild failed for " + base + " (exit " + std::to_string(rc) + "), skipping");
        return "";
    }

    std::string version = parse_pkgbuild_version(pkgbuild_path);
    if (!version.empty()) {
        log_msg(log_file, base + ": real VCS version is " + version);
    }
    return version;
}

/* clones/updates AUR repo and returns PKGBUILD contents for review */
std::string AurClient::fetch_pkgbuild(const std::string& name, const std::string& pkgbase) {
    namespace fs = std::filesystem;

    std::string base = (!pkgbase.empty() && pkgbase != name) ? pkgbase : name;
    std::string actual_dir = default_cache_dir();
    std::string pkg_dir = actual_dir + "/" + base;

    const char* sudo_user = getenv("SUDO_USER");
    std::string as_user;
    if (geteuid() == 0 && sudo_user && sudo_user[0])
        as_user = std::string("sudo -H -u ") + sudo_user + " ";

    fs::create_directories(actual_dir);
    if (!as_user.empty()) {
        std::string cmd = "chown '" + std::string(sudo_user) + "' '" + actual_dir + "'";
        system(cmd.c_str());
    }

    if (fs::exists(pkg_dir + "/.git")) {
        std::string cmd = as_user + "git -C '" + pkg_dir + "' pull --ff-only >/dev/null 2>&1";
        system(cmd.c_str());
    } else {
        std::string cmd = as_user + "git clone --depth 1 'https://aur.archlinux.org/"
                          + base + ".git' '" + pkg_dir + "' >/dev/null 2>&1";
        if (system(cmd.c_str()) != 0) {
            last_error_ = "Failed to clone AUR package: " + base;
            return "";
        }
    }

    std::string pkgbuild_path = pkg_dir + "/PKGBUILD";
    std::ifstream f(pkgbuild_path);
    if (!f) {
        last_error_ = "PKGBUILD not found for: " + base;
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

/* clones AUR git repo and runs makepkg to produce .pkg.tar.zst */
std::string AurClient::build_package(const std::string& name,
                                     const std::string& log_file,
                                     const std::string& build_dir,
                                     const std::string& pkgbase) {
    namespace fs = std::filesystem;

    std::string base = (!pkgbase.empty() && pkgbase != name) ? pkgbase : name;

    std::string actual_dir = build_dir.empty() ? default_cache_dir() : build_dir;
    std::string pkg_dir = actual_dir + "/" + base;

    const char* sudo_user = getenv("SUDO_USER");
    std::string as_user;
    if (geteuid() == 0) {
        if (sudo_user && sudo_user[0]) {
            as_user = std::string("sudo -H -u ") + sudo_user + " ";
        } else {
            last_error_ = "Cannot build AUR packages as root directly. Use: sudo ./pmt";
            return "";
        }
    }

    log_msg(log_file, "Preparing build directory...");
    fs::create_directories(actual_dir);
    if (!as_user.empty()) {
        std::string cmd = "chown '" + std::string(sudo_user) + "' '" + actual_dir + "'";
        system(cmd.c_str());
        if (fs::exists(pkg_dir)) {
            cmd = "chown -R '" + std::string(sudo_user) + "' '" + pkg_dir + "'";
            system(cmd.c_str());
        }
    }

    if (fs::exists(pkg_dir + "/.git")) {
        log_msg(log_file, "Updating existing clone...");
        std::string cmd = as_user + "git -C '" + pkg_dir + "' pull --ff-only";
        if (run_cmd(cmd, log_file) != 0) {
            log_msg(log_file, "Pull failed, re-cloning...");
            fs::remove_all(pkg_dir);
        }
    }

    if (!fs::exists(pkg_dir)) {
        log_msg(log_file, "Cloning https://aur.archlinux.org/" + base + ".git ...");
        std::string cmd = as_user + "git clone --depth 1 'https://aur.archlinux.org/"
                          + base + ".git' '" + pkg_dir + "'";
        if (run_cmd(cmd, log_file) != 0) {
            last_error_ = "Failed to clone AUR package: " + base;
            return "";
        }
    }

    if (!fs::exists(pkg_dir + "/PKGBUILD")) {
        last_error_ = "PKGBUILD not found for: " + name;
        return "";
    }

    std::string pkgbuild_ver = parse_pkgbuild_version(pkg_dir + "/PKGBUILD");
    if (!pkgbuild_ver.empty()) {
        for (const auto& entry : fs::directory_iterator(pkg_dir)) {
            std::string fname = entry.path().filename().string();
            if (fname.find(".pkg.tar") != std::string::npos &&
                fname.find(pkgbuild_ver) != std::string::npos) {
                log_msg(log_file, "Using cached build: " + fname);
                return entry.path().string();
            }
        }
    }

    for (const auto& entry : fs::directory_iterator(pkg_dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.find(".pkg.tar") != std::string::npos)
            fs::remove(entry.path());
    }

    log_msg(log_file, "Running makepkg -sf --nocheck --noconfirm ...");
    std::string cmd = as_user + "bash -c 'cd \"" + pkg_dir
                      + "\" && MAKEFLAGS=-j$(nproc)"
                        " PKGDEST=\"" + pkg_dir
                      + "\" makepkg -sf --nocheck --noconfirm'";
    if (run_cmd(cmd, log_file) != 0) {
        last_error_ = "makepkg failed for: " + name;
        return "";
    }

    log_msg(log_file, "Locating built package...");

    for (const auto& entry : fs::directory_iterator(pkg_dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.find(".pkg.tar") != std::string::npos) {
            return entry.path().string();
        }
    }

    std::string list_cmd = as_user + "bash -c 'cd \"" + pkg_dir
                           + "\" && makepkg --packagelist 2>/dev/null'";
    FILE* fp = popen(list_cmd.c_str(), "r");
    if (fp) {
        char buf[512];
        while (fgets(buf, sizeof(buf), fp)) {
            std::string path = buf;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            if (!path.empty() && fs::exists(path)) {
                pclose(fp);
                return path;
            }
        }
        pclose(fp);
    }

    last_error_ = "Built package not found for: " + name;
    return "";
}

std::vector<PackageInfo> AurClient::parse_results(const std::string& json_str) {
    JsonParser parser;
    auto root = parser.parse(json_str);
    if (!root || !root->is_object()) {
        last_error_ = "Failed to parse AUR response: " + parser.error();
        return {};
    }

    auto results = (*root)["results"];
    if (!results || !results->is_array()) return {};

    std::vector<PackageInfo> packages;
    for (const auto& item : results->array_val) {
        packages.push_back(json_to_package(item));
    }
    return packages;
}

PackageInfo AurClient::json_to_package(const JsonPtr& obj) {
    PackageInfo info;
    info.source = PackageSource::AUR;
    info.repo = "aur";
    info.name = (*obj)["Name"]->str();
    info.version = (*obj)["Version"]->str();
    info.description = (*obj)["Description"]->str();
    info.url = (*obj)["URL"]->str();
    auto pkgbase = (*obj)["PackageBase"];
    if (pkgbase && !pkgbase->is_null())
        info.pkgbase = pkgbase->str();
    info.aur_votes = (*obj)["NumVotes"]->integer();
    info.aur_maintainer = (*obj)["Maintainer"]->str();
    info.aur_out_of_date = !(*obj)["OutOfDate"]->is_null();

    auto deps = (*obj)["Depends"];
    if (deps && deps->is_array()) {
        for (const auto& d : deps->array_val)
            info.depends.push_back(d->str());
    }

    auto optdeps = (*obj)["OptDepends"];
    if (optdeps && optdeps->is_array()) {
        for (const auto& d : optdeps->array_val)
            info.optdepends.push_back(d->str());
    }

    auto conflicts = (*obj)["Conflicts"];
    if (conflicts && conflicts->is_array()) {
        for (const auto& d : conflicts->array_val)
            info.conflicts.push_back(d->str());
    }

    auto provides = (*obj)["Provides"];
    if (provides && provides->is_array()) {
        for (const auto& d : provides->array_val)
            info.provides.push_back(d->str());
    }

    auto makedeps = (*obj)["MakeDepends"];
    if (makedeps && makedeps->is_array()) {
        for (const auto& d : makedeps->array_val)
            info.makedepends.push_back(d->str());
    }

    auto licenses = (*obj)["License"];
    if (licenses && licenses->is_array()) {
        for (const auto& l : licenses->array_val)
            info.licenses.push_back(l->str());
    }

    return info;
}

}
