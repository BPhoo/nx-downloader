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
        [ 诊断信息…（点开后就地展开下面两段）]  ← 底部锚点：可聚焦，保证能滚到最底
        （展开后）环境 / 路径 / 图片加载结果
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
         唯一的例外是**按键触发**的常规路径：点图标开文件 / 目录选择器。
         （「诊断信息」自 v2.7.0 起也改成在主页面就地展开，不再新建页面 ——
           真机实测点它会直接报错退出，而就地展开是图片位已验证的机制。）
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
class RemoteImageView;

class MainView : public brls::List
{
  public:
    /// startupNotice：启动阶段要告诉用户的事（网络失败 / 已生成 url.txt 等），先存起来，
    ///                等界面跑起来后再显示（构造函数里不碰界面文案以外的任何东西）
    /// urlEntry：     已经由 main.cpp 读好的 url.txt 内容（updata / download / img）
    /// settingsText： 已经由 main.cpp 读好的 settings.txt 原文（界面自己解析）
    ///
    /// ★ 为什么把 url.txt / settings.txt 的**读取**移到构造函数之外：
    ///   Applet 模式（从相册启动）下 SD 卡是与父 applet / 系统共用的。真机日志显示
    ///   崩溃点正好落在构造函数中间、紧跟在一次 SD 写盘之后；把构造函数里的
    ///   所有 SD 访问拿掉之后，它只剩「纯 CPU 的视图树搭建」+ 逐行日志。
    explicit MainView(Downloader* downloader, const std::string& startupNotice = "",
        const appcfg::UrlEntry& urlEntry = appcfg::UrlEntry(), const std::string& settingsText = "");
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
        // ★ 用自己实现 RemoteImageView，而**不是** brls::Image：
        //   brls::Image 走 nvgCreateImage(path) → 内部 fopen，需要 sdmc: 的 devoptab；
        //   而且它有个真机坑 —— View::layout 会把首次布局时的高度缓存进 origViewHeight，
        //   而 Application::pushView 会强制先布局一次（那时图片还处于折叠状态、高度 0），
        //   于是 0 被永久缓存，再用 0×0 的纹理尺寸算出 0/0 = NaN 的缩放比。
        //   自绘的视图没有这些问题：纹理为空就不画，尺寸全部用整数，绝不产生 NaN。
        RemoteImageView* view = nullptr;
        std::string item;     // url.txt 里那一项的原文（http(s) 链接 或 本地文件名）
        std::string cached;   // 本地文件路径（sdmc: 开头）
        std::string note;     // 这一格的状态说明（给诊断页用）
        bool pending = false; // 需要联网下载
        bool loaded  = false; // 已经加载进 view
    };

    void buildRows();
    void buildStatusArea();
    /// 用 main.cpp 已经读好的 img: 列表建图片位（自己**不读 SD 卡**）
    void buildGallery(const std::vector<std::string>& configured);
    void buildFooter(const std::string& startupNotice);

    void openTextFilePicker(bool forUpdate);
    void openOutputDirPicker();

    /// 展开 / 收起底部的诊断信息。
    ///
    /// ★ v2.7.0 起**不再 pushView 新页面**（旧版点它会直接报错退出）：
    ///   诊断正文改用主页面已有的 Label 就地展开/收起 —— expand/collapse
    ///   是图片位一直在用、已验证的机制，不涉及视图栈、不新建视图树。
    ///   正文控制在几百字节并拆成两段，避免任何超长文本。
    void toggleDiagnostics();

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

    /// 把启动提示（Applet 模式 / 网络失败 / 新建了 url.txt）显示出来。
    /// 放在界面已经跑起来之后（第一次轮询）而不是构造函数里 ——
    /// 这样构造函数在 Applet 模式与完整内存模式下做的事**完全一样**，
    /// 出问题时不会因为「多建了一个 Label」而分不清是哪条路径崩的。
    void showStartupNotice();

    void setButtonsEnabled(bool enabled);
    void setCancelEnabled(bool enabled);
    void setOutputDir(const std::string& path, bool persist);

    //-------- 三个「只改文本」的显示接口（内容不变时什么都不做）--------//
    void setStatus(const std::string& text);
    void setDetail(const std::string& text);
    void setProgress(int percent);

    /// 任务进行期间每秒写一行日志，用来判断「渲染循环是否还活着」
    void heartbeat();

    /// 解析 settings.txt 原文（不读 SD 卡，内容由 main.cpp 读好传进来）
    void readSettings(const std::string& text);
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

    //-------- 就地展开的诊断信息（默认收起，点击底部的按钮才展开）--------//
    brls::Label* diagLabel        = nullptr; // 第 1 段：环境 / 路径 / 上次返回内容
    brls::Label* diagImagesLabel  = nullptr; // 第 2 段：每张图片的加载结果
    std::string  diagText;                   // 第 1 段的文本（首次展开时才生成）
    std::string  diagImagesText;             // 第 2 段的文本
    bool         diagReady = false;          // 文本是否已生成
    bool         diagOpen  = false;          // 当前是否处于展开状态

    std::vector<ImageSlot> imageSlots;
    /// 正在下载的图片下标（-1 = 没有）
    int imageLoadingIndex = -1;
    /// 启动流程是否已经跑过
    bool startupImagesStarted = false;
    int imagesLoaded = 0;
    int imagesFailed = 0;

    /// 已经加载进显存的像素总量（用来看住 Applet 模式下的显存/内存预算）
    size_t imagePixelsUsed = 0;

    /// 启动提示（构造函数只存起来，第一次轮询才显示）
    std::string startupNotice;
    bool noticeShown = false;

    /// 正在把「上次保存的值」填进输入框 —— 这期间不要回写 settings.txt
    /// （内容一样，白写一次 SD 卡；Applet 模式下 SD 卡 I/O 越少越安全）
    bool applyingSaved = false;

    /// 最近一次**真的写出去**的 settings.txt 内容；相同就不再写盘
    /// （saveSettings() 是 const，所以这里必须 mutable）
    mutable std::string lastSavedText;

    /// 页面出现后已经轮询了多少次（第一次轮询 ≈ 第一帧之后）
    int aliveTicks = 0;

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
