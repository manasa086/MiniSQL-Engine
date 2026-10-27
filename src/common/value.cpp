#include "common/value.h"

#include <stdexcept>

int Schema::indexOf(const std::string& name) const {
    int found = -1;
    for (size_t i = 0; i < cols.size(); ++i) {
        const Column& c = cols[i];
        if (c.name == name || (!c.table.empty() && c.table + "." + c.name == name)) {
            if (found >= 0) throw std::runtime_error("ambiguous column: " + name);
            found = static_cast<int>(i);
        }
    }
    return found;
}

std::string toString(const Value& v) {
    if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
    return std::get<std::string>(v);
}

int compare(const Value& a, const Value& b) {
    if (a.index() != b.index()) throw std::runtime_error("type mismatch in comparison");
    if (auto* x = std::get_if<int64_t>(&a)) {
        int64_t y = std::get<int64_t>(b);
        return *x < y ? -1 : (*x > y ? 1 : 0);
    }
    int c = std::get<std::string>(a).compare(std::get<std::string>(b));
    return (c > 0) - (c < 0);
}
