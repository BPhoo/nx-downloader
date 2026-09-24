/*
    NX Downloader - 主界面

    界面结构（v2.3.0）：

      ┌ NX Downloader ──────────────────────────────────────────┐
        更新链接  [ 输入框 ]  [ 选 txt 图标 ]              [ 访问 ]
        下载链接  [ 输入框 ]  [ 选 txt 图标 ] [ 选目录图标 ] [ 访问 ]
        状态：……
        [ 进度条 ]
        [ 取消当前任务 ]          ← 有任务时才出现，平时不可聚焦
        详情 / 返回内容：……
        图片：……                  ← url.txt 里 img: 的加载情况
        [ 图片 1 ]  [ 图片 2 ] ……  ← 固定高度的图片位（不可聚焦）
        下载保存到：sdmc:/（第二行的文件夹图标可更换）
        [ 诊断信息与提示 ]         ← 底部锚点：可聚焦，保证能滚到最底
      └─────────────────────────────────────────────────────────┘

    交互：
      * A                打开系统键盘手动输入链接（最长 100 字符，无下限）
      * X                直接读「项目文件夹/url.txt」里对应的值
      * 文件图标         打开 txt 选择器，把选中文件里的对应字段填进该行输入框
      * 目录图标（仅第二行）选择下载保存目录，默认 SD 卡根目录
      * 十字键 / 左摇杆  移动焦点；+ 退出

    ★ 两条必须守住的设计铁律
      1) 工作线程运行期间/收尾时**绝不改动视图栈**：不用 Dialog、不发通知、
         不在任务回调里 pushView/popView；进度与结果只通过改 Label 文本体现。
         （v2.0.0/v2.1.x 那种「动画回调里 pushView」的写法正是真机冻死的元凶。）
         唯一的例外是**按键触发**的常规路径：点图标开选择器、点「诊断信息」翻开详情页。
      2) 页面最底部必须留一个**可聚焦**的控件。borealis 的 List 只会「滚动到当前焦点」，
         底部如果没有可聚焦项，再往下就永远滚不动（实测就是「底部内容看不全」）。
*/
#pragma once

#include <borealis.hpp>

#include <string>
#include <vector>

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
        Images, // 批量下载 url.txt 里 img: 指向的图片
    };

    /// 一个图片位：先看本地缓存，没有再联网下载
    struct ImageSlot
    {
        brls::Image* view = nullptr;
        std::string item;   // url.txt 里那一项的原文（http(s) 链接 或 本地文件名）
        std::string cached; // 本地文件路径（sdmc: 开头）
        bool pending = false; // 需要联网下载
        bool loaded  = false; // 已经加载进 view
    };

    void buildRows();
    void buildStatusArea();
    void buildGallery();
    void buildFooter();

    void openTextFilePicker(bool forUpdate);
    void openOutputDirPicker();
    void openDetailsView();

    void startUpdateCheck();
    void startDownload();
    void beginDownload(const std::string& url);
    void beginTask(Task kind, const std::string& statusText);

    /// 任务进入终态后的一次性收尾（只改文本，不动视图结构）
    void finishTask(Downloader::State state);
    /// 图片批量的收尾：加载当前这张，然后继续下一张
    void finishImageTask(Downloader::State state);

    /// 启动后第一次轮询时调用：加载本地已有的图片，并开始下载缺的那些
    void startStartupImageLoad();
    void loadImageIntoSlot(size_t index, const std::string& path);
    void refreshImageSummary();
    /// 找下一张待下载的图片并开始下载；没有则返回 false
    bool startNextPendingImage();

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

    brls::Label* statusLabel        = nullptr;
    brls::ProgressDisplay* bar      = nullptr;
    brls::Button* cancelButton      = nullptr;
    brls::Label* detailLabel        = nullptr;
    brls::Label* imageLabel         = nullptr;
    brls::Label* dirLabel           = nullptr;
    brls::Button* detailsButton     = nullptr;
    brls::RepeatingTask* pollTask   = nullptr;

    std::vector<ImageSlot> imageSlots;
    /// 正在下载的图片下标（-1 = 没有）
    int imageLoadingIndex = -1;
    /// 启动流程是否已经跑过
    bool startupImagesStarted = false;
    int imagesLoaded = 0;
    int imagesFailed = 0;

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
    int pollCount       = 0;
    int lastHeartbeatAt = 0;
};
