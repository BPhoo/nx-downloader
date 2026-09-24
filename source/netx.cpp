#include "netx.hpp"

#include <cstdio>
#include <string>

#include "logx.hpp"

namespace netx
{

namespace
{

bool g_socketOk = false;
bool g_nifmOk   = false;

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
    text += "，socket=" + (g_socketOk ? std::string("正常") : logx::result(g_socketErr));
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
        g_socketOk = true;
        logx::line("socketInitializeDefault() = OK");
        return true;
    }

    logx::linef("socketInitializeDefault() 失败 = %s", logx::result(g_socketErr).c_str());

    //------------------------------------------------------------------
    // 3. 降级重试 —— 这一次是**真的更小**的配置
    //    默认配置（libnx g_defaultSocketInitConfig）：
    //      tcp 0x8000 / 0x10000 / 0x40000 / 0x40000
    //      udp 0x2400 / 0xA500, sb_efficiency = 4, num_bsd_sessions = 3
    //    下面把每一项都压到很小，并只开 1 个 bsd 会话。
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
        g_socketOk = true;
        logx::line("socketInitialize(小配置) = OK");
        return true;
    }

    logx::linef("socketInitialize(小配置) 失败 = %s", logx::result(g_retryErr).c_str());
    logx::line("网络不可用：联网功能将被禁用，其余功能仍可使用");
    return false;
}

void exit()
{
    if (g_socketOk)
    {
        socketExit();
        g_socketOk = false;
    }

    if (g_nifmOk)
    {
        nifmExit();
        g_nifmOk = false;
    }
}

} // namespace netx
