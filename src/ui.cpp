#include "ui.h"
#include "input.h"
#include <algorithm>
#include <climits>

namespace {

struct DiffLine {
    char tag;
    std::string text;
};

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : content) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

/* LCS-based diff between old and new file contents */
std::vector<DiffLine> compute_diff(const std::vector<std::string>& old_lines,
                                   const std::vector<std::string>& new_lines) {
    int m = static_cast<int>(old_lines.size());
    int n = static_cast<int>(new_lines.size());

    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            if (old_lines[i - 1] == new_lines[j - 1])
                dp[i][j] = dp[i - 1][j - 1] + 1;
            else
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        }
    }

    std::vector<DiffLine> result;
    int i = m, j = n;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && old_lines[i - 1] == new_lines[j - 1]) {
            result.push_back({' ', old_lines[i - 1]});
            --i; --j;
        } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
            result.push_back({'+', new_lines[j - 1]});
            --j;
        } else {
            result.push_back({'-', old_lines[i - 1]});
            --i;
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

}

namespace pmt {

using C = Terminal::Color;

UI::UI(Terminal& term) : term_(term) {}

bool UI::show_detail_pane() const {
    return term_.cols() >= 60;
}

int UI::list_width() const {
    if (!show_detail_pane()) return term_.cols();
    return std::max(20, static_cast<int>(term_.cols() * 0.4));
}

int UI::detail_width() const {
    if (!show_detail_pane()) return 0;
    return term_.cols() - list_width() - 1;
}

int UI::content_height() const {
    return term_.rows() - 4;
}

void UI::ensure_visible() {
    int h = content_height();
    if (h <= 0) return;
    if (selected < list_scroll) list_scroll = selected;
    if (selected >= list_scroll + h) list_scroll = selected - h + 1;
    if (list_scroll < 0) list_scroll = 0;
}

/* renders full TUI frame */
void UI::draw(const std::vector<PackageInfo>& packages) {
    term_.clear();
    term_.hide_cursor();
    draw_search_bar();
    draw_borders();
    draw_package_list(packages);
    draw_detail_pane(packages);
    draw_status_bar(packages);

    if (focus == Focus::SearchBar) {
        term_.show_cursor();
        term_.move_to(0, 13 + search_cursor);
    }

    term_.flush();
}

void UI::draw_search_bar() {
    int w = term_.cols();
    term_.move_to(0, 0);

    if (focus == Focus::SearchBar) {
        term_.write(Terminal::bold());
        term_.write(accent_fg());
    } else {
        term_.write(Terminal::dim());
    }
    term_.write(" [/] Search: ");
    term_.write(Terminal::reset());
    term_.write_truncated(search_text, w - 14);

    const char* tab_label;
    if (filter_installed)     tab_label = " [Installed] ";
    else if (filter_updates)  tab_label = " [Updates] ";
    else if (show_aur)        tab_label = " [AUR] ";
    else                      tab_label = " [Repos] ";

    int tab_len = 0;
    for (const char* p = tab_label; *p; ++p) ++tab_len;
    int tab_x = w - tab_len - 1;
    if (tab_x > 14 + static_cast<int>(search_text.size())) {
        term_.move_to(0, tab_x);
        term_.write(Terminal::dim());
        term_.write(tab_label);
    }
    term_.write(Terminal::reset());
}

void UI::draw_borders() {
    int w = term_.cols();
    int lw = list_width();

    term_.move_to(1, 0);
    term_.write(Terminal::dim());
    for (int i = 0; i < w - 1; ++i) term_.write("\u2500");

    if (show_detail_pane()) {
        int start_row = 2;
        int end_row = term_.rows() - 2;
        for (int r = start_row; r <= end_row; ++r) {
            term_.move_to(r, lw);
            term_.write("\u2502");
        }
    }

    term_.move_to(term_.rows() - 2, 0);
    for (int i = 0; i < w - 1; ++i) term_.write("\u2500");

    if (show_detail_pane()) {
        term_.move_to(1, lw);
        term_.write("\u252c");
        term_.move_to(term_.rows() - 2, lw);
        term_.write("\u2534");
    }

    term_.write(Terminal::reset());
}

void UI::draw_package_list(const std::vector<PackageInfo>& packages) {
    int lw = list_width();
    int h = content_height();
    int start_row = 2;
    int total = static_cast<int>(packages.size());

    for (int i = 0; i < h; ++i) {
        int idx = list_scroll + i;
        int row = start_row + i;
        term_.move_to(row, 0);

        if (idx >= total) {
            term_.write(Terminal::dim());
            term_.write("   ~");
            term_.write(Terminal::reset());
            continue;
        }

        const auto& pkg = packages[idx];
        bool is_selected = (idx == selected && focus != Focus::SearchBar);

        if (is_selected) {
            term_.write(accent_fg());
            term_.write(Terminal::reverse_video());
            term_.write(Terminal::bold());
            term_.write(std::string(lw - 1, ' '));
            term_.move_to(row, 0);
            term_.write(accent_fg());
            term_.write(Terminal::reverse_video());
            term_.write(Terminal::bold());
        }

        term_.write(is_selected ? " > " : "   ");

        term_.write(pkg.source == PackageSource::AUR ? accent_fg() : color_fg(C::Blue));

        std::string repo_name = pkg.repo;
        repo_name += '/';
        repo_name += pkg.name;
        int name_max = lw - 4;
        if (pkg.installed) name_max -= 7;
        int repo_max = name_max - 3;
        term_.write_truncated(repo_name, repo_max);

        int name_used = std::min(static_cast<int>(repo_name.size()), repo_max);
        int ver_space = name_max - 3 - name_used;
        if (ver_space > 2) {
            term_.write(" ");
            term_.write(is_selected ? "" : accent_fg());
            term_.write(Terminal::dim());
            term_.write_truncated(pkg.version, ver_space);
        }

        if (pkg.installed) {
            term_.move_to(row, lw - 7);
            term_.write(color_fg(C::Green));
            term_.write(" [inst]");
        }

        term_.write(Terminal::reset());
    }
}

void UI::rebuild_detail_lines(const std::vector<PackageInfo>& packages) {
    detail_lines_.clear();
    detail_cache_idx_ = selected;

    if (selected < 0 || selected >= static_cast<int>(packages.size())) return;

    const auto& pkg = packages[selected];
    int dw = detail_width();
    if (dw < 10) return;

    int label_width = std::min(16, dw / 3);
    if (label_width < 4) label_width = 4;
    int label_col = label_width + 3;
    if (label_col >= dw) label_col = dw - 1;
    int value_width = dw - label_col;
    if (value_width < 1) value_width = 1;

    auto join = [](const std::vector<std::string>& v) {
        std::string r;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) r += ", ";
            r += v[i];
        }
        return r;
    };

    auto add_field = [&](const char* label, const std::string& value) {
        if (static_cast<int>(value.size()) <= value_width) {
            detail_lines_.push_back(std::string(label) + "|" + value);
        } else {
            detail_lines_.push_back(std::string(label) + "|" + value.substr(0, value_width));
            int pos = value_width;
            while (pos < static_cast<int>(value.size())) {
                int chunk = std::min(value_width, static_cast<int>(value.size()) - pos);
                detail_lines_.push_back("|" + value.substr(pos, chunk));
                pos += chunk;
            }
        }
    };

    add_field("Name", pkg.name);
    add_field("Version", pkg.version);
    if (pkg.installed && !pkg.installed_version.empty() && pkg.installed_version != pkg.version)
        add_field("Installed", pkg.installed_version);
    add_field("Repository", pkg.repo);
    add_field("Description", pkg.description);
    if (!pkg.url.empty())         add_field("URL", pkg.url);
    if (!pkg.arch.empty())        add_field("Architecture", pkg.arch);
    if (!pkg.licenses.empty())    add_field("Licenses", join(pkg.licenses));
    if (!pkg.groups.empty())      add_field("Groups", join(pkg.groups));
    if (!pkg.depends.empty())     add_field("Depends On", join(pkg.depends));
    if (!pkg.optdepends.empty())  add_field("Optional Deps", join(pkg.optdepends));
    if (!pkg.makedepends.empty()) add_field("Make Deps", join(pkg.makedepends));
    if (!pkg.provides.empty())    add_field("Provides", join(pkg.provides));
    if (!pkg.conflicts.empty())   add_field("Conflicts", join(pkg.conflicts));
    if (pkg.download_size > 0)    add_field("Download Size", format_size(pkg.download_size));
    if (pkg.install_size > 0)     add_field("Installed Size", format_size(pkg.install_size));
    if (pkg.build_date > 0)       add_field("Build Date", format_date(pkg.build_date));
    if (pkg.install_date > 0)     add_field("Install Date", format_date(pkg.install_date));
    if (!pkg.packager.empty())    add_field("Packager", pkg.packager);

    if (pkg.source == PackageSource::AUR) {
        add_field("AUR Votes", std::to_string(pkg.aur_votes));
        if (!pkg.aur_maintainer.empty())
            add_field("Maintainer", pkg.aur_maintainer);
        if (pkg.aur_out_of_date)
            add_field("Status", "Out of date!");
    }
}

void UI::draw_detail_pane(const std::vector<PackageInfo>& packages) {
    if (!show_detail_pane()) return;
    int lw = list_width() + 1;
    int dw = detail_width();
    int h = content_height();
    int start_row = 2;

    if (selected < 0 || selected >= static_cast<int>(packages.size())) {
        term_.move_to(start_row, lw);
        term_.write(Terminal::dim());
        term_.write("No package selected");
        term_.write(Terminal::reset());
        return;
    }

    if (dw < 10) return;

    if (selected != detail_cache_idx_)
        rebuild_detail_lines(packages);

    int label_width = std::min(16, dw / 3);
    if (label_width < 4) label_width = 4;
    int label_col = label_width + 3;
    if (label_col >= dw) label_col = dw - 1;

    int total_lines = static_cast<int>(detail_lines_.size());
    for (int i = 0; i < h && (detail_scroll + i) < total_lines; ++i) {
        int row = start_row + i;
        const std::string& line = detail_lines_[detail_scroll + i];

        term_.move_to(row, lw);

        size_t sep = line.find('|');
        if (sep != std::string::npos && sep > 0) {
            std::string padded_label = line.substr(0, sep);
            while (static_cast<int>(padded_label.size()) < label_width)
                padded_label += ' ';
            if (static_cast<int>(padded_label.size()) > label_width)
                padded_label.resize(label_width);

            term_.write(Terminal::bold());
            term_.write(accent_fg());
            term_.write(" ");
            term_.write_truncated(padded_label + ": ", dw - 1);
            term_.write(Terminal::reset());

            int label_used = 1 + label_width + 2;
            int remaining = dw - label_used;
            if (remaining > 0) {
                std::string value = line.substr(sep + 1);
                term_.write_truncated(value, remaining);
            }
        } else if (sep == 0) {
            term_.move_to(row, lw + label_col);
            term_.write_truncated(line.substr(1), dw - label_col);
        }
        term_.write(Terminal::reset());
    }

    if (total_lines > h) {
        int pct = total_lines > 0 ? (detail_scroll * 100) / total_lines : 0;
        char indicator[16];
        int ilen = snprintf(indicator, sizeof(indicator), " [%d%%]", pct);
        term_.move_to(start_row, lw + dw - ilen - 1);
        term_.write(Terminal::dim());
        term_.write(indicator);
        term_.write(Terminal::reset());
    }
}

void UI::draw_status_bar(const std::vector<PackageInfo>& packages) {
    int w = term_.cols();
    int row = term_.rows() - 1;
    term_.move_to(row, 0);

    if (progress.active) {
        term_.write(" ");
        term_.write(progress.label);
        term_.write(" ");
        int bar_width = w - static_cast<int>(progress.label.size()) - 12;
        if (bar_width > 10) {
            term_.write("[");
            int filled = static_cast<int>(progress.fraction * bar_width);
            term_.write(color_fg(C::Green));
            for (int i = 0; i < bar_width; ++i) {
                term_.write(i < filled ? "\u2588" : "\u2591");
            }
            term_.write(Terminal::reset());
            term_.write("] ");
            char pct[8];
            snprintf(pct, sizeof(pct), "%3d%%", static_cast<int>(progress.fraction * 100));
            term_.write(pct);
        }
    } else if (!status_message.empty()) {
        term_.write(" ");
        term_.write(accent_fg());
        term_.write(status_message);
    } else {
        static constexpr struct { const char* key; const char* label; } hints[] = {
            {"[i]", "nstall "},  {"[r]", "emove "},  {"[d]", "owngrade "},
            {"[u]", "pgrade "}, {"[s]", "ort "}, {"[a]", "ur upgrade "}, {"[c]", "ache clear "}, {"[/]", "search "}, {"[q]", "uit"},
        };
        term_.write(" ");
        for (const auto& h : hints) {
            term_.write(Terminal::bold());
            term_.write(accent_fg());
            term_.write(h.key);
            term_.write(Terminal::reset());
            term_.write(Terminal::dim());
            term_.write(h.label);
            term_.write(Terminal::reset());
        }

        char count[32];
        int clen;
        if (!packages.empty())
            clen = snprintf(count, sizeof(count), "Pkgs: %d/%d", selected + 1, static_cast<int>(packages.size()));
        else
            clen = snprintf(count, sizeof(count), "Pkgs: 0/0");
        int count_x = w - clen - 2;
        if (count_x > 40) {
            term_.move_to(row, count_x);
            term_.write(Terminal::reset());
            term_.write(Terminal::dim());
            term_.write(count);
        }
    }

    term_.write(Terminal::reset());
}

/* modal yes/no confirmation dialog */
bool UI::draw_confirm_dialog(const std::string& title, const std::vector<std::string>& lines) {
    int w = std::min(60, term_.cols() - 4);
    int h = static_cast<int>(lines.size()) + 4;
    int start_col = (term_.cols() - w) / 2;
    int start_row = (term_.rows() - h) / 2;

    term_.write(Terminal::reset());

    for (int r = 0; r < h; ++r) {
        term_.move_to(start_row + r, start_col);
        if (r == 0 || r == h - 1) {
            term_.write(r == 0 ? "\u250c" : "\u2514");
            for (int c = 1; c < w - 1; ++c) term_.write("\u2500");
            term_.write(r == 0 ? "\u2510" : "\u2518");
        } else {
            term_.write("\u2502");
            for (int c = 1; c < w - 1; ++c) term_.write(" ");
            term_.write("\u2502");
        }
    }

    term_.move_to(start_row + 1, start_col + 2);
    term_.write(Terminal::bold());
    term_.write(accent_fg());
    term_.write_truncated(title, w - 4);
    term_.write(Terminal::reset());

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        term_.move_to(start_row + 2 + i, start_col + 2);
        term_.write_truncated(lines[i], w - 4);
    }

    bool selected_yes = false;
    Input input;

    auto draw_buttons = [&]() {
        int btn_row = start_row + h - 2;
        term_.move_to(btn_row, start_col + 1);
        for (int c = 1; c < w - 1; ++c) term_.write(" ");
        term_.move_to(btn_row, start_col + 2);

        if (selected_yes) {
            term_.write(accent_fg());
            term_.write(Terminal::bold());
            term_.write(Terminal::reverse_video());
        } else {
            term_.write(Terminal::dim());
        }
        term_.write(" yes ");
        term_.write(Terminal::reset());
        term_.write(" ");
        if (!selected_yes) {
            term_.write(accent_fg());
            term_.write(Terminal::bold());
            term_.write(Terminal::reverse_video());
        } else {
            term_.write(Terminal::dim());
        }
        term_.write(" no ");
        term_.write(Terminal::reset());
        term_.hide_cursor();
        term_.flush();
    };

    draw_buttons();

    while (true) {
        auto ev = input.read_key();
        if (ev.key == Key::Left || ev.key == Key::Right ||
            ev.key == Key::Tab ||
            (ev.key == Key::Char && (ev.ch == 'h' || ev.ch == 'l'))) {
            selected_yes = !selected_yes;
            draw_buttons();
        } else if (ev.key == Key::Enter) {
            return selected_yes;
        } else if (ev.key == Key::Escape ||
                   (ev.key == Key::Char && (ev.ch == 'q' || ev.ch == 'n' || ev.ch == 'N'))) {
            return false;
        } else if (ev.key == Key::Char && (ev.ch == 'y' || ev.ch == 'Y')) {
            return true;
        }
    }
}

/* modal scrollable selection menu, returns chosen index or -1 */
int UI::draw_selection_dialog(const std::string& title, const std::vector<std::string>& options) {
    if (options.empty()) return -1;

    int max_items = std::min(static_cast<int>(options.size()), term_.rows() - 6);
    int w = std::min(60, term_.cols() - 4);
    int h = max_items + 4;
    int start_col = (term_.cols() - w) / 2;
    int start_row = (term_.rows() - h) / 2;
    int sel = 0;
    int scroll = 0;

    Input input;

    auto draw = [&]() {
        term_.write(Terminal::reset());

        for (int r = 0; r < h; ++r) {
            term_.move_to(start_row + r, start_col);
            if (r == 0 || r == h - 1) {
                term_.write(r == 0 ? "\u250c" : "\u2514");
                for (int c = 1; c < w - 1; ++c) term_.write("\u2500");
                term_.write(r == 0 ? "\u2510" : "\u2518");
            } else {
                term_.write("\u2502");
                for (int c = 1; c < w - 1; ++c) term_.write(" ");
                term_.write("\u2502");
            }
        }

        term_.move_to(start_row + 1, start_col + 3);
        term_.write(Terminal::bold());
        term_.write(accent_fg());
        term_.write_truncated(title, w - 5);
        term_.write(Terminal::reset());

        term_.move_to(start_row + 2, start_col + 1);
        term_.write(accent_fg());
        term_.write(Terminal::dim());
        for (int c = 1; c < w - 1; ++c) term_.write("\u2500");
        term_.write(Terminal::reset());

        for (int i = 0; i < max_items; ++i) {
            int idx = scroll + i;
            if (idx >= static_cast<int>(options.size())) break;
            int row = start_row + 3 + i;
            term_.move_to(row, start_col + 1);
            term_.write(std::string(w - 2, ' '));

            if (idx == sel) {
                term_.move_to(row, start_col + 1);
                term_.write(accent_fg());
                term_.write(Terminal::bold());
                term_.write(">");
                term_.move_to(row, start_col + 2);
                term_.write(accent_fg());
                term_.write(Terminal::bold());
                term_.write(Terminal::reverse_video());
                std::string padded = " " + options[idx];
                while (static_cast<int>(padded.size()) < w - 3) padded += ' ';
                term_.write_truncated(padded, w - 3);
            } else {
                term_.move_to(row, start_col + 3);
                term_.write(accent_fg());
                term_.write_truncated(options[idx], w - 5);
            }
            term_.write(Terminal::reset());
        }

        if (scroll > 0) {
            term_.move_to(start_row + 3, start_col + w - 3);
            term_.write(accent_fg());
            term_.write("\u25b2");
            term_.write(Terminal::reset());
        }
        if (scroll + max_items < static_cast<int>(options.size())) {
            term_.move_to(start_row + 2 + max_items, start_col + w - 3);
            term_.write(accent_fg());
            term_.write("\u25bc");
            term_.write(Terminal::reset());
        }

        term_.hide_cursor();
        term_.flush();
    };

    draw();

    while (true) {
        auto ev = input.read_key();
        if (ev.key == Key::Up || (ev.key == Key::Char && ev.ch == 'k')) {
            if (sel > 0) { sel--; if (sel < scroll) scroll = sel; }
            draw();
        } else if (ev.key == Key::Down || (ev.key == Key::Char && ev.ch == 'j')) {
            if (sel < static_cast<int>(options.size()) - 1) {
                sel++;
                if (sel >= scroll + max_items) scroll = sel - max_items + 1;
            }
            draw();
        } else if (ev.key == Key::Home || (ev.key == Key::Char && ev.ch == 'g')) {
            sel = 0; scroll = 0; draw();
        } else if (ev.key == Key::End || (ev.key == Key::Char && ev.ch == 'G')) {
            sel = static_cast<int>(options.size()) - 1;
            scroll = std::max(0, sel - max_items + 1);
            draw();
        } else if (ev.key == Key::Enter) {
            return sel;
        } else if (ev.key == Key::Escape || (ev.key == Key::Char && ev.ch == 'q')) {
            return -1;
        }
    }
}

void UI::draw_message(const std::string& title, const std::string& msg) {
    int w = std::min(60, term_.cols() - 4);
    int start_col = (term_.cols() - w) / 2;
    int start_row = (term_.rows() - 5) / 2;

    term_.write(Terminal::reset());

    for (int r = 0; r < 5; ++r) {
        term_.move_to(start_row + r, start_col);
        if (r == 0 || r == 4) {
            term_.write(r == 0 ? "\u250c" : "\u2514");
            for (int c = 1; c < w - 1; ++c) term_.write("\u2500");
            term_.write(r == 0 ? "\u2510" : "\u2518");
        } else {
            term_.write("\u2502");
            for (int c = 1; c < w - 1; ++c) term_.write(" ");
            term_.write("\u2502");
        }
    }

    term_.move_to(start_row + 1, start_col + 2);
    term_.write(Terminal::bold());
    term_.write(accent_fg());
    term_.write_truncated(title, w - 4);
    term_.write(Terminal::reset());

    term_.move_to(start_row + 2, start_col + 2);
    term_.write_truncated(msg, w - 4);

    term_.move_to(start_row + 3, start_col + 2);
    term_.write(accent_fg());
    term_.write("Press any key...");
    term_.write(Terminal::reset());
    term_.flush();

    Input input;
    for (;;) {
        auto ev = input.read_key_timeout(100);
        if (ev.key != Key::None) break;
    }
}

/* full-screen auto-scrolling build log with elapsed timer */
void UI::draw_build_log(const std::string& title, const std::vector<std::string>& log_lines,
                        bool finished, int elapsed_secs) {
    int w = term_.cols();
    int h = term_.rows();

    term_.clear();
    term_.hide_cursor();

    term_.move_to(0, 0);
    term_.write(Terminal::bold());
    term_.write(accent_fg());
    term_.write(" ");
    term_.write_truncated(title, w - 2);
    term_.write(Terminal::reset());

    term_.move_to(1, 0);
    term_.write(Terminal::dim());
    for (int i = 0; i < w - 1; ++i) term_.write("\u2500");
    term_.write(Terminal::reset());

    int log_height = h - 4;
    if (log_height < 1) log_height = 1;
    int total = static_cast<int>(log_lines.size());
    int start = std::max(0, total - log_height);

    for (int i = 0; i < log_height; ++i) {
        int idx = start + i;
        term_.move_to(2 + i, 0);
        if (idx < total) {
            term_.write(" ");
            term_.write(Terminal::dim());
            term_.write_truncated(log_lines[idx], w - 2);
            term_.write(Terminal::reset());
        }
    }

    term_.move_to(h - 2, 0);
    term_.write(Terminal::dim());
    for (int i = 0; i < w - 1; ++i) term_.write("\u2500");
    term_.write(Terminal::reset());

    term_.move_to(h - 1, 0);

    if (!finished) {
        static const char* spin[] = {
            "\u280b", "\u2819", "\u2839", "\u2838",
            "\u283c", "\u2834", "\u2826", "\u2827",
            "\u2807", "\u280f"
        };
        int sidx = elapsed_secs % 10;
        term_.write(" ");
        term_.write(accent_fg());
        term_.write(spin[sidx]);
        term_.write(Terminal::reset());
        term_.write(" Building... ");
    } else {
        term_.write(" ");
        term_.write(color_fg(Terminal::Green));
        term_.write("\u2714");
        term_.write(Terminal::reset());
        term_.write(" Done ");
    }

    int mins = elapsed_secs / 60;
    int secs = elapsed_secs % 60;
    char elapsed[32];
    if (mins > 0)
        snprintf(elapsed, sizeof(elapsed), "[%dm %02ds]", mins, secs);
    else
        snprintf(elapsed, sizeof(elapsed), "[%ds]", secs);
    term_.write(Terminal::dim());
    term_.write(elapsed);
    term_.write(Terminal::reset());

    char lcount[32];
    int lclen = snprintf(lcount, sizeof(lcount), "%d lines", total);
    int lcount_x = w - lclen - 2;
    if (lcount_x > 30) {
        term_.move_to(h - 1, lcount_x);
        term_.write(Terminal::dim());
        term_.write(lcount);
        term_.write(Terminal::reset());
    }

    term_.flush();
}

/* scrollable PKGBUILD viewer with diff toggle */
bool UI::draw_pkgbuild_review(const std::string& pkg_name, const std::string& content,
                              const std::string& old_content) {
    auto lines = split_lines(content);

    bool has_diff = !old_content.empty();
    std::vector<DiffLine> diff_lines;
    if (has_diff) {
        auto old_lines = split_lines(old_content);
        diff_lines = compute_diff(old_lines, lines);
    }

    bool show_diff = has_diff;
    int scroll = 0;

    Input input;

    auto draw = [&]() {
        int w = term_.cols();
        int h = term_.rows();
        int view_height = h - 4;
        if (view_height < 1) view_height = 1;

        int total;
        if (show_diff)
            total = static_cast<int>(diff_lines.size());
        else
            total = static_cast<int>(lines.size());
        int max_scroll = std::max(0, total - view_height);
        if (scroll > max_scroll) scroll = max_scroll;

        char nrbuf[16];
        int gutter = snprintf(nrbuf, sizeof(nrbuf), "%d", total);
        if (gutter < 3) gutter = 3;
        int gutter_col = gutter + 3;

        term_.clear();
        term_.hide_cursor();

        term_.move_to(0, 0);
        term_.write(Terminal::bold());
        term_.write(accent_fg());
        term_.write(" Review PKGBUILD: ");
        std::string title_suffix = pkg_name;
        if (has_diff)
            title_suffix += show_diff ? " (diff)" : " (full)";
        term_.write_truncated(title_suffix, w - 20);
        term_.write(Terminal::reset());

        term_.move_to(1, 0);
        term_.write(Terminal::dim());
        for (int i = 0; i < w - 1; ++i) term_.write("\u2500");
        term_.write(Terminal::reset());

        for (int i = 0; i < view_height; ++i) {
            int idx = scroll + i;
            term_.move_to(2 + i, 0);

            if (show_diff && idx < total) {
                const auto& dl = diff_lines[idx];
                if (dl.tag == '+')
                    term_.write(color_fg(C::Green));
                else if (dl.tag == '-')
                    term_.write(color_fg(C::Red));
                else
                    term_.write(Terminal::dim());
                snprintf(nrbuf, sizeof(nrbuf), " %*d %c ", gutter, idx + 1, dl.tag);
                term_.write(nrbuf);
                term_.write(Terminal::reset());

                int content_width = w - gutter_col - 1;
                if (content_width > 0) {
                    if (dl.tag == '+')
                        term_.write(color_fg(C::Green));
                    else if (dl.tag == '-')
                        term_.write(color_fg(C::Red));
                    term_.write_truncated(dl.text, content_width);
                    term_.write(Terminal::reset());
                }
            } else if (!show_diff && idx < total) {
                term_.write(Terminal::dim());
                snprintf(nrbuf, sizeof(nrbuf), " %*d \u2502 ", gutter, idx + 1);
                term_.write(nrbuf);
                term_.write(Terminal::reset());

                int content_width = w - gutter_col - 1;
                if (content_width > 0)
                    term_.write_truncated(lines[idx], content_width);
            }
        }

        term_.move_to(h - 2, 0);
        term_.write(Terminal::dim());
        for (int i = 0; i < w - 1; ++i) term_.write("\u2500");
        term_.write(Terminal::reset());

        term_.move_to(h - 1, 0);
        term_.write(" ");
        term_.write(Terminal::bold());
        term_.write(accent_fg());
        term_.write("[a]");
        term_.write(Terminal::reset());
        term_.write(Terminal::dim());
        term_.write("ccept ");
        term_.write(Terminal::reset());
        term_.write(Terminal::bold());
        term_.write(accent_fg());
        term_.write("[q]");
        term_.write(Terminal::reset());
        term_.write(Terminal::dim());
        term_.write("uit ");
        term_.write(Terminal::reset());

        if (has_diff) {
            term_.write(Terminal::bold());
            term_.write(accent_fg());
            term_.write("[d]");
            term_.write(Terminal::reset());
            term_.write(Terminal::dim());
            term_.write(show_diff ? "full " : "diff ");
            term_.write(Terminal::reset());
        }

        term_.write(Terminal::dim());
        term_.write(" j/k/PgUp/PgDn scroll");
        term_.write(Terminal::reset());

        if (total > view_height) {
            int pct = max_scroll > 0 ? (scroll * 100) / max_scroll : 100;
            char ind[16];
            int ilen = snprintf(ind, sizeof(ind), " [%d%%]", pct);
            int ix = w - ilen - 2;
            if (ix > 40) {
                term_.move_to(h - 1, ix);
                term_.write(Terminal::dim());
                term_.write(ind);
                term_.write(Terminal::reset());
            }
        }

        term_.flush();
    };

    draw();

    while (true) {
        auto ev = input.read_key();

        if (ev.key == Key::Char) {
            if (ev.ch == 'a' || ev.ch == 'y') return true;
            if (ev.ch == 'q' || ev.ch == 'n') return false;
            if (ev.ch == 'd' && has_diff) { show_diff = !show_diff; scroll = 0; draw(); }
            if (ev.ch == 'j') { scroll++; draw(); }
            if (ev.ch == 'k') { if (scroll > 0) scroll--; draw(); }
            if (ev.ch == 'g') { scroll = 0; draw(); }
            if (ev.ch == 'G') { scroll = INT_MAX; draw(); }
        } else if (ev.key == Key::Down) {
            scroll++; draw();
        } else if (ev.key == Key::Up) {
            if (scroll > 0) { scroll--; draw(); }
        } else if (ev.key == Key::PageDown) {
            int h = term_.rows();
            int view_height = h - 4;
            if (view_height < 1) view_height = 1;
            scroll += view_height;
            draw();
        } else if (ev.key == Key::PageUp) {
            int h = term_.rows();
            int view_height = h - 4;
            if (view_height < 1) view_height = 1;
            scroll = std::max(0, scroll - view_height);
            draw();
        } else if (ev.key == Key::Home) {
            scroll = 0; draw();
        } else if (ev.key == Key::End) {
            scroll = INT_MAX; draw();
        } else if (ev.key == Key::Escape) {
            return false;
        } else if (ev.key == Key::Enter) {
            return true;
        }
    }
}

const char* UI::accent_fg() const {
    if (!accent_code.empty()) return accent_code.c_str();
    if (color_disabled) return "";
    return Terminal::fg(C::Cyan);
}

const char* UI::color_fg(Terminal::Color c) const {
    if (color_disabled) return "";
    return Terminal::fg(c);
}

}
