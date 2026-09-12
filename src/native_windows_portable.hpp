#pragma once

#if defined(_WIN32)
#include <filesystem>

namespace bumble::portable {
bool prepare(const std::filesystem::path& data_root, bool diagnostics);
}
#endif
