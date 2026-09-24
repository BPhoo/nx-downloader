/*
    NX Downloader
    ------------
    Nintendo Switch 自制程序：
      * 第一行：输入 / 读取更新链接 → 访问 → 显示服务器返回内容
      * 第二行：输入 / 读取下载链接 → 访问 → 下载文件到 SD 卡（可选目录、有进度、有完成与错误提示）

    启动时会确保项目文件夹与 url.txt 存在（不存在则创建）。

    依赖：
      * devkitPro (libnx)
      * borealis  (UI)
      * libcurl   (网络，devkitPro 的 switch-curl 包)
*/

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <curl/curl.h>

#include <borealis.hpp>
// nanovg 的上下文与 fallback 接口（borealis 的 include 路径里已经带了）
#include <nanovg/nanovg.h>

#include "app_config.hpp"
#include "downloader.hpp"
#include "fsx.hpp"
#include "main_view.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

/// 初始化网络（BSD socket 服务）
///
/// 为什么不直接用 socketInitializeDefault()：
///   从「相册」启动时是 Applet 模式，可用内存远小于完整应用模式，
///   socketInitializeDefault() 默认的缓冲配置可能申请失败而返回非 0。
///   这里先用默认配置，失败再退回到一份较小的配置重试一次。
/// 兼容不同版本 libnx 的 SocketInitConfig：
/// 旧版本带有 bsdsockets_version 字段，新版本已移除该字段。
/// 用 SFINAE 让「赋值」只在字段存在时才发生，从而一份代码两种布局都能编译。
template <typename T>
static auto setBsdVersion(T& c, int v, int) -> decltype(c.bsdsockets_version = v, void())
{
    c.bsdsockets_version = v;
}
template <typename T>
static void setBsdVersion(T&, int, long)
{
}

static bool initNetwork()
{
    if (R_SUCCEEDED(socketInitializeDefault()))
        return true;

    SocketInitConfig cfg{}; // 值初始化：所有字段先清零
    setBsdVersion(cfg, 1, 0);
    cfg.tcp_tx_buf_size      = 0x8000;
    cfg.tcp_rx_buf_size      = 0x10000;
    cfg.tcp_tx_buf_max_size  = 0x40000;
    cfg.tcp_rx_buf_max_size  = 0x40000;
    cfg.udp_tx_buf_size      = 0x2400;
    cfg.udp_rx_buf_size      = 0x4000;
    cfg.sb_efficiency        = 4;

    return R_SUCCEEDED(socketInitialize(&cfg));
}

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
    const bool socketOk = initNetwork();

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

    //------------------------------------------------------------------
    // 4. 项目文件夹与 url.txt
    //    没有项目文件夹就创建，没有 url.txt 就在里面生成一份模板
    //------------------------------------------------------------------
    bool createdDir  = false;
    bool createdFile = false;
    const bool projectOk = appcfg::ensureProjectLayout(&createdDir, &createdFile);

    //------------------------------------------------------------------
    // 5. borealis
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

    if (!projectOk)
        brls::Application::notify("无法创建项目文件夹，url.txt 相关功能不可用：" + std::string(appcfg::PROJECT_DIR));
    else if (createdDir && createdFile)
        brls::Application::notify("已创建项目文件夹与 url.txt：" + std::string(appcfg::PROJECT_DIR));
    else if (createdFile)
        brls::Application::notify("已生成 url.txt：" + std::string(appcfg::URL_FILE));
    else if (createdDir)
        brls::Application::notify("已创建项目文件夹：" + std::string(appcfg::PROJECT_DIR));

    if (!socketOk)
        brls::Application::notify("网络初始化失败：联网功能不可用。请确认已连接 Wi-Fi；若从相册启动仍失败，可按住 R 键从游戏图标以完整内存模式启动");

    //------------------------------------------------------------------
    // 6. 中文字体：必须在 Application::init 之后（此时字体表与 nanovg 上下文才就绪）
    //    否则英文/日文主机上的中文会全是方块
    //------------------------------------------------------------------
    attachChineseFallbackFont();

    //------------------------------------------------------------------
    // 7. 主界面
    //------------------------------------------------------------------
    brls::Application::pushView(new MainView(&g_downloader));

    while (brls::Application::mainLoop())
        ;

    //------------------------------------------------------------------
    // 8. 收尾顺序很重要：
    //    先让工作线程退出（它还在用文件系统），再关文件系统 / 网络
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
