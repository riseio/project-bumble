#pragma once

#include <filesystem>

namespace bumble::diagnostics {
void initialize(const std::filesystem::path& root, bool console);
void phase(const char* name) noexcept;
void end_startup() noexcept;
void finish(int result) noexcept;
void test_crash(const char* kind);
}
