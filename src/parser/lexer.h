#pragma once
#include <string>
#include <vector>

struct Token {
    enum Kind { Ident, Number, String, Symbol, End } kind;
    std::string text;
};

std::vector<Token> tokenize(const std::string& sql);
