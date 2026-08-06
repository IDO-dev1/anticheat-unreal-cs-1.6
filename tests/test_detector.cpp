#include "detector.hpp"
#include <cassert>
#include <iostream>

int main() {
    liveac::Config c;
    c.evidence_cooldown = 0.0f;
    liveac::PlayerDetector d(c);
    liveac::Sample a{1.000, 0, 0, 0, 0, 0, true, true};
    liveac::Sample b{1.020, 0, 30, 0, 0, 1, true, true};
    auto e = d.push(a);
    e = d.push(b);
    assert(!e.empty());
    assert(e.front().type == "AIM_SNAP");
    assert(d.score() > 0);
    std::cout << "detector test passed\n";
}
