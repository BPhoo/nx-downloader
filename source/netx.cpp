#include "netx.hpp"

#include <cstdio>
#include <string>

#include "logx.hpp"

namespace netx
{

namespace
{

/// libnx 的 LibnxError_AlreadyInitialized
///   Module_Libnx = 345, LibnxError_AlreadyInitialized = 7
///   → MAKERESULT = 0x00000F59（真机日志里就是这个值）
constexpr Result kAlreadyInitialized = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

bool g_socketOk    = false;
/// socket 是不是「我们自己」初始化的（决定退出时要不要 socketExit）
bool g_socketOwner = false;
bool g_nifmOk      = false;

Result g_nifmErr   = 0;
Result g_socketErr = 0;
Result g_retryErr  = 0;

std::string g_connText = "未查询";

/// 把连接状态变成一句人话，写进日志也显示在界面上
void probeConnection()
{
    g_connText = "nifm 不可用";

    if (!g_nifmOk)
        return;

    NifmInternetConnectionType type     = static_cast<NifmInternetConnectionType>(0);
    u32 wifiStrength                    = 0;
    NifmInternetConnectionStatus status = static_cast<NifmInternetConnectionStatus>(0);

    Result rc = nifmGetInternetConnectionStatus(&type, &wifiStrength, &status);
    if (R_FAILED(rc))
    {
        g_connText = "查询联网状态失败 " + logx::result(rc);
        return;
    }

    const char* typeText = (type == NifmInternetConnectionType_WiFi) ? "Wi-Fi"
        : (type == NifmInternetConnectionType_Ethernet)               ? "有线"
                                                                     : "无";

    if (status == NifmInternetConnectionStatus_Connected)
        g_connText = std::string("已联网（") + typeText + "，信号 " + std::to_string(wifiStrength) + "）";
    else
        g_connText = std::string("未联网（") + typeText + "，状态码 " + std::to_string(static_cast<int>(status)) + "）";
}

/// socket 层可用性自检。
///
/// 曾经想用 bsdSocket()/bsdClose() 开一个 TCP socket 来实测，但 libnx 4.12.0 的
/// 库里**没有导出这两个符号**（编译会 undefined reference），所以改为只记录
/// 初始化返回值 —— 真正的可用性由第一次实际请求（curl）的结果来体现，
/// 那个结果在 downloader 里已经完整记进日志了。
void probeSocketLayer()
{
    logx::line("socket 初始化返回值已记录；实际可用性看后续请求的日志");
}

} // namespace

Result socketError()
{
    return g_socketErr;
}

Result retryError()
{
    return g_retryErr;
}

Result nifmError()
{
    return g_nifmErr;
}

bool ready()
{
    return g_socketOk;
}

bool fullMemoryMode()
{
    return appletGetAppletType() == AppletType_Application;
}

std::string describe()
{
    std::string text = std::string("模式=") + (fullMemoryMode() ? "完整内存" : "Applet（相册启动）");
    text += "，联网=" + g_connText;
    text += "，network=" + (g_socketOk ? std::string("可用") : ("不可用 " + logx::result(g_socketErr)));
    return text;
}

bool init()
{
    if (g_socketOk)
        return true;

    logx::linef("网络初始化：%s", fullMemoryMode() ? "完整内存模式" : "Applet 模式（相册启动）");

    //------------------------------------------------------------------
    // 1. nifm —— 参考 NXSimpleDownloader：socket 之前先把网络接口服务初始化
    //------------------------------------------------------------------
    if (!g_nifmOk)
    {
        g_nifmErr = nifmInitialize(NifmServiceType_User);
        g_nifmOk  = R_SUCCEEDED(g_nifmErr);

        logx::linef("nifmInitialize(NifmServiceType_User) = %s", logx::result(g_nifmErr).c_str());
    }

    probeConnection();
    logx::linef("联网状态：%s", g_connText.c_str());

    //------------------------------------------------------------------
    // 2. socket —— 先用 libnx 默认配置
    //------------------------------------------------------------------
    g_socketErr = socketInitializeDefault();

    if (R_SUCCEEDED(g_socketErr))
    {
        g_socketOk    = true;
        g_socketOwner = true;
        logx::line("socketInitializeDefault() = OK");
        probeSocketLayer();
        return true;
    }

    logx::linef("socketInitializeDefault() = %s", logx::result(g_socketErr).c_str());

    //------------------------------------------------------------------
    // 3. ★ 已经被初始化过 —— 这不是失败，是「现成的可用状态」
    //
    //    libnx 的 socket.c 里 `AddDevice(&g_socketDevoptab)`（登记 "soc:"）只在
    //    bsdInitialize 成功之后执行；socketInitialize 开头只做了一件事：
    //        int dev = FindDevice("soc:");
    //        if (dev != -1) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    //    所以拿到 AlreadyInitialized ⇒ 本进程里已经有一次成功的 socket 初始化。
    //------------------------------------------------------------------
    if (g_socketErr == kAlreadyInitialized)
    {
        g_socketOk    = true;
        g_socketOwner = false; // 不是我们开的，退出时不要 socketExit

        logx::line("socket 已被初始化过（AlreadyInitialized）—— 说明网络层本来就可用，按成功处理");
        probeSocketLayer();
        return true;
    }

    //------------------------------------------------------------------
    // 4. 其它错误 —— 降级重试，这次是**真的更小**的配置
    //    默认配置（libnx g_defaultSocketInitConfig）：
    //      tcp 0x8000 / 0x10000 / 0x40000 / 0x40000
    //      udp 0x2400 / 0xA500, sb_efficiency = 4, num_bsd_sessions = 3
    //------------------------------------------------------------------
    SocketInitConfig small{};
    small.tcp_tx_buf_size     = 0x800;
    small.tcp_rx_buf_size     = 0x1000;
    small.tcp_tx_buf_max_size = 0x2000;
    small.tcp_rx_buf_max_size = 0x4000;
    small.udp_tx_buf_size     = 0x1000;
    small.udp_rx_buf_size     = 0x1000;
    small.sb_efficiency       = 1;
    small.num_bsd_sessions    = 1;
    small.bsd_service_type    = BsdServiceType_User;

    g_retryErr = socketInitialize(&small);

    if (R_SUCCEEDED(g_retryErr))
    {
        g_socketOk    = true;
        g_socketOwner = true;
        logx::line("socketInitialize(小配置) = OK");
        probeSocketLayer();
        return true;
    }

    logx::linef("socketInitialize(小配置) = %s", logx::result(g_retryErr).c_str());
    logx::line("网络不可用：联网功能将被禁用，其余功能仍可使用");
    return false;
}

void exit()
{
    // 只关我们自己开的：如果是别人（启动阶段）已经初始化好的，就不要拆掉
    if (g_socketOk && g_socketOwner)
    {
        socketExit();
        logx::line("socketExit() 完成");
    }

    g_socketOk    = false;
    g_socketOwner = false;

    if (g_nifmOk)
    {
        nifmExit();
        g_nifmOk = false;
    }
}

} // namespace netx
