/*
    NX Downloader - 网络初始化

    做法参考 NXSimpleDownloader（Mayo0x0）与 hb-appstore：
      1. 先 nifmInitialize(NifmServiceType_User) —— 把网络接口服务起起来；
      2. 再 socketInitializeDefault()；
      3. 失败时用一份**真正更小**的配置重试（hbmenu/Applet 模式下内存紧张时有用）；
      4. 退出顺序 socketExit() → nifmExit()。

    ⚠️ 上一版 v2.0.0 的“重试”是错的：它照抄了一份和 libnx 默认配置**一模一样**的
    SocketInitConfig（libnx 的 socketInitializeDefault() 内部就是 socketInitialize(NULL)
    → &g_defaultSocketInitConfig），所以重试必然同样失败，等于没有兜底。
    这里把缓冲区和会话数都压到很小，才是真正意义上的降级。
*/
#pragma once

#include <switch.h>

#include <string>

namespace netx
{

/// 初始化网络。幂等：已经成功过就直接返回 true。
bool init();

/// 网络当前是否可用
bool ready();

/// 关闭网络（socketExit → nifmExit）
void exit();

/// 本次/最近一次 socket 初始化（默认配置）的错误码
Result socketError();

/// 默认配置失败后，小配置重试的错误码（没重试过则为 0）
Result retryError();

/// nifmInitialize 的错误码
Result nifmError();

/// 是否运行在「完整内存模式」（按住 R 从游戏图标启动）
bool fullMemoryMode();

/// 一行环境描述（写日志 / 界面提示用）
std::string describe();

} // namespace netx
