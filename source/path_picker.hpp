/*
    NX Downloader - SD 卡目录选择视图

    这是一个「列出当前目录下的子目录」的列表视图：
      * A      进入子目录 / 使用当前目录
      * B      返回上一屏（不改变已选目录）
      * 十字键  移动光标

    注意：列表内容是在 draw() 里重建的（而不是在按钮回调里），
    这样可以避开「在回调中删除正在聚焦的视图」这类悬垂指针问题。
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

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;

  private:
    /// 请求切换到某个目录（实际切换延迟到下一帧）
    void requestNavigate(const std::string& path);

    /// 重新构建列表内容
    void rebuild();

    /// 确认选择当前目录
    void confirmCurrent();

    std::string currentPath;
    std::string pendingPath;

    std::function<void(const std::string&)> onSelect;
};
