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

/// 日志上限。超过就丢掉前半段（而不是停止记录）：
/// 出问题时最有价值的是最后几行，静默停止会让诊断反而失效。
constexpr size_t MAX_LOG_BYTES = 24 * 1024;

// UI 线程和下载工作线程都会写日志。
// libnx 的 IPC 会话不是并发安全的，而且内存里的缓冲区是 std::string ——
// 两个线程同时改它就是数据竞争（真机表现：随机死机）。
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
    {
        // 只保留最近的：清空并留一行说明
        char mark[96];
        std::snprintf(mark, sizeof(mark), "===== 前文过长已丢弃（序号 %d 之后继续）=====\n", g_sequence);
        g_buffer.assign(mark);
    }

    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "[%03d] ", ++g_sequence);

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

void ui(const std::string& text)
{
    line("[UI] " + text);
}

void uif(const char* format, ...)
{
    if (!g_enabled)
        return;

    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    ui(std::string(buf));
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
