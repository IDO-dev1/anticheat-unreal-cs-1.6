#include "detector.hpp"
#include <cassert>
#include <iostream>

using liveac::PlayerDetector;
using liveac::Sample;

static Sample sample(double t, std::uint64_t cmd, float pitch, float yaw,
                     bool ground, std::uint32_t buttons = 0,
                     bool visible = false, float error = 180.0f,
                     float previous_error = 180.0f, bool crosshair = false) {
    Sample s{}; s.time=t; s.command_number=cmd; s.pitch=pitch; s.yaw=yaw;
    s.on_ground=ground; s.alive=true; s.buttons=buttons;
    s.target_slot=visible?2:0; s.target_visible=visible; s.target_angle_error=error;
    s.previous_target_angle_error=previous_error; s.target_in_crosshair=crosshair;
    return s;
}

int main() {
    constexpr std::uint32_t ATTACK=1u, JUMP=2u;

    // Large turns without a visible target must not produce aim evidence.
    {
        PlayerDetector d; double t=0; std::uint64_t c=0; float yaw=0; bool found=false;
        for(int i=0;i<20;i++) {
            yaw += 45.0f;
            auto ev=d.push(sample(t+=0.02,++c,0,yaw,true,(i%2)?ATTACK:0));
            if(!ev.empty()) found=true;
        }
        assert(!found && d.score()==0.0f);
    }

    // Context-aware repeated target snaps + attack.
    {
        PlayerDetector d; double t=0; std::uint64_t c=0; float yaw=0; bool found=false;
        for(int n=0;n<4;n++) {
            d.push(sample(t+=0.02,++c,0,yaw,true,0,true,25.0f,25.0f));
            yaw += 25.0f;
            d.push(sample(t+=0.02,++c,0,yaw,true,0,true,1.0f,25.0f,true));
            auto ev=d.push(sample(t+=0.01,++c,0,yaw,true,ATTACK,true,0.8f,1.0f,true));
            for(auto &e:ev) if(e.type=="TARGET_AIM_SNAP") found=true;
            d.push(sample(t+=0.01,++c,0,yaw,true,0,true,1.0f,0.8f,true));
        }
        assert(found);
    }

    // UDS IDEALJUMP adaptation remains active.
    {
        PlayerDetector d; double t=0; std::uint64_t c=0; bool found=false;
        d.push(sample(t+=0.01,++c,0,0,true));
        for(int chain=0;chain<12;++chain) {
            for(int i=0;i<11;i++) d.push(sample(t+=0.01,++c,0,0,false));
            d.push(sample(t+=0.01,++c,0,0,true));
            auto ev=d.push(sample(t+=0.01,++c,0,0,false,JUMP));
            for(auto &e:ev) if(e.type=="UDS_IDEALJUMP_ADAPTED") found=true;
        }
        assert(found);
    }

    std::cout << "context detector tests passed\n";
}
