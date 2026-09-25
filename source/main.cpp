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
#include "console_mode.hpp"
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
    // 5b. 提前读 url.txt / settings.txt
    //
    //     ★ 刻意**不**留到主界面的构造函数里读：
    //       Applet 模式（从相册启动）下 SD 卡是与父 applet / 系统共用的，
    //       真机日志显示崩溃点正好落在构造函数中间、紧跟一次 SD 写盘之后。
    //       把这两个读挪到 UI 之前，构造函数就只剩「纯 CPU 的视图树搭建」。
    //       这里每一步都记结果，出问题能看出是哪一次 SD 访问。
    //------------------------------------------------------------------
    appcfg::UrlEntry urlEntry;
    {
        std::string urlText;
        const bool read  = fsx::readWholeFile(appcfg::URL_FILE, &urlText);
        const bool parse = read && appcfg::parseUrlFile(urlText, &urlEntry);

        logx::linef("读 url.txt = %d（%u 字节），解析 = %d（updata=%d download=%d img=%u 项）",
            read ? 1 : 0, static_cast<unsigned>(urlText.size()), parse ? 1 : 0,
            urlEntry.update.empty() ? 0 : 1, urlEntry.download.empty() ? 0 : 1,
            static_cast<unsigned>(urlEntry.images.size()));
    }

    std::string settingsText;
    {
        const bool read = fsx::readWholeFile(appcfg::SETTINGS_FILE, &settingsText);
        logx::linef("读 settings.txt = %d（%u 字节）", read ? 1 : 0,
            static_cast<unsigned>(settingsText.size()));
    }

    //------------------------------------------------------------------
    // 6. ★ 界面分支：Applet 模式 → 文字（控制台）模式
    //
    //    真机上 Applet 模式（从相册启动）会在**构造图形界面**的过程中整机死机
    //    （冻结点落在纯 CPU 的视图树构造里，紧跟在一次 SD 写盘之后，详见 BUILD.md §1）。
    //    与其继续赌，不如在这条路径上直接改用纯 libnx 控制台：
    //      * 不创建 GL 上下文、不用 nanovg / borealis / 字体；
    //      * 功能保留「检查更新 / 下载文件 / 重新读 url.txt / SD 卡 I/O 自检」。
    //
    //    这同时也是一次决定性实验：文字模式若在 Applet 模式下活得好好的，
    //    说明问题在 borealis/GL 那条栈上；若它也卡死，就是 Applet 环境本身的问题。
    //
    //    两个开关文件（放在项目文件夹里）方便真机上反复验证，不用改代码重新编译：
    //      console.txt = 强制文字模式（完整内存模式下也能验证文字界面）
    //      gui.txt     = 强制图形界面（Applet 模式下也能再试一次图形界面）
    //------------------------------------------------------------------
    const bool isApplet   = appletGetAppletType() != AppletType_Application;
    const bool wantGui    = fsx::exists(std::string(appcfg::PROJECT_DIR) + "/gui.txt");
    const bool wantText   = fsx::exists(std::string(appcfg::PROJECT_DIR) + "/console.txt");
    const bool useConsole = (!wantGui && (isApplet || wantText));

    logx::linef("界面选择：appletType=%d（isApplet=%d）强制文字=%d 强制图形=%d → %s",
        static_cast<int>(appletGetAppletType()), isApplet ? 1 : 0, wantText ? 1 : 0, wantGui ? 1 : 0,
        useConsole ? "文字（控制台）模式" : "图形界面");

    if (useConsole)
    {
        logx::ui("不初始化 borealis / GL，直接进文字模式");

        const int code = consmode::run(&g_downloader, urlEntry);

        logx::ui("文字模式退出，开始收尾");
        g_downloader.requestCancel();
        g_downloader.join();
        curl_global_cleanup();
        logx::close();
        fsx::exit();
        netx::exit();
        romfsExit();
        return code;
    }

    //------------------------------------------------------------------
    // 7. borealis（只用于完整内存模式；按住 R 从游戏图标启动）
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
    logx::linef("applet 状态：%s", netx::appletStateText().c_str());

    //------------------------------------------------------------------
    // 8. 中文字体：必须在 Application::init 之后（此时字体表与 nanovg 上下文才就绪）
    //    否则英文/日文主机上的中文会全是方块
    //------------------------------------------------------------------
    attachChineseFallbackFont();

    //------------------------------------------------------------------
    // 8. 启动提示
    //    v2.2.0 起不再用 Application::notify：通知同样是「多一个动画 + 多一个视图」，
    //    而本轮修复的重点就是把收尾路径上的动画/视图栈操作全部去掉。
    //    这里改成把要说的话交给主界面，显示在详情区。
    //------------------------------------------------------------------
    std::string notice;

    if (!netx::fullMemoryMode())
        notice += "当前是 Applet 模式（从相册启动）：系统键盘会被禁用（有死机风险），"
                  "需要完整功能请按住 R 键从游戏图标启动。\n\n";

    if (!netx::ready())
        notice += "网络初始化失败 " + logx::result(netx::socketError()) +
                  "（小配置重试 " + logx::result(netx::retryError()) + "）· 详见 " +
                  std::string(appcfg::LOG_FILE) + "\n\n";

    if (!projectOk)
        notice += "无法创建项目文件夹：" + std::string(appcfg::PROJECT_DIR) + "\n\n";
    else if (createdFile)
        notice += "已生成配置文件 " + std::string(appcfg::URL_FILE) + "（可按 X 读取）\n\n";

    if (!notice.empty())
        logx::ui("启动提示：" + notice);

    //------------------------------------------------------------------
    // 9. 主界面
    //    pushView 内部会立刻布局一次整棵视图树（invalidate(true)），
    //    所以这里前后各写一行日志：真机若在这里崩溃，一眼就能看出是
    //    「构造」还是「首次布局」出的问题。
    //------------------------------------------------------------------
    logx::ui("准备 pushView 主界面");
    brls::Application::pushView(new MainView(&g_downloader, notice, urlEntry, settingsText));
    logx::ui("pushView 已返回，进入主循环");

    while (brls::Application::mainLoop())
        ;

    //------------------------------------------------------------------
    // 10. 收尾顺序很重要：
    //     先让下载线程退出（它还在用文件系统），再关日志、文件系统 / 网络
    //------------------------------------------------------------------
    logx::line("主循环退出，开始收尾");

    g_downloader.requestCancel();
    g_downloader.join();

    curl_global_cleanup();
    logx::close(); // 日志句柄必须在 fsx::exit() 之前关掉
    fsx::exit();
    netx::exit();
    romfsExit();

    return EXIT_SUCCESS;
}
