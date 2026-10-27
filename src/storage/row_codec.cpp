#include "storage/row_codec.h"

#include <cstring>
#include <stdexcept>

std::string serializeRow(const Row& row, const Schema& schema) {
    if (row.size() != schema.cols.size()) throw std::runtime_error("row arity does not match schema");
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) {
        if (schema.cols[i].type == Type::Int) {
            auto* v = std::get_if<int64_t>(&row[i]);
            if (!v) throw std::runtime_error("expected INT for column " + schema.cols[i].name);
            out.append(reinterpret_cast<const char*>(v), sizeof(int64_t));
        } else {
            auto* s = std::get_if<std::string>(&row[i]);
            if (!s) throw std::runtime_error("expected TEXT for column " + schema.cols[i].name);
            if (s->size() > UINT16_MAX) throw std::runtime_error("text value too long");
            uint16_t n = static_cast<uint16_t>(s->size());
            out.append(reinterpret_cast<const char*>(&n), sizeof n);
            out.append(*s);
        }
    }
    return out;
}

Row deserializeRow(const char* data, size_t len, const Schema& schema) {
    Row row;
    size_t pos = 0;
    for (const auto& col : schema.cols) {
        if (col.type == Type::Int) {
            if (pos + sizeof(int64_t) > len) throw std::runtime_error("corrupt record");
            int64_t v;
            std::memcpy(&v, data + pos, sizeof v);
            pos += sizeof v;
            row.emplace_back(v);
        } else {
            uint16_t n;
            if (pos + sizeof n > len) throw std::runtime_error("corrupt record");
            std::memcpy(&n, data + pos, sizeof n);
            pos += sizeof n;
            if (pos + n > len) throw std::runtime_error("corrupt record");
            row.emplace_back(std::string(data + pos, n));
            pos += n;
        }
    }
    return row;
}
