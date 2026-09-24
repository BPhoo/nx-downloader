/*
    NX Downloader - SD 卡日志

    为什么需要它：
      真机出问题时我们没有任何调试手段（没有 gdb、看不到 console 输出）。
      把每个阶段的结果（尤其是 libnx 的 Result 错误码）写到 SD 卡上，
      用户把文件发回来就能直接定位，不用再靠猜。

    实现：内存里累积，每次 line() 都整份重写文件 —— 这样即使进程随后
      崩溃/死机，最后一行也已经落盘了。

    ⚠️ 两个必须记住的约束：
      1. 日志是「多线程写入」的：下载工作线程和 UI 线程都会写。
         所有写入都过同一把互斥锁，否则两个线程同时改内存里的缓冲区
         就是数据竞争（真机上表现为随机死机）。
      2. 满 24KB 之后**丢掉前半段、保留最近的**，而不是停止记录 ——
         出问题时最有价值的就是最后几行，静默停止会让诊断反而失效。
*/
#pragma once

#include <switch.h>

#include <string>

namespace logx
{

/// 日志文件路径（sdmc:/switch/nx-downloader/log.txt）
const char* path();

/// 是否已启用（能写文件）
bool enabled();

/// 重建日志文件并写启动横幅
void open();

/// 追加一行（立刻落盘）
void line(const std::string& text);

/// printf 风格追加一行
void linef(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// 主线程（UI 线程）专用：行首会自动加 "[UI] " 标记，
/// 这样「界面卡在哪一步」和「下载线程干了什么」在日志里一眼可分。
void ui(const std::string& text);

/// printf 风格的 ui()
void uif(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// 把 libnx 的 Result 格式化成 "0x%08X"
std::string result(Result rc);

} // namespace logx
