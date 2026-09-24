/*
    NX Downloader - 配置文件（.txt）选择视图

    界面上两行的「文件图标」按钮都会打开它：浏览 SD 卡目录，
    选中某个 .txt 后回调把完整路径交给调用方，由调用方解析出
    updata / download 再填进对应的输入框。

    按键：
      * A / 十字键 + 左摇杆   在列表里移动；在目录上按 A 进入，在 .txt 上按 A 选中
      * B                    返回上一屏（不改变输入框内容）

    实现要点（踩坑记录，与 PathPickerView 同理）：
      * 绝对不要在视图还没入栈时（构造函数阶段）调用 Application::giveFocus，
        否则会把本视图的项存进 focusStack，popView 之后焦点悬空、按键全部失灵。
      * 不要在 draw() 里增删子视图；在回调里直接 rebuild 即可。
*/
#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

class TextFilePickerView : public brls::List
{
  public:
    /// startPath 起始目录；onPick 收到被选中 txt 的完整 sdmc 路径
    TextFilePickerView(const std::string& startPath, std::function<void(const std::string&)> onPick);
    ~TextFilePickerView() override;

    void willAppear(bool resetState) override;

  private:
    void navigateTo(const std::string& path);
    void rebuild();

    std::string currentPath;

    std::function<void(const std::string&)> onPick;

    /// 本视图是否已经入栈（决定 rebuild 时能否安全地抢焦点）
    bool onStack = false;
};
