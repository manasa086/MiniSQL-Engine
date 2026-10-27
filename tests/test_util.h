#pragma once
#include <filesystem>
#include <iostream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } \
    } while (0)

// Fresh empty directory under the system temp dir, removed on destruction.
struct TempDir {
    std::string path;
    explicit TempDir(const std::string& tag)
        : path((std::filesystem::temp_directory_path() / ("minisql_test_" + tag)).string()) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};
