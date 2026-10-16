// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "common/thread.h"
#include "common/windows/timer_resolution.h"
#include "common/windows/timed_wait_profile.h"
void AssertFatalImpl(){std::abort();} void AssertFailSoftImpl(){std::abort();}
void AssertFailedAt(const char*){std::abort();} void UnreachableAt(const char*){std::abort();}
namespace P=Common::Windows::TimedWaitProfile;
static void Clear(){for(auto& c:P::counters)c.store(0);}
static u64 Time(FILETIME f){return (u64{f.dwHighDateTime}<<32)|f.dwLowDateTime;}
static u64 Cpu(){FILETIME a,b,c,d;assert(GetThreadTimes(GetCurrentThread(),&a,&b,&c,&d));return Time(c)+Time(d);}
int main(){
    using namespace std::chrono_literals;
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    Common::SetPreciseTimerSleeps(true);
    P::enabled=false;P::ActivateThread();
    {Common::Event e; assert(!e.WaitFor(100us));}assert(P::Read().waits==0);
    P::enabled=true;
    {Common::Event e;assert(!e.WaitFor(100us));}assert(P::Read().waits==0); // not HostTiming
    P::ActivateThread();
    {Common::Event e;e.Set();assert(e.WaitFor(1s));}assert(P::Read().waits==1);
    Clear();
    Common::Event wake;std::atomic_bool started{};
    std::jthread waiter{[&]{P::ActivateThread();started=true;started.notify_one();assert(wake.WaitFor(1s));}};
    started.wait(false);wake.Set();waiter.join();assert(P::Read().waits==1);
    std::cout<<"PASS profiling disabled/unselected/enabled, early signal and cross-thread wake\n";
    for(bool precise:{false,true})for(u32 us:{100u,250u,1000u,16000u}){
        Common::SetPreciseTimerSleeps(precise);Clear();Common::Event e;
        const u32 count=us==16000?50:1000;
        std::vector<double> late;late.reserve(count);
        auto start=std::chrono::steady_clock::now();auto cpu=Cpu();
        for(u32 i=0;i<count;++i){auto t=std::chrono::steady_clock::now();assert(!e.WaitFor(std::chrono::microseconds{us}));late.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count()-us);}
        auto used=(Cpu()-cpu)/10000.0;auto wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        auto s=P::Read();assert(s.waits==count&&s.requested_ns==u64{count}*us*1000);
        std::sort(late.begin(),late.end());
        std::cout<<"precise="<<precise<<" wait_us="<<us<<" count="<<count<<" elapsed_ms="<<wall<<" CPU_ms="<<used
            <<" spin_ms="<<s.spin_ns/1e6<<" sleeps="<<s.sleeps<<" sleep_ms="<<s.sleep_ns/1e6
            <<" fallback="<<s.fallbacks<<" maxmargin_us="<<s.margin_max_ns/1e3
            <<" lateness_us p50="<<late[count/2]<<" p99="<<late[count*99/100]<<" max="<<late.back()<<"\n";
    }
    P::enabled=false;P::ActivateThread();
    std::cout<<"PASS actual Event timer/fallback: counters and deadlines retained; diagnostic, not gameplay FPS\n";
}
