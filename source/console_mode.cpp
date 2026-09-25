/*
    NX Downloader - 文字（控制台）模式

    为什么存在：
      Applet 模式（从相册启动）下 borealis 图形界面在真机上会整机死机
      （冻结点在「纯 CPU 的视图树构造」里，紧跟在一次 SD 写盘之后，见 BUILD.md §1）。
      而 Applet 模式本身又天生受限：内存小、无系统键盘、nifm 查询受限、SD 卡与系统共用。

      于是这条路径改成**纯 libnx 控制台**：不创建 GL 上下文、不用 nanovg / borealis / 字体。
      功能上保留「检查更新（看返回内容）/ 下载文件（带百分比）/ 重新读 url.txt」，
      足够在相册里把活干完。

    ★ 它同时是一次**决定性实验**：
      如果文字模式在 Applet 模式下活得好好的 → 问题出在 borealis/GL 那条栈上；
      如果它也卡死 → 说明是 Applet 环境本身（例如 SD 卡与系统争用），与图形无关。
      所以每个操作都会把「进行到哪一步」即时打到屏幕上 ——
      死机时屏幕会保留最后一帧，用户直接就能报出停在哪一行。

    ⚠️ 这里所有文案都用 ASCII：
       libnx 的 console 用内置位图字体，没有汉字字形，写中文只会显示成方块。
       日志文件里仍然是中文（那是给电脑看的），屏幕上不打印日志正文。
*/
#include "console_mode.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "app_config.hpp"
#include "downloader.hpp"
#include "fsx.hpp"
#include "logx.hpp"
#include "netx.hpp"

namespace consmode
{

namespace
{

void out(const std::string& text)
{
    std::printf("%s\n", text.c_str());
    consoleUpdate(nullptr);
}

/// 只留可打印 ASCII，汉字等一律折叠成一个 '?'（避免整行变成方块）
std::string ascii(const std::string& text, size_t maxLen = 76)
{
    std::string result;
    for (char c : text)
    {
        const unsigned char u = static_cast<unsigned char>(c);

        if (c == '\n' || c == '\r' || c == '\t')
            continue;

        if (u >= 0x20 && u < 0x7f)
        {
            result.push_back(c);
        }
        else if (result.empty() || result.back() != '?')
        {
            result.push_back('?');
        }

        if (result.size() >= maxLen)
            break;
    }

    return result;
}

std::string bytesText(long long bytes)
{
    char buf[64];

    if (bytes >= 1024LL * 1024LL)
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%lld B", bytes);

    return std::string(buf);
}

const char* stateName(Downloader::State s)
{
    switch (s)
    {
        case Downloader::State::Idle:
            return "idle";
        case Downloader::State::Running:
            return "running";
        case Downloader::State::Finished:
            return "finished";
        case Downloader::State::Failed:
            return "failed";
        case Downloader::State::Cancelled:
        default:
            return "cancelled";
    }
}

/// 等当前任务跑完，同时把进度打到屏幕上（每变化 5% 打一行）
void waitTask(Downloader* downloader, const char* what)
{
    int lastPercent = -10;
    int stall       = 0;

    out(std::string("  ") + what + " ...");

    while (true)
    {
        if (!appletMainLoop())
            return; // applet 被要求退出

        const Downloader::State state = downloader->state();

        if (state == Downloader::State::Running)
        {
            const int percent = downloader->percent();

            if (percent >= lastPercent + 5)
            {
                lastPercent = percent;

                const long long got = downloader->downloaded();
                const long long all = downloader->total();

                std::string line = "    " + std::to_string(percent) + "%  " + bytesText(got);
                if (all > 0)
                    line += " / " + bytesText(all);

                out(line);
            }

            svcSleepThread(100 * 1000 * 1000); // 100ms
            continue;
        }

        // 终态
        if (state == Downloader::State::Finished)
        {
            const std::string path = downloader->outputPath();
            out(std::string("  OK: ") + stateName(state) +
                (path.empty() ? std::string() : ("  ->  " + ascii(path))));
        }
        else
        {
            out(std::string("  ") + stateName(state) + ": " + ascii(downloader->errorText()));
        }

        // 状态可能还在 Idle（刚启动），给几拍缓冲
        if (state == Downloader::State::Idle && stall++ < 5)
        {
            svcSleepThread(100 * 1000 * 1000);
            continue;
        }

        return;
    }
}

/// 检查更新：把返回内容的前几行打到屏幕上（这就是「有没有新版本」的依据）
void checkUpdate(Downloader* downloader, const std::string& url)
{
    if (url.empty())
    {
        out("  no updata in url.txt");
        return;
    }

    if (!netx::ready() && !netx::init())
    {
        out("  network not ready (socket init failed)");
        return;
    }

    out(std::string("  fetching ") + ascii(url, 64));

    if (!downloader->startFetchText(url))
    {
        out("  cannot start (bad url?)");
        return;
    }

    waitTask(downloader, "fetching");

    if (downloader->state() != Downloader::State::Finished)
        return;

    const std::string body = downloader->bodyText();
    out("  HTTP " + std::to_string(downloader->httpStatus()) + " , " +
        std::to_string(body.size()) + " bytes:");

    // 逐行打印（最多 12 行）——返回内容一般就是版本号/更新说明
    size_t printed = 0;
    size_t start   = 0;
    while (start <= body.size() && printed < 12)
    {
        size_t nl = body.find('\n', start);
        if (nl == std::string::npos)
            nl = body.size();

        const std::string line = body.substr(start, nl - start);
        if (!line.empty())
        {
            out("    | " + ascii(line, 70));
            printed++;
        }

        if (nl >= body.size())
            break;
        start = nl + 1;
    }

    if (printed == 0)
        out("    (empty body)");
}

void downloadFile(Downloader* downloader, const std::string& url)
{
    if (url.empty())
    {
        out("  no download link in url.txt");
        return;
    }

    if (!netx::ready() && !netx::init())
    {
        out("  network not ready (socket init failed)");
        return;
    }

    std::string name = fsx::fileNameFromUrl(url);
    if (name.empty())
        name = "download.bin";

    // 控制台模式下固定存到项目文件夹，免得跟别的东西混在一起
    if (!fsx::ensureDirectory(appcfg::PROJECT_DIR))
    {
        out("  cannot create project dir");
        return;
    }

    out(std::string("  -> ") + ascii(appcfg::PROJECT_DIR) + "/" + ascii(name, 40));

    if (!downloader->start(url, appcfg::PROJECT_DIR))
    {
        out("  cannot start download");
        return;
    }

    waitTask(downloader, "downloading");
}

/// SD 卡 I/O 自检：照日志的写法（追加 + flush）连续写 40 次，把每次耗时打出来。
/// 这一步是专门用来验证「是不是 SD 卡 I/O 把整机拖死的」。
void ioTest()
{
    const std::string path = std::string(appcfg::PROJECT_DIR) + "/iotest.txt";

    out("  SD I/O self-test: 40 appends of 64B with flush");
    out(std::string("  file: ") + ascii(path));

    FsFile file = {};
    if (!fsx::openForAppend(path, &file))
    {
        out("  open failed -- abort");
        return;
    }

    s64 offset = 0;

    for (int i = 0; i < 40; i++)
    {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "line %02d ...................................................\n", i);

        const u64 len = static_cast<u64>(std::strlen(buf));
        const u64 t0  = svcGetSystemTick();

        const Result rc = fsx::writeFileChunk(&file, offset, buf, len, true);

        const double freq = 19200000.0; // Switch 的系统 tick 固定 19.2MHz
        const double ms   = static_cast<double>(svcGetSystemTick() - t0) * 1000.0 / freq;

        if (R_FAILED(rc))
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "  write %d/40 FAILED rc=0x%08X", i, static_cast<unsigned>(rc));
            out(msg);
            fsx::flushAndCloseFile(&file);
            return;
        }

        offset += static_cast<s64>(len);

        // 每 4 次报一行；单次超过 50ms 也单独报（说明 SD 卡开始卡了）
        if (i % 4 == 0 || ms > 50.0)
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "  write %d/40 ok (%.1f ms)", i, ms);
            out(msg);
        }
    }

    fsx::flushAndCloseFile(&file);

    const s64 size = fsx::fileSize(path);
    out("  done, file size = " + std::to_string(static_cast<long long>(size)) + " bytes");
    out(std::string("  delete = ") + (fsx::removeFile(path) ? "ok" : "FAILED"));
}

/// 重新读 url.txt（也可以手改 url.txt 后再按 X，不用退出程序）
bool reloadUrl(appcfg::UrlEntry* entry)
{
    std::string text;
    if (!fsx::readWholeFile(appcfg::URL_FILE, &text))
    {
        out("  cannot read url.txt");
        return false;
    }

    appcfg::UrlEntry fresh;
    if (!appcfg::parseUrlFile(text, &fresh))
    {
        out("  parse failed (expect: { updata: .. , download: .. , img: .. })");
        return false;
    }

    // 保留没写的那一项（parseUrlFile 不改未识别的项）
    if (!fresh.update.empty())
        entry->update = fresh.update;
    if (!fresh.download.empty())
        entry->download = fresh.download;

    return true;
}

void printMenu(const appcfg::UrlEntry& entry)
{
    out("");
    out("  updata   : " + (entry.update.empty() ? std::string("(empty)") : ascii(entry.update, 60)));
    out("  download : " + (entry.download.empty() ? std::string("(empty)") : ascii(entry.download, 60)));
    out("  img      : " + std::to_string(entry.images.size()) + " item(s), not shown here");
    out("");
    out("  [A] check update (show returned text)");
    out("  [B] download to project folder");
    out("  [X] reload url.txt");
    out("  [Y] SD card I/O self-test");
    out("  [+] exit");
    out("");
}

} // namespace

int run(Downloader* downloader, const appcfg::UrlEntry& entry)
{
    consoleInit(nullptr);
    consoleClear();

    out("==================================================");
    out(" NX Downloader - CONSOLE mode (applet/album)");
    out("==================================================");
    out("The graphical UI is skipped in applet mode because");
    out("it hangs on real hardware here (see BUILD.md).");
    out("For the full GUI: hold R and launch from a game.");
    out("");
    out("env     : " + ascii(netx::appletStateText(), 70));
    out("network : " + (netx::ready() ? std::string("ready") : std::string("NOT ready")));
    out("project : " + ascii(appcfg::PROJECT_DIR));
    out("log     : " + ascii(appcfg::LOG_FILE));
    logx::ui("文字（控制台）模式已启动");

    appcfg::UrlEntry current = entry;
    printMenu(current);

    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop())
    {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_A)
        {
            checkUpdate(downloader, current.update);
            printMenu(current);
        }
        else if (down & HidNpadButton_B)
        {
            downloadFile(downloader, current.download);
            printMenu(current);
        }
        else if (down & HidNpadButton_X)
        {
            out("  reloading url.txt ...");
            out(reloadUrl(&current) ? "  ok" : "  failed");
            printMenu(current);
        }
        else if (down & HidNpadButton_Y)
        {
            ioTest();
            printMenu(current);
        }

        svcSleepThread(50 * 1000 * 1000); // 50ms
    }

    out("exiting ...");
    consoleExit(nullptr);
    return 0;
}

} // namespace consmode
