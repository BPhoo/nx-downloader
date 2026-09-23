/*
    NX Downloader - SD 卡目录选择视图

    这是一个「列出当前目录下的子目录」的列表视图：
      * A      进入子目录 / 使用当前目录
      * B      返回上一屏（不改变已选目录）
      * 十字键 / 左摇杆  移动光标

    实现要点（踩坑记录）：
      * 绝对不要在视图「还没入栈」时（构造函数阶段）调用 Application::giveFocus。
        否则会把本视图自己的某项存进 focusStack，导致 popView 之后焦点悬空、
        所有按键失灵。
      * 不要在 draw() 里增删子视图；在按钮回调里直接 rebuild 即可——borealis
        在删除正被聚焦的子项时会自动把全局焦点清掉，不会有悬垂指针。
*/
#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

class PathPickerView : public brls::List
{
  public:
    PathPickerView(const std::string& startPath, std::function<void(const std::string&)> onSelect);
    ~PathPickerView() override;

    void willAppear(bool resetState) override;

  private:
    /// 切换到某个目录并重建列表
    void navigateTo(const std::string& path);

    /// 重新构建列表内容
    void rebuild();

    /// 确认选择当前目录
    void confirmCurrent();

    std::string currentPath;

    std::function<void(const std::string&)> onSelect;

    /// 本视图是否已经入栈（用于判断 rebuild 时能否安全地抢焦点）
    bool onStack = false;
};
