/*
    NX Downloader - 主界面

    界面上只有三个控件：
      1. 链接输入框（ListItem，按 A 调起系统键盘）
      2. 保存目录按钮（带文件夹图标的 Button）
      3. 下载按钮（主色 Button）

    下载过程完全交给 Downloader（工作线程），本视图只负责：
      * 用一个 RepeatingTask 定期把进度刷到进度对话框上；
      * 结束后弹出完成/失败提示。
*/
#pragma once

#include <borealis.hpp>

#include <string>

#include "downloader.hpp"

class DownloadProgressDialog;

class MainView : public brls::List
{
  public:
    explicit MainView(Downloader* downloader);
    ~MainView() override;

    /// 轮询任务在下载进入终态后调用（UI 线程）
    void onDownloadFinished();

  private:
    void openPathPicker();
    void setDownloadDir(const std::string& path, bool persist);

    void startDownload();
    void beginDownload(const std::string& url, const std::string& fileName);

    void showMessage(const std::string& title, const std::string& text);

    void readSettings(std::string* url, std::string* dir) const;
    void saveSettings() const;

    Downloader* downloader = nullptr;

    brls::ListItem* urlItem      = nullptr;
    brls::Button* pathButton     = nullptr;
    brls::Button* downloadButton = nullptr;

    std::string downloadDir      = "sdmc:/downloads";

    DownloadProgressDialog* progressDialog = nullptr;
    brls::RepeatingTask* pollTask          = nullptr;
};
