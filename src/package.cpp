#include "package.h"
#include <ctime>

namespace pmt {

std::string format_size(int64_t bytes) {
    if (bytes < 0) return "0 B";
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }
    char buf[64];
    if (unit == 0) {
        snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    }
    return buf;
}

std::string format_date(int64_t timestamp) {
    if (timestamp == 0) return "N/A";
    time_t t = static_cast<time_t>(timestamp);
    struct tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}
