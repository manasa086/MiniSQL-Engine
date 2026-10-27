#include "common/value.h"

#include <stdexcept>

int Schema::indexOf(const std::string& name) const {
    for (size_t i = 0; i < cols.size(); ++i)
        if (cols[i].name == name) return static_cast<int>(i);
    return -1;
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
