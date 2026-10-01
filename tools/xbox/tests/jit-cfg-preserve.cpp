// SPDX-FileCopyrightText: Copyright 2026 eden-xbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
__declspec(noinline) int invoke(int (*fn)()) { return fn(); }
int main() {
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));
    if (!code) return 1;
    const unsigned char body[]{0xb8,42,0,0,0,0xc3};
    std::memcpy(code, body, sizeof(body));
    DWORD old{};
    if (!VirtualProtectFromApp(code,4096,PAGE_EXECUTE_READ,&old)) return 2;
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    if (invoke(reinterpret_cast<int(*)()>(code))!=42) return 3;
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY policy{};
    GetProcessMitigationPolicy(GetCurrentProcess(),ProcessControlFlowGuardPolicy,&policy,sizeof(policy));
    std::printf("CFG enabled: %u\n",policy.EnableControlFlowGuard);
    if (!policy.EnableControlFlowGuard) return 7; // Build this test with /guard:cf on both cl and link.
    for (int pass=0;pass<4;++pass) {
        auto start=std::chrono::steady_clock::now();
        const DWORD flags=PAGE_EXECUTE_READ | ((pass%2) ? PAGE_TARGETS_NO_UPDATE : 0);
        for(int i=0;i<20000;++i) {
            if(!VirtualProtectFromApp(code,4096,PAGE_READWRITE,&old)) return 4;
            std::memcpy(code,body,sizeof(body));
            // New entry within a previously initialized page (as when appending a block).
            std::memcpy(code+16,body,sizeof(body));
            if(!VirtualProtectFromApp(code,4096,flags,&old)) { std::printf("RX failed %lu\n",GetLastError()); return 5; }
            FlushInstructionCache(GetCurrentProcess(),code,4096);
            if(invoke(reinterpret_cast<int(*)()>(code))!=42) return 6;
            if(invoke(reinterpret_cast<int(*)()>(code+16))!=42) return 6;
        }
        double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/20000;
        std::printf("pass %d preserve=%d: %.3f us/pair+flush+CFG call\n",pass,pass%2,us);
    }
    VirtualFree(code,0,MEM_RELEASE);
}
