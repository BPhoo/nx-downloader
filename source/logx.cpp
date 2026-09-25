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

/// 日志文件上限。超过就**轮转**一次（关掉重开、只保留最近一部分），
/// 既不无限增长，也不会静默停止记录 —— 出问题时最有价值的是最后几行。
constexpr size_t MAX_LOG_BYTES = 64 * 1024;

/// 轮转时保留尾部多少字节
constexpr size_t KEEP_ON_ROTATE = 8 * 1024;

// UI 线程和下载工作线程都会写日志。
// libnx 的 IPC 会话不是并发安全的，而且内存里的缓冲区是 std::string ——
// 两个线程同时改它就是数据竞争（真机表现：随机死机）。
std::mutex g_mutex;

std::string g_buffer;
bool g_enabled = false;
int g_sequence = 0;

/// 最近一次写盘是否失败（见 logx::writeFailed 的说明）
bool g_writeFailed = false;

//----------------------------------------------------------------------
// ★ 常驻打开的日志文件句柄 + 当前写入偏移。
//
//   为什么不再用 `writeWholeFile`（每次整份重写）：
//   那条路每写一行要「删文件 → 建文件 → 打开 → 写 → flush → 关」共 6 次 FS 操作，
//   启动阶段二十多行日志就会对 SD 卡打出上百次操作。而 Applet 模式（从相册启动）下
//   SD 卡是与父 applet / 系统共用的，这么打很可能把整机拖死（真机实测：日志停在
//   构造函数中间，屏幕卡住不动）。
//   改成「启动时打开一次，之后逐行追加」后，每行只剩 **1 次** 写操作。
//----------------------------------------------------------------------
FsFile g_file = {};
bool g_fileOpen = false;
s64 g_offset = 0;

/// 追加一段字节到日志文件（调用者必须已持 g_mutex）
bool writeBytesLocked(const char* data, size_t size)
{
    if (!g_fileOpen || size == 0)
        return false;

    // flush=true：真正落盘。宁可慢一点，也要保证崩溃/死机时最后一行在盘上。
    const Result rc = fsx::writeFileChunk(&g_file, g_offset, data, size, true);
    if (R_FAILED(rc))
        return false;

    g_offset += static_cast<s64>(size);
    return true;
}

/// 轮转：关掉重开，只保留最近一部分（调用者必须已持 g_mutex）
void rotateLocked()
{
    if (g_fileOpen)
    {
        fsx::flushAndCloseFile(&g_file);
        g_fileOpen = false;
    }

    std::string tail = (g_buffer.size() > KEEP_ON_ROTATE)
                           ? g_buffer.substr(g_buffer.size() - KEEP_ON_ROTATE)
                           : g_buffer;

    g_buffer = "===== 日志过长，已保留最近部分（序号 " + std::to_string(g_sequence) + " 之后继续）=====\n" + tail;

    if (fsx::openForAppend(appcfg::LOG_FILE, &g_file, &g_offset))
    {
        g_fileOpen = true;
        writeBytesLocked(g_buffer.data(), g_buffer.size());
    }
}

/// 真正追加一行（调用者必须已持 g_mutex）
void appendLineLocked(const std::string& text)
{
    if (!g_enabled)
        return;

    if (g_buffer.size() + text.size() + 32 > MAX_LOG_BYTES)
        rotateLocked();

    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "[%03d] ", ++g_sequence);

    std::string entry(prefix);
    entry += text;
    entry += '\n';

    g_buffer += entry;

    if (writeBytesLocked(entry.data(), entry.size()))
    {
        if (g_writeFailed)
        {
            g_writeFailed             = false;
            const std::string notice  = "[!] 日志写入已恢复（之前有内容丢失）\n";
            g_buffer += notice;
            writeBytesLocked(notice.data(), notice.size());
        }
        return;
    }

    // 写不进去：只标记一次（界面会显示）。
    // 这一行只留在内存里 —— 只要后续某次写成功，它也会一起落盘，
    // 于是「日志为什么变短」在日志里也有答案。
    if (!g_writeFailed)
    {
        g_writeFailed            = true;
        g_buffer += "[!] 日志写入 SD 卡失败：以上内容之后的日志可能都没能落盘\n";
    }
}

} // namespace

bool writeFailed()
{
    return g_writeFailed;
}

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
    appendLineLocked(text);
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
    std::lock_guard<std::mutex> lock(g_mutex);

    g_buffer.clear();
    g_sequence    = 0;
    g_writeFailed = false;
    g_offset      = 0;

    const bool dirOk = fsx::isDirectory(appcfg::PROJECT_DIR) || fsx::ensureDirectory(appcfg::PROJECT_DIR);
    if (!dirOk)
        return;

    // 打开（并清空）日志文件，之后一直复用这个句柄逐行追加
    if (!fsx::openForAppend(appcfg::LOG_FILE, &g_file, &g_offset))
        return;

    g_fileOpen = true;
    g_enabled  = true;

    appendLineLocked("===== NX Downloader 启动日志 =====");
    appendLineLocked("固件/环境: appletType=" + std::to_string(static_cast<int>(appletGetAppletType())));
}

void close()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_fileOpen)
    {
        fsx::flushAndCloseFile(&g_file);
        g_fileOpen = false;
    }

    g_enabled = false;
}

} // namespace logx
