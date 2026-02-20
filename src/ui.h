#pragma once
#include "terminal.h"
#include "package.h"
#include <string>
#include <vector>

namespace pmt {

enum class Focus {
    SearchBar,
    PackageList,
    DetailPane,
};

struct ProgressInfo {
    std::string label;
    double fraction = 0.0;
    bool active = false;
};

class UI {
public:
    explicit UI(Terminal& term);

    void draw(const std::vector<PackageInfo>& packages);

    Focus focus = Focus::PackageList;
    int selected = 0;
    int list_scroll = 0;
    int detail_scroll = 0;
    std::string search_text;
    int search_cursor = 0;
    bool show_aur = false;
    bool filter_installed = false;
    bool filter_updates = false;
    std::string status_message;
    ProgressInfo progress;
    bool color_disabled = false;
    std::string accent_code;

    int list_width() const;
    int detail_width() const;
    int content_height() const;
    bool show_detail_pane() const;
    void ensure_visible();

    bool draw_confirm_dialog(const std::string& title, const std::vector<std::string>& lines);
    void draw_message(const std::string& title, const std::string& msg);
    int draw_selection_dialog(const std::string& title, const std::vector<std::string>& options);
    void draw_build_log(const std::string& title, const std::vector<std::string>& log_lines,
                        bool finished, int elapsed_secs);
    bool draw_pkgbuild_review(const std::string& pkg_name, const std::string& content,
                              const std::string& old_content = "");

private:
    Terminal& term_;

    int detail_cache_idx_ = -1;
    std::vector<std::string> detail_lines_;
    void rebuild_detail_lines(const std::vector<PackageInfo>& packages);

    void draw_search_bar();
    void draw_package_list(const std::vector<PackageInfo>& packages);
    void draw_detail_pane(const std::vector<PackageInfo>& packages);
    void draw_status_bar(const std::vector<PackageInfo>& packages);
    void draw_borders();

    const char* accent_fg() const;
    const char* color_fg(Terminal::Color c) const;
};

}
