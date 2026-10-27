#include "parser/lexer.h"

#include <cctype>
#include <stdexcept>

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        if (std::isspace(c)) { ++i; continue; }
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '.')) ++j;
            out.push_back({Token::Ident, s.substr(i, j - i)});
            i = j;
        } else if (std::isdigit(c) || (c == '-' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i + 1;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
            out.push_back({Token::Number, s.substr(i, j - i)});
            i = j;
        } else if (c == '\'') {
            size_t j = s.find('\'', i + 1);
            if (j == std::string::npos) throw std::runtime_error("unterminated string literal");
            out.push_back({Token::String, s.substr(i + 1, j - i - 1)});
            i = j + 1;
        } else {
            std::string two = s.substr(i, 2);
            if (two == "!=" || two == "<>" || two == "<=" || two == ">=") {
                out.push_back({Token::Symbol, two == "<>" ? "!=" : two});
                i += 2;
            } else if (std::string(",*()=<>;").find(c) != std::string::npos) {
                out.push_back({Token::Symbol, std::string(1, c)});
                ++i;
            } else {
                throw std::runtime_error(std::string("unexpected character: ") + (char)c);
            }
        }
    }
    out.push_back({Token::End, ""});
    return out;
}
