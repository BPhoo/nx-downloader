#include "logx.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "app_config.hpp"
#include "fsx.hpp"

namespace logx
{

namespace
{

/// 日志上限，超过就不再追加（防止无限增长把 SD 卡写爆）
constexpr size_t MAX_LOG_BYTES = 32 * 1024;

// UI 线程和下载工作线程都可能写日志。
// libnx 的 IPC 会话不是并发安全的，所以这里必须串行化
// ——否则两个线程同时对同一个 FsFileSystem 会话发请求就是数据竞争。
std::mutex g_mutex;

std::string g_buffer;
bool g_enabled = false;
int g_sequence = 0;

} // namespace

const char* path()
{
    return appcfg::LOG_FILE;
}

bool enabled()
{
    return g_enabled;
}

std::string result(Result rc)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(rc));
    return std::string(buf);
}

void line(const std::string& text)
{
    if (!g_enabled)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_buffer.size() >= MAX_LOG_BYTES)
        return;

    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "[%02d] ", ++g_sequence);

    g_buffer += prefix;
    g_buffer += text;
    g_buffer += '\n';

    // 整份重写：崩溃/死机时最后一行也已经在盘上
    fsx::writeWholeFile(appcfg::LOG_FILE, g_buffer);
}

void linef(const char* format, ...)
{
    if (!g_enabled)
        return;

    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    line(std::string(buf));
}

void open()
{
    g_buffer.clear();
    g_sequence = 0;
    g_enabled  = fsx::isDirectory(appcfg::PROJECT_DIR) || fsx::ensureDirectory(appcfg::PROJECT_DIR);

    if (!g_enabled)
        return;

    line("===== NX Downloader 启动日志 =====");
    linef("固件/环境: appletType=%d", static_cast<int>(appletGetAppletType()));
}

} // namespace logx
