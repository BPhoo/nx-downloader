/*
    NX Downloader - 下载进度对话框

    只负责「显示」：所有数据都从 Downloader 的原子变量读取，
    真正的轮询由 MainView 里的 RepeatingTask 驱动（必须跑在 UI 线程上）。
*/
#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

class Downloader;

/// 进度对话框的内容视图（纵向布局）
class DownloadProgressContent : public brls::BoxLayout
{
  public:
    explicit DownloadProgressContent(const std::string& fileName);

    void setDetail(const std::string& text);
    void setProgress(int percent);

  private:
    brls::Label* nameLabel          = nullptr;
    brls::Label* detailLabel        = nullptr;
    brls::ProgressDisplay* bar      = nullptr;
};

/// 模态下载对话框：内容 + 一个「取消」按钮
class DownloadProgressDialog
{
  public:
    DownloadProgressDialog(const std::string& fileName, std::function<void()> onCancelRequested);

    /// 推入视图栈显示
    void open();

    /// 关闭（cb 在视图真正出栈、本对象已被释放后调用）
    void close(std::function<void()> cb);

    /// 从下载器状态刷新界面
    void update(const Downloader* downloader);

  private:
    brls::Dialog* dialog            = nullptr;
    DownloadProgressContent* content = nullptr;
};
