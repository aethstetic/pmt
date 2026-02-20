#pragma once
#include "package.h"
#include "json.h"
#include <string>
#include <vector>
#include <mutex>
#include <openssl/ssl.h>

namespace pmt {

class AurClient {
public:
    AurClient();
    ~AurClient();

    AurClient(const AurClient&) = delete;
    AurClient& operator=(const AurClient&) = delete;

    std::vector<PackageInfo> search(const std::string& query);
    PackageInfo info(const std::string& name);
    std::vector<PackageInfo> search_provides(const std::string& name);
    std::vector<PackageInfo> info_batch(const std::vector<std::string>& names);
    void preconnect();
    static bool is_vcs_package(const std::string& name);
    std::string check_vcs_version(const std::string& name,
                                  const std::string& pkgbase = "",
                                  const std::string& log_file = "");
    std::string build_package(const std::string& name,
                              const std::string& log_file = "",
                              const std::string& build_dir = "",
                              const std::string& pkgbase = "");
    std::string fetch_pkgbuild(const std::string& name, const std::string& pkgbase = "");
    std::string last_error() const { return last_error_; }

private:
    std::string last_error_;

    std::mutex conn_mutex_;
    int sockfd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;

    static constexpr int RBUF_SIZE = 8192;
    char rbuf_[RBUF_SIZE]{};
    int rbuf_len_ = 0;
    int rbuf_pos_ = 0;

    bool ensure_connected();
    void disconnect();
    void reset_rbuf();
    int ssl_read_byte();
    std::string https_get(const std::string& path);
    std::string read_http_response();
    std::vector<PackageInfo> parse_results(const std::string& json_str);
    PackageInfo json_to_package(const JsonPtr& obj);
    static std::string url_encode(const std::string& s);
    int run_cmd(const std::string& cmd, const std::string& log_file);
    void log_msg(const std::string& log_file, const std::string& msg);
    static std::string parse_pkgbuild_version(const std::string& pkgbuild_path);
    static std::string resolve_home_dir();

public:
    static std::string default_cache_dir();
    static std::string reviewed_cache_dir();
};

}
