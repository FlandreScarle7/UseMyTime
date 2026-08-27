/*
 * UseMyTime - MiniJson 实现（依赖 TimeHooks 的状态）
 */
#include "MiniJson.h"
#include "TimeHooks.h"
#include <cstdio>

namespace usemytime {

const MiniJson::Value MiniJson::s_empty;

std::string MiniJson::StatusResponse()
{
    const long long EPOCH_DIFF_MS = 11644473600000LL; // 1601 -> 1970

    long long realMs    = TimeHooks::GetRealNowMs();
    long long virtualMs = TimeHooks::GetVirtualNowMs();

    auto fmt = [EPOCH_DIFF_MS](long long ms) -> std::string {
        FILETIME ft;
        ULARGE_INTEGER v;
        v.QuadPart = ms * 10000LL;
        ft.dwLowDateTime  = v.LowPart;
        ft.dwHighDateTime = v.HighPart;
        FILETIME localFt = ft;
        SYSTEMTIME st;
        FileTimeToSystemTime(&localFt, &st);
        char buf[64];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
        return std::string(buf);
    };

    MiniJson r;
    r.SetBool("ok", true);
    r.SetNumber("offset_ms", static_cast<double>(TimeHooks::GetOffsetMs()));
    r.SetNumber("speed", TimeHooks::GetSpeed());
    r.SetNumber("enabled", TimeHooks::IsEnabled() ? 1 : 0);
    r.SetString("real_time", fmt(realMs));
    r.SetString("virtual_time", fmt(virtualMs));
    r.SetNumber("delta_ms", static_cast<double>(virtualMs - realMs));
    return r.Dump() + "\n";
}

} // namespace usemytime
