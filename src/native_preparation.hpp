#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>

namespace bumble::first_run {
struct PreparationCancelled : std::runtime_error {
    PreparationCancelled() : std::runtime_error("Preparation cancelled") {}
};

struct PreparationProgress {
    struct Snapshot { std::string stage; uint32_t completed = 0, total = 0; };
    std::atomic_bool cancelled{false};
    void report(std::string stage, uint32_t completed = 0, uint32_t total = 0);
    void check() const;
    Snapshot snapshot();
private:
    std::mutex mutex;
    Snapshot value;
};

void run_preparation(const std::function<void(PreparationProgress&)>& work);
struct TextureChoice { bool generate = false; bool remember = false; };
TextureChoice choose_enhanced_textures();
}
