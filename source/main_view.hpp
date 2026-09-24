/*
    NX Downloader - 主界面

    界面结构（严格两行）：

      第一行（检查更新）  [ 输入框：更新链接 ] [ 选 txt ] [ 访问 ]
      第二行（下载文件）  [ 输入框：下载链接 ] [ 选 txt ] [ 选目录 ] [ 访问 ]

    交互：
      * A                打开系统键盘，手动输入链接（最长 100 字符，无下限）
      * X                直接把「项目文件夹/url.txt」里对应的值读进来
                         （第一行读 updata，第二行读 download）
      * 文件图标         打开 txt 选择器，选中任意 .txt 后把对应字段填进该行输入框
      * 目录图标（仅第二行）选择下载保存目录，默认直接存到 SD 卡根目录
      * 十字键 / 左摇杆  移动焦点
      * +                退出

    下载与检查更新都交给 Downloader 的工作线程，本视图只负责：
      * 用一个 RepeatingTask 定期把进度刷到进度对话框上；
      * 结束后弹出结果（下载 → 完成/失败提示；检查更新 → 返回内容）。
*/
#pragma once

#include <borealis.hpp>

#include <string>

#include "app_config.hpp"
#include "downloader.hpp"

class DownloadProgressDialog;
class InputRow;

class MainView : public brls::List
{
  public:
    explicit MainView(Downloader* downloader);
    ~MainView() override;

    /// 轮询任务在任务进入终态后调用（UI 线程）
    void onTaskFinished();

  private:
    /// 当前正在跑的是哪种任务（决定结束时怎么展示结果）
    enum class Task
    {
        None = 0,
        Update,
        Download,
    };

    void buildRows();

    void openTextFilePicker(bool forUpdate);
    void openOutputDirPicker();

    void startUpdateCheck();
    void startDownload();
    void beginDownload(const std::string& url);
    void beginTask(Task kind, const std::string& title);

    void setButtonsEnabled(bool enabled);
    void setOutputDir(const std::string& path, bool persist);

    void showMessage(const std::string& title, const std::string& text);

    void readSettings();
    void saveSettings() const;

    /// 从指定 txt 里取出对应字段填进该行输入框
    bool loadFromFile(bool forUpdate, const std::string& filePath);

    /// 按 X 时的快捷路径：直接读项目文件夹里的 url.txt
    bool loadFromProjectUrlFile(bool forUpdate);

    Downloader* downloader = nullptr;

    InputRow* updateRow   = nullptr;
    InputRow* downloadRow = nullptr;

    brls::Label* dirLabel = nullptr;

    /// 下载保存目录，默认 SD 卡根目录
    std::string outputDir = "sdmc:/";

    // 控件建好之前先从 settings.txt 里读出来暂存
    std::string savedUpdate;
    std::string savedDownload;
    std::string savedDir;

    DownloadProgressDialog* progressDialog = nullptr;
    brls::RepeatingTask* pollTask          = nullptr;

    Task task = Task::None;
};
