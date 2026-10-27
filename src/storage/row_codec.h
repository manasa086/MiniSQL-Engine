#pragma once
#include <string>

#include "common/value.h"

// Int -> 8 bytes; Text -> u16 length + bytes. Throws on schema mismatch.
std::string serializeRow(const Row& row, const Schema& schema);
Row deserializeRow(const char* data, size_t len, const Schema& schema);
