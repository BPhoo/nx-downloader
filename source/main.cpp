/*
    NX Downloader
    ------------
    Nintendo Switch 自制程序：输入链接 → 选择保存目录 → 下载到 SD 卡。

    依赖：
      * devkitPro (libnx)
      * borealis  (UI)
      * libcurl   (网络，devkitPro 的 switch-curl 包)
*/

#include <switch.h>

#include <cstdio>
#include <cstdlib>

#include <curl/curl.h>

#include <borealis.hpp>
// nanovg 的上下文与 fallback 接口（borealis 的 include 路径里已经带了）
#include <nanovg/nanovg.h>

#include "downloader.hpp"
#include "fsx.hpp"
#include "main_view.hpp"

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
void showFatalError(const char* message)
{
    consoleInit(NULL);

    std::printf("\n  NX Downloader\n\n  %s\n\n  Press A to exit\n", message);

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

    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_ChineseSimplified)))
    {
        brls::Logger::warning("简体中文共享字体不可用，中文可能显示为方块");
        return;
    }

    // freeData = false：font.address 指向 pl 服务的共享内存，进程内常驻，
    // 绝不能让 nanovg 去 free 它（否则就是往系统共享内存上 free，直接崩）
    const int fontId = brls::Application::loadFontFromMemory("zh-Hans", font.address, font.size, false);

    if (fontId == -1)
    {
        brls::Logger::warning("简体中文字体加载失败，中文可能显示为方块");
        return;
    }

    brls::FontStash* stash = brls::Application::getFontStash();
    NVGcontext* vg         = brls::Application::getNVGContext();

    // nvgAddFallbackFontId 在任一句柄为 -1 时会静默返回，这里显式判断
    if (stash != nullptr && vg != nullptr && stash->regular >= 0)
    {
        nvgAddFallbackFontId(vg, stash->regular, fontId);
        brls::Logger::info("已挂载简体中文字体 fallback");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    //------------------------------------------------------------------
    // 1. RomFS：borealis 的图标 / 翻译文件都在里面
    //------------------------------------------------------------------
    romfsInit();

    //------------------------------------------------------------------
    // 2. 网络（libcurl 需要先初始化 socket 驱动）
    //------------------------------------------------------------------
    const bool socketOk = R_SUCCEEDED(socketInitializeDefault());

    curl_global_init(CURL_GLOBAL_DEFAULT);

    //------------------------------------------------------------------
    // 3. SD 卡文件系统
    //------------------------------------------------------------------
    if (!fsx::init())
    {
        showFatalError("Cannot access the SD card filesystem.");
        curl_global_cleanup();
        if (socketOk)
            socketExit();
        romfsExit();
        return EXIT_FAILURE;
    }

    // 准备好默认目录（失败不致命，真正下载时会再检查一次）
    fsx::ensureDirectory("sdmc:/switch/nx-downloader");
    fsx::ensureDirectory("sdmc:/downloads");

    //------------------------------------------------------------------
    // 4. borealis
    //------------------------------------------------------------------
    brls::Logger::setLogLevel(brls::LogLevel::INFO);

    // 载入界面提示语的翻译（romfs:/i18n/en-US/brls.json）
    i18n::loadTranslations();

    if (!brls::Application::init("NX Downloader"))
    {
        showFatalError("Borealis initialization failed.");
        fsx::exit();
        curl_global_cleanup();
        if (socketOk)
            socketExit();
        romfsExit();
        return EXIT_FAILURE;
    }

    if (!socketOk)
        brls::Application::notify("网络初始化失败，下载可能不可用");

    //------------------------------------------------------------------
    // 5. 中文字体：必须在 Application::init 之后（此时字体表与 nanovg 上下文才就绪）
    //    否则英文/日文主机上的中文会全是方块
    //------------------------------------------------------------------
    attachChineseFallbackFont();

    //------------------------------------------------------------------
    // 6. 主界面
    //------------------------------------------------------------------
    brls::Application::pushView(new MainView(&g_downloader));

    while (brls::Application::mainLoop())
        ;

    //------------------------------------------------------------------
    // 7. 收尾顺序很重要：
    //    先让下载线程退出（它还在用文件系统），再关文件系统 / 网络
    //------------------------------------------------------------------
    g_downloader.requestCancel();
    g_downloader.join();

    curl_global_cleanup();
    fsx::exit();

    if (socketOk)
        socketExit();

    romfsExit();

    return EXIT_SUCCESS;
}
