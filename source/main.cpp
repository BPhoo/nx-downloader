/*
    NX Downloader
    ------------
    Nintendo Switch 自制程序：
      * 第一行：输入 / 读取更新链接 → 访问 → 显示服务器返回内容
      * 第二行：输入 / 读取下载链接 → 访问 → 下载文件到 SD 卡（可选目录、有进度、有完成与错误提示）

    启动时会确保项目文件夹与 url.txt 存在（不存在则创建），
    并把每个阶段的真实结果（含 libnx 的 Result 错误码）写进
    sdmc:/switch/nx-downloader/log.txt —— 真机出问题时看这个文件即可。

    依赖：
      * devkitPro (libnx)
      * borealis  (UI)
      * libcurl   (网络，devkitPro 的 switch-curl 包)
*/

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <curl/curl.h>

#include <borealis.hpp>
// nanovg 的上下文与 fallback 接口（borealis 的 include 路径里已经带了）
#include <nanovg/nanovg.h>

#include "app_config.hpp"
#include "downloader.hpp"
#include "fsx.hpp"
#include "logx.hpp"
#include "main_view.hpp"
#include "netx.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

// 全局下载器：生命周期与进程一致。
// 这样即使界面（MainView）已经被销毁，工作线程也一定能被安全地 join 掉。
static Downloader g_downloader;

namespace
{

/// 在最早期出错时给个看得见的提示（此时还没有 UI 库可用）
///
/// 这里刻意用英文：consoleInit() 走的是 libnx 的调试控制台，它用的是内置的
/// 等宽位图字体，没有汉字字形，写中文只会显示成方块。
/// 同时把错误写进日志文件（如果文件系统已经可用）。
void showFatalError(const char* message, Result rc = 0)
{
    if (logx::enabled())
    {
        logx::linef("致命错误: %s (rc=%s)", message, logx::result(rc).c_str());
        logx::line("已停止启动流程");
    }

    consoleInit(NULL);

    std::printf("\n  NX Downloader\n\n  %s\n", message);

    if (rc != 0)
        std::printf("  rc = 0x%08X\n", static_cast<unsigned>(rc));

    std::printf("\n  Log file (if SD is writable):\n    %s\n\n  Press A to exit\n", logx::path());

    consoleUpdate(NULL);

    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop())
    {
        padUpdate(&pad);

        if (padGetButtonsDown(&pad) & HidNpadButton_A)
            break;

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

/// 把系统内置的简体中文字体挂到正文字体后面，作为 fallback
///
/// 为什么需要这一步：
///   borealis 在 Switch 上用的是「系统共享字体」（PlSharedFontType_Standard），
///   而这份字体是按主机语言提供的 —— 主机语言是英文/日文时它并不含汉字，
///   界面上的中文会直接显示成方块/空白。系统里另外常驻了一份简体中文共享字体，
///   这里把它补进 fallback 链，中文就能正常显示。
///
/// 关于优先级：fonsAddFallbackFont() 是「追加到末尾」，查找字形时「从头往后」，
///   所以 borealis 先前注册的 material 图标字体优先级仍然更高，
///   中文字体只在前面几个字体都没有该字形时才会被用到，不会影响图标显示。
void attachChineseFallbackFont()
{
    PlFontData font = {};

    Result rc = plGetSharedFontByType(&font, PlSharedFontType_ChineseSimplified);
    if (R_FAILED(rc))
    {
        logx::linef("plGetSharedFontByType(ChineseSimplified) 失败 = %s（中文会显示成方块）",
            logx::result(rc).c_str());
        return;
    }

    logx::linef("简体中文共享字体：%u 字节", static_cast<unsigned>(font.size));

    // freeData = false：font.address 指向 pl 服务的共享内存，进程内常驻，
    // 绝不能让 nanovg 去 free 它（否则就是往系统共享内存上 free，直接崩）
    const int fontId = brls::Application::loadFontFromMemory("zh-Hans", font.address, font.size, false);

    if (fontId == -1)
    {
        logx::line("简体中文字体加载失败（中文会显示成方块）");
        return;
    }

    brls::FontStash* stash = brls::Application::getFontStash();
    NVGcontext* vg         = brls::Application::getNVGContext();

    // nvgAddFallbackFontId 在任一句柄为 -1 时会静默返回，这里显式判断
    if (stash != nullptr && vg != nullptr && stash->regular >= 0)
    {
        nvgAddFallbackFontId(vg, stash->regular, fontId);
        logx::line("已挂载简体中文字体 fallback");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    //------------------------------------------------------------------
    // 1. RomFS：borealis 的图标 / 翻译文件都在里面
    //------------------------------------------------------------------
    Result rc = romfsInit();
    if (R_FAILED(rc))
    {
        showFatalError("romfsInit failed.", rc);
        return EXIT_FAILURE;
    }

    //------------------------------------------------------------------
    // 2. SD 卡文件系统 + 日志
    //    日志尽量早开，后面每一步的结果都能落盘
    //------------------------------------------------------------------
    if (!fsx::init())
    {
        showFatalError("Cannot access the SD card filesystem.");
        romfsExit();
        return EXIT_FAILURE;
    }

    logx::open();
    logx::line("romfsInit = OK");
    logx::linef("fsx::init = OK (%s)", appcfg::PROJECT_DIR);

    //------------------------------------------------------------------
    // 3. 网络（nifm → socket，含真正的降级重试；失败不致命，界面仍可用）
    //------------------------------------------------------------------
    netx::init();

    //------------------------------------------------------------------
    // 4. libcurl
    //------------------------------------------------------------------
    const CURLcode curlRc = curl_global_init(CURL_GLOBAL_DEFAULT);
    logx::linef("curl_global_init = %d (%s)", static_cast<int>(curlRc),
        curl_easy_strerror(curlRc));

    //------------------------------------------------------------------
    // 5. 项目文件夹与 url.txt
    //    没有项目文件夹就创建，没有 url.txt 就在里面生成一份模板
    //------------------------------------------------------------------
    bool createdDir      = false;
    bool createdFile     = false;
    const bool projectOk = appcfg::ensureProjectLayout(&createdDir, &createdFile);

    logx::linef("ensureProjectLayout = %s (新建目录=%d 新建url.txt=%d)",
        projectOk ? "OK" : "FAILED", createdDir ? 1 : 0, createdFile ? 1 : 0);

    //------------------------------------------------------------------
    // 6. borealis
    //------------------------------------------------------------------
    brls::Logger::setLogLevel(brls::LogLevel::INFO);

    // 载入界面提示语的翻译（romfs:/i18n/en-US/brls.json）
    i18n::loadTranslations();

    if (!brls::Application::init("NX Downloader"))
    {
        showFatalError("Borealis initialization failed.");
        netx::exit();
        curl_global_cleanup();
        fsx::exit();
        romfsExit();
        return EXIT_FAILURE;
    }

    logx::line("Application::init = OK");
    logx::linef("环境：%s", netx::describe().c_str());

    //------------------------------------------------------------------
    // 7. 中文字体：必须在 Application::init 之后（此时字体表与 nanovg 上下文才就绪）
    //    否则英文/日文主机上的中文会全是方块
    //------------------------------------------------------------------
    attachChineseFallbackFont();

    //------------------------------------------------------------------
    // 8. 启动提示
    //    网络失败时把错误码直接显示出来（而不是只让用户看一句「失败」）
    //------------------------------------------------------------------
    if (!netx::fullMemoryMode())
        brls::Application::notify("当前是 Applet 模式（从相册启动）：系统键盘与网络都可能受限，"
                                  "需要完整功能请按住 R 键从游戏图标启动");

    if (!netx::ready())
        brls::Application::notify("网络初始化失败 " + logx::result(netx::socketError()) +
                                  "（小配置重试 " + logx::result(netx::retryError()) + "）· 详见 " +
                                  std::string(appcfg::LOG_FILE));

    if (!projectOk)
        brls::Application::notify("无法创建项目文件夹：" + std::string(appcfg::PROJECT_DIR));
    else if (createdFile)
        brls::Application::notify("已生成配置文件 " + std::string(appcfg::URL_FILE));

    //------------------------------------------------------------------
    // 9. 主界面
    //------------------------------------------------------------------
    brls::Application::pushView(new MainView(&g_downloader));

    while (brls::Application::mainLoop())
        ;

    //------------------------------------------------------------------
    // 10. 收尾顺序很重要：
    //     先让下载线程退出（它还在用文件系统），再关文件系统 / 网络
    //------------------------------------------------------------------
    logx::line("主循环退出，开始收尾");

    g_downloader.requestCancel();
    g_downloader.join();

    curl_global_cleanup();
    fsx::exit();
    netx::exit();
    romfsExit();

    return EXIT_SUCCESS;
}
