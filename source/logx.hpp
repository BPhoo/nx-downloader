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
///
/// 实现要点：**打开一次、之后逐行追加**（见 fsx::openForAppend）。
/// 早期实现是「每写一行整份重写文件」，每行要删+建+开+写+flush+关共 6 次 FS 操作，
/// 启动阶段就会对 SD 卡打出上百次操作 —— Applet 模式下 SD 卡是与系统共用的，
/// 真机上表现为整机死机。现在每行只剩 1 次写操作。
void open();

/// 冲刷并关闭日志文件（退出前调用）
void close();

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

/// 日志落盘是否曾经失败（SD 卡写入异常）。
///
/// 为什么必须暴露它：如果 SD 卡写不进去，日志会**静静地停在上一次成功的那一行**，
/// 看起来像是「程序死在这一步」，其实只是后面的行没写下来 ——
/// 上一版真机排查就被这种情况误导过。界面会把它显示出来，别让日志默默变短。
bool writeFailed();

} // namespace logx
