#include "detector.hpp"
#include <cassert>
#include <iostream>

using liveac::PlayerDetector;
using liveac::Sample;

static Sample sample(double t, std::uint64_t cmd, float pitch, float yaw,
                     bool ground, std::uint32_t buttons = 0) {
    Sample s{}; s.time=t; s.command_number=cmd; s.pitch=pitch; s.yaw=yaw;
    s.on_ground=ground; s.alive=true; s.buttons=buttons; return s;
}

int main() {
    constexpr std::uint32_t ATTACK = 1u;
    constexpr std::uint32_t JUMP = 2u;

    // UDS AIM TYPE 5 adaptation: establish baseline, then repeated tiny steps + attack.
    {
        PlayerDetector d;
        double t=0; std::uint64_t c=0; float yaw=0;
        for (int i=0;i<20;i++) { yaw += 0.02f; d.push(sample(t+=0.01, ++c, 0, yaw, true)); }
        for (int i=0;i<4;i++) { yaw += 0.0005f; d.push(sample(t+=0.01, ++c, 0, yaw, true)); }
        auto ev = d.push(sample(t+=0.01, ++c, 0, yaw, true, ATTACK));
        bool found=false; for (auto &e:ev) if(e.type=="UDS_AIM_TYPE_5_ADAPTED") found=true;
        assert(found);
    }

    // UDS IDEALJUMP adaptation: >10 airborne frames, land, immediate jump; repeat 12x.
    {
        PlayerDetector d; double t=0; std::uint64_t c=0;
        d.push(sample(t+=0.01, ++c, 0, 0, true));
        bool found=false;
        for(int chain=0; chain<12; ++chain) {
            for(int i=0;i<11;i++) d.push(sample(t+=0.01, ++c, 0, 0, false));
            d.push(sample(t+=0.01, ++c, 0, 0, true));
            auto ev=d.push(sample(t+=0.01, ++c, 0, 0, false, JUMP));
            for(auto &e:ev) if(e.type=="UDS_IDEALJUMP_ADAPTED") found=true;
        }
        assert(found);
    }

    // Autoattack-style repeated command gaps.
    {
        PlayerDetector d; double t=0; std::uint64_t c=0; bool found=false;
        d.push(sample(t+=0.01, ++c, 0,0,true));
        for(int n=0;n<6;n++) {
            for(int i=0;i<2;i++) d.push(sample(t+=0.01, ++c,0,0,true));
            auto ev=d.push(sample(t+=0.01, ++c,0,0,true,ATTACK));
            for(auto &e:ev) if(e.type=="UDS_AUTOATTACK_ADAPTED") found=true;
            d.push(sample(t+=0.01, ++c,0,0,true));
        }
        assert(found);
    }
    std::cout << "detector tests passed\n";
}
