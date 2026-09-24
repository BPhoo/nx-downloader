/*
    NX Downloader - 主界面

    界面结构（严格两行链接行 + 下方状态区）：

      更新链接  [ 输入框 ]  [ 选 txt 图标 ]              [ 访问 ]
      下载链接  [ 输入框 ]  [ 选 txt 图标 ] [ 选目录图标 ] [ 访问 ]
      状态：……                     ← 一行状态（正在检查更新… / 下载完成 …）
      [ 进度条 ]
      [ 取消当前任务 ]
      详情 / 返回内容：……          ← 多行，检查更新的返回内容、错误详情都写这里
      下载保存到：……（第二行的文件夹图标可更换）
      提示 …… · 运行环境 …… · 日志路径 ……

    交互：
      * A                打开系统键盘手动输入链接（最长 100 字符，无下限）
      * X                直接读「项目文件夹/url.txt」里对应的值
                         （第一行读 updata，第二行读 download）
      * 文件图标         打开 txt 选择器，把选中文件里的对应字段填进该行输入框
      * 目录图标（仅第二行）选择下载保存目录，默认 SD 卡根目录
      * 十字键 / 左摇杆  移动焦点
      * +                退出

    ★ v2.2.0 的设计铁律（这是真机「网络成功后整个界面冻死」的修复核心）：
      工作线程运行期间以及收尾时，**绝不改动视图栈**。
        * 不使用 Dialog；
        * 不使用 Application::notify 通知；
        * 不在任务回调里 pushView / popView。
      所有进度、结果、错误都只通过改本页面里的 Label 文本来体现
      （setText 只置脏标记，真正的布局发生在下一帧的绘制阶段，绝不会在
        动画回调里嵌套触发 pushView/布局）。

      为什么：v2.0.0 / v2.1.x 的收尾路径是
          RepeatingTask → Dialog::close(cb) → menu_animation 回调 → Application::pushView
      也就是「在动画回调里改视图栈 + 嵌套动画」。这条路径已经整个删掉。
      唯一保留 pushView 的地方是「点图标打开文件/目录选择器」这类按键触发的常规路径，
      而且选择器的回调只改文本、不改视图栈。
*/
#pragma once

#include <borealis.hpp>

#include <string>

#include "app_config.hpp"
#include "downloader.hpp"

class InputRow;

class MainView : public brls::List
{
  public:
    /// startupNotice：启动阶段要告诉用户的事（网络失败 / 已生成 url.txt 等），显示在详情区
    explicit MainView(Downloader* downloader, const std::string& startupNotice = "");
    ~MainView() override;

    /// 轮询任务每 100ms 调用一次（始终在 UI 线程上）
    void onPoll();

  private:
    /// 当前正在跑的是哪种任务（决定收尾时怎么展示结果）
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
    void beginTask(Task kind, const std::string& statusText);

    /// 任务进入终态后的一次性收尾（只改文本，不动视图结构）
    void finishTask(Downloader::State state);

    void setButtonsEnabled(bool enabled);
    void setCancelEnabled(bool enabled);
    void setOutputDir(const std::string& path, bool persist);

    //-------- 三个「只改文本」的显示接口（内容不变时什么都不做）--------//
    void setStatus(const std::string& text);
    void setDetail(const std::string& text);
    void setProgress(int percent);

    /// 任务进行期间每秒写一行日志，用来判断「渲染循环是否还活着」
    void heartbeat();

    void readSettings();
    void saveSettings() const;

    /// 从指定 txt 里取出对应字段填进该行输入框
    bool loadFromFile(bool forUpdate, const std::string& filePath);

    /// 按 X 时的快捷路径：直接读项目文件夹里的 url.txt
    bool loadFromProjectUrlFile(bool forUpdate);

    Downloader* downloader = nullptr;

    InputRow* updateRow   = nullptr;
    InputRow* downloadRow = nullptr;

    brls::Label* statusLabel           = nullptr;
    brls::ProgressDisplay* bar         = nullptr;
    brls::Button* cancelButton         = nullptr;
    brls::Label* detailLabel           = nullptr;
    brls::Label* dirLabel              = nullptr;
    brls::RepeatingTask* pollTask      = nullptr;

    /// 下载保存目录，默认 SD 卡根目录
    std::string outputDir = "sdmc:/";

    // 控件建好之前先从 settings.txt 里读出来暂存
    std::string savedUpdate;
    std::string savedDownload;
    std::string savedDir;

    Task task = Task::None;

    /// 已经写到界面上的内容：内容没变就不碰控件，避免每次都让布局重算
    std::string shownStatus;
    std::string shownDetail;
    int shownPercent = -1;

    /// 心跳用的计数（轮询是 100ms 一次，10 次 ≈ 1 秒）
    int pollCount        = 0;
    int lastHeartbeatAt  = 0;
};
