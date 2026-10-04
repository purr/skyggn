#pragma once

#include <chrono>

namespace skyggn {

// the moment the time limit for one file runs out
struct deadline {
    std::chrono::steady_clock::time_point end;
    bool passed() const { return std::chrono::steady_clock::now() > end; }
};

}  // namespace skyggn
