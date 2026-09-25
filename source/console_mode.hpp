/*
    NX Downloader - 文字（控制台）模式

    用途与设计动机见 console_mode.cpp 顶部注释。要点：
      * 不创建 GL 上下文、不用 nanovg / borealis / 字体，纯 libnx 控制台；
      * Applet 模式（从相册启动）下自动进入（真机上图形界面在这条路径会整机死机）；
      * 屏上文案一律 ASCII（libnx console 没有汉字字形）。
*/
#pragma once

#include <string>

#include "app_config.hpp"

class Downloader;

namespace consmode
{

/// 运行文字模式主循环，返回进程退出码。
/// entry 是 main.cpp 已经读好的 url.txt 内容（本函数内部会持有可变副本）。
int run(Downloader* downloader, const appcfg::UrlEntry& entry);

} // namespace consmode
