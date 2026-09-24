/*
    NX Downloader - 网络初始化

    做法参考 NXSimpleDownloader（Mayo0x0）与 hb-appstore：
      1. 先 nifmInitialize(NifmServiceType_User) 把网络接口服务起起来；
      2. 再 socketInitializeDefault()；
      3. 若真的失败，用一份**显著更小**的配置重试；
      4. 退出顺序 socketExit() → nifmExit()。

    ⚠️ v2.1.0 及之前有两个错，都会让「网络不可用」：
      a) 降级重试照抄了 libnx 的默认配置（socketInitializeDefault() 内部就是
         socketInitialize(NULL) → &g_defaultSocketInitConfig），等于没有兜底；
      b) ★ 把 `LibnxError_AlreadyInitialized` 当成了失败。
         真机日志给出 0x00000F59 = MAKERESULT(Module_Libnx=345, 7)
         = LibnxError_AlreadyInitialized。而 libnx 的 socket.c 里
         `AddDevice("soc:")` **只在 bsdInitialize 成功之后才执行**，
         所以收到这个错误码恰恰说明「本进程里 socket 已经初始化好并且可用」。
         把它当失败就等于自己把联网功能整个禁掉 —— 已改为按成功处理。
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
