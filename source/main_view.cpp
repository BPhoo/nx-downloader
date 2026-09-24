#include "main_view.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <nanovg/nanovg.h>

#include "file_picker.hpp"
#include "fsx.hpp"
#include "logx.hpp"
#include "netx.hpp"
#include "path_picker.hpp"

using namespace brls;

//=====================================================================
// 常量与工具
//=====================================================================
namespace
{

/// 输入框最长 100 字符（需求规定的上限，没有下限）
constexpr int MAX_URL_LENGTH = 100;

/// 输入框里最多画多少个字符（超出只画省略号；真正的溢出由 scissor 裁掉）
constexpr size_t MAX_DISPLAY_CHARS = 110;

/// 输入框的宽度下限，保证极端布局下也不会出现 0 宽控件
constexpr unsigned MIN_INPUT_WIDTH = 200;

/// 详情区最多显示多少字节（完整内容会另存到 RESULT_FILE，不会丢）
constexpr size_t MAX_RESULT_DISPLAY = 2000;

unsigned listItemHeight()
{
    Style* style = Application::getStyle();
    return style != nullptr ? style->List.Item.height : 70;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string stripLineBreaks(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c != '\n' && c != '\r')
            out.push_back(c);
    }
    return out;
}

/// 按「字符数」粗略截断（UTF-8 安全）。
///
/// 为什么不用 nanovg 的 nvgTextBounds 精确测量：
///   v2.0.0 是在 draw() 里每帧调用 nvgTextBounds 做二分截断的。
///   那会在绘制过程中反复触发 fontstash 的字形查找/图集刷新，
///   在 Applet 模式下是不必要的风险。这里改成在 setText() 时按字符数
///   预先截断好，draw() 只做一次 nvgText，零测量、零分配。
std::string truncateChars(const std::string& text, size_t maxChars)
{
    if (text.empty() || maxChars == 0)
        return "";

    size_t chars = 0;
    size_t index = 0;

    while (index < text.size())
    {
        // 前进一个完整的 UTF-8 码点
        index++;
        while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80)
            index++;

        chars++;
        if (chars >= maxChars)
            break;
    }

    if (index >= text.size())
        return text;

    return text.substr(0, index) + "…";
}

/// 按字节上限裁剪（同样不切坏 UTF-8）
std::string capText(const std::string& text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return text;

    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        cut--;

    return text.substr(0, cut) + "\n\n…（内容过长，完整内容见 " + std::string(appcfg::RESULT_FILE) + "）";
}

} // namespace

//=====================================================================
// 输入框
//
// 注意：UrlInputItem / InputRow 必须放在全局作用域，不能塞进上面的匿名
// 命名空间 —— main_view.hpp 里对 InputRow 做的是全局作用域的前置声明，
// 匿名命名空间里的同名类是完全不同的类型，会编译不过。
//=====================================================================

/// 一行里的链接输入框。
///
/// 为什么不用 ListItem 自带的 value：value 是右对齐绘制的，链接动辄上百字符，
/// 右对齐会让文字一路向左溢出、盖住左边的标题。这里整段自绘：
///   * 左边一个标题
///   * 右边一个圆角框，内容是左对齐、超长截断的链接文本
class UrlInputItem : public ListItem
{
  public:
    UrlInputItem(const std::string& labelText, const std::string& placeholder)
        : ListItem(labelText, "")
        , placeholder(placeholder)
    {
        this->setDrawTopSeparator(false);
    }

    /// 内容被改动时回调（用来把设置落盘）
    std::function<void()> onChange;

    /// 需要提示用户时回调（MainView 会把它接到状态栏/详情区）。
    /// 刻意不用 Application::notify：通知同样是「多一个动画 + 多一个额外视图」，
    /// 而本版本的目标就是把界面上的动画/视图栈操作减到最少。
    std::function<void(const std::string&)> onNotice;

    bool onClick() override
    {
        //--------------------------------------------------------------
        // ⚠️ Applet 模式（从相册启动）下**不要**调系统键盘。
        //    libnx 的 swkbd 本质是「再起一个 applet」，在 applet 环境里
        //    既可能直接失败，也可能把界面卡住（真机实测过「按 A 就死机」）。
        //    真要用键盘，必须按住 R 键从游戏图标启动（完整内存模式）。
        //--------------------------------------------------------------
        if (appletGetAppletType() != AppletType_Application)
        {
            this->notify("Applet 模式下不能用系统键盘（有死机风险）：请按 X 读取 url.txt，"
                         "或按住 R 键从游戏图标启动以获得完整功能");
            return true;
        }

        const std::string initial = this->url;

        const bool ok = Swkbd::openForText(
            [this](std::string value) { this->setText(value); },
            this->label,
            this->placeholder,
            MAX_URL_LENGTH,
            initial);

        if (!ok)
            this->notify("系统键盘没能打开：可按 X 或点文件图标从 url.txt 读取");

        return true;
    }

    /// 有提示就转给 MainView；万一没接回调，退回 borealis 的通知
    /// （宁可多一个通知，也不能把提示静默吞掉）
    void notify(const std::string& text)
    {
        if (this->onNotice)
            this->onNotice(text);
        else
            Application::notify(text);
    }

    void setText(const std::string& value)
    {
        this->url = stripLineBreaks(value);

        // 显示用文本在这里一次算好，draw() 里不再做任何测量/分配
        this->display = truncateChars(this->url, MAX_DISPLAY_CHARS);

        this->invalidate();

        if (this->onChange)
            this->onChange();
    }

    const std::string& text() const
    {
        return this->url;
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style, FrameContext* ctx) override
    {
        const unsigned padding = style->List.Item.padding;
        const int midY         = y + static_cast<int>(height / 2);

        //------------------------- 左侧标题 -------------------------
        nvgFontFaceId(vg, ctx->fontStash->regular);
        nvgFontSize(vg, this->textSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, ctx->theme->textColor);

        unsigned labelWidth = this->cachedLabelWidth;
        if (!this->label.empty())
        {
            // 标题宽度只测一次（两个输入框就是两次 nvgTextBounds，之后一直复用）。
            // v2.0.0 是每帧都测，还会为截断反复测 —— 那属于没必要的风险。
            if (labelWidth == 0)
            {
                float bounds[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                nvgTextBounds(vg, 0.0f, 0.0f, this->label.c_str(), nullptr, bounds);
                labelWidth             = static_cast<unsigned>(bounds[2] - bounds[0]) + 2;
                this->cachedLabelWidth = labelWidth;
            }

            nvgBeginPath(vg);
            nvgText(vg, static_cast<float>(x + padding), static_cast<float>(midY), this->label.c_str(), nullptr);
        }

        //------------------------- 右侧输入框 -------------------------
        const unsigned boxLeft = x + padding + labelWidth + padding;
        if (boxLeft + padding + 80 >= x + width)
            return; // 空间不够就不画了，避免出现负宽度

        const unsigned boxWidth  = (x + width - padding) - boxLeft;
        const unsigned boxHeight = height - height / 4;
        const int boxTop         = y + static_cast<int>((height - boxHeight) / 2);
        const float radius       = static_cast<float>(boxHeight) / 4.0f;

        // 底：借用分隔线色做一层很淡的填充，深/浅主题下都能看见
        nvgFillColor(vg, ctx->theme->listItemSeparatorColor);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, static_cast<float>(boxLeft), static_cast<float>(boxTop),
            static_cast<float>(boxWidth), static_cast<float>(boxHeight), radius);
        nvgFill(vg);

        //------------------------- 内容 -------------------------
        const unsigned textLeft = boxHeight / 4;

        const bool empty        = this->url.empty();
        const std::string& body = empty ? this->placeholder : this->display;

        nvgFontSize(vg, style->List.Item.valueSize);
        nvgFontFaceId(vg, ctx->fontStash->regular);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, empty ? ctx->theme->descriptionColor : ctx->theme->listItemValueColor);

        // 用 scissor 把文本裁在输入框内：不需要任何文字测量，
        // 超长链接也只会被干净地切掉，不会溢出到图标/按钮上。
        nvgSave(vg);
        nvgScissor(vg,
            static_cast<float>(boxLeft + textLeft),
            static_cast<float>(boxTop),
            static_cast<float>(boxWidth - textLeft * 2),
            static_cast<float>(boxHeight));

        nvgBeginPath(vg);
        nvgText(vg, static_cast<float>(boxLeft + textLeft),
            static_cast<float>(boxTop + static_cast<int>(boxHeight / 2)),
            body.c_str(), nullptr);

        nvgRestore(vg);
    }

  private:
    std::string url;
    std::string placeholder;
    /// 已按字符数截断的显示文本（draw 直接用）
    std::string display;
    /// 标题宽度缓存（首次绘制时测一次）
    unsigned cachedLabelWidth = 0;
};

//=====================================================================
// 一行：输入框 + 文件图标 (+ 目录图标) + 访问按钮
//=====================================================================

class InputRow : public BoxLayout
{
  public:
    /// withFolderButton：只有「下载」行才需要选保存目录的图标
    InputRow(const std::string& labelText, const std::string& placeholder, bool withFolderButton)
        : BoxLayout(BoxLayoutOrientation::HORIZONTAL)
    {
        const unsigned h = listItemHeight();

        this->setHeight(h);
        // 行高 / 6 ≈ 11，兜底不小于 8，别出现 0 间距
        this->setSpacing(h / 6 > 8 ? h / 6 : 8);

        // 顺序就是左右顺序。输入框排最前，宽度在 layout() 里根据剩余空间算。
        this->input = new UrlInputItem(labelText, placeholder);
        this->addView(this->input);

        this->fileButton = new Button(ButtonStyle::REGULAR);
        this->fileButton->setImage(BOREALIS_ASSET("icon/txt.png"));
        this->addView(this->fileButton);

        if (withFolderButton)
        {
            this->folderButton = new Button(ButtonStyle::REGULAR);
            this->folderButton->setImage(BOREALIS_ASSET("icon/folder.png"));
            this->addView(this->folderButton);
        }

        this->goButton = new Button(ButtonStyle::PRIMARY);
        this->goButton->setLabel("访问");
        this->addView(this->goButton);
    }

    void layout(NVGcontext* vg, Style* style, FontStash* stash) override
    {
        // 用 getHeight(false) 拿「原始高度」，不要用 getHeight()：
        // 后者会乘上 collapseState（滚动出屏时会动画到 0），
        // 高度为 0 时子视图宽度会被算成 0，白白多出一堆边界情况。
        unsigned h = this->getHeight(false);

        // 兜底：万一高度还没被父级设好（或异常为 0），退回样式表里的行高
        if (h < 20)
            h = listItemHeight();

        const unsigned gap = static_cast<unsigned>(this->getSpacing());

        // 图标按钮做成正方形，访问按钮稍宽（按行高成比例，跟着系统 UI 缩放走）
        const unsigned iconWidth = h;
        const unsigned goWidth   = h * 2;

        const unsigned count = static_cast<unsigned>(this->getViewsCount());
        unsigned fixed       = iconWidth + goWidth + gap * (count > 0 ? count - 1 : 0);
        if (this->folderButton != nullptr)
            fixed += iconWidth;

        const unsigned total      = this->getWidth();
        const unsigned inputWidth = (total > fixed + MIN_INPUT_WIDTH) ? (total - fixed) : MIN_INPUT_WIDTH;

        // BoxLayout 的水平布局用的是各子视图「已存」的宽度，
        // 所以必须在调基类之前把宽度都定下来。
        this->input->setWidth(inputWidth);
        this->fileButton->setWidth(iconWidth);
        if (this->folderButton != nullptr)
            this->folderButton->setWidth(iconWidth);
        this->goButton->setWidth(goWidth);

        BoxLayout::layout(vg, style, stash);
    }

    UrlInputItem* input  = nullptr;
    Button* fileButton   = nullptr;
    Button* folderButton = nullptr;
    Button* goButton     = nullptr;
};

namespace
{

//=====================================================================
// 轮询任务：把工作线程的状态搬到界面上
//
// 它只做一件事：调用 view->onPoll()。
// onPoll() 内部只会改 Label 文本 —— 不碰视图栈、不弹窗、不发通知。
//=====================================================================
class PollTask : public RepeatingTask
{
  public:
    explicit PollTask(MainView* view)
        : RepeatingTask(100)
        , view(view)
    {
    }

    /// 视图被销毁时调用：之后即使再被调度也不会碰这个对象
    void detach()
    {
        this->view = nullptr;
    }

    void run(retro_time_t currentTime) override
    {
        // ★ 必须调用基类实现：`lastRun = currentTime` 就在里面。
        //   不调用它，lastRun 恒为 0，下面的节流条件恒成立，
        //   任务会退化成「每帧都跑」（60 次/秒）。
        RepeatingTask::run(currentTime);

        if (this->view != nullptr)
            this->view->onPoll();
    }

  private:
    MainView* view;
};

} // namespace

//=====================================================================
// MainView
//=====================================================================

MainView::MainView(Downloader* downloader, const std::string& startupNotice)
    : List()
    , downloader(downloader)
{
    // 控件还没建，先从 settings.txt 把上次的值取出来暂存
    this->readSettings();

    if (!this->savedDir.empty())
        this->outputDir = fsx::normalize(this->savedDir);

    this->addView(new Header("NX Downloader", true, "上行检查更新 · 下行下载文件"));

    this->buildRows();

    //------------------------- 状态区 -------------------------//
    // 全部做成常驻控件：任何时候都只是改文本，不增删视图、不弹窗。
    this->statusLabel = new Label(LabelStyle::REGULAR, "状态：就绪", false);
    this->shownStatus = "就绪";
    this->addView(this->statusLabel);

    this->bar = new ProgressDisplay(ProgressDisplayFlags::PERCENTAGE);
    this->bar->setHeight(60);
    this->addView(this->bar);

    this->cancelButton = new Button(ButtonStyle::REGULAR);
    this->cancelButton->setLabel("取消当前任务");
    this->cancelButton->setState(ButtonState::DISABLED);
    this->cancelButton->getClickEvent()->subscribe([this](View*) {
        if (this->downloader == nullptr)
            return;

        logx::ui("用户点了「取消当前任务」");
        this->downloader->requestCancel();
        this->setStatus("正在取消…");
    });
    this->addView(this->cancelButton);

    //------------------------- 详情区 -------------------------//
    const std::string detailText = startupNotice.empty()
        ? "（这里会显示检查更新的返回内容、以及错误详情）"
        : startupNotice;

    this->detailLabel = new Label(LabelStyle::DESCRIPTION, detailText, true);
    this->shownDetail  = detailText;
    this->addView(this->detailLabel);

    this->dirLabel = new Label(LabelStyle::DESCRIPTION,
        "下载保存到：" + this->outputDir + "（第二行的文件夹图标可更换）", true);
    this->addView(this->dirLabel);

    Label* tip = new Label(LabelStyle::DESCRIPTION,
        "A 输入链接（最长 100 字符）· X 读取 " + std::string(appcfg::URL_FILE) +
            " 里对应的值 · 文件图标可挑选任意 .txt\n"
            "同名文件会被直接覆盖；进度、结果与错误都显示在本页，不再弹窗。",
        true);
    this->addView(tip);

    // 运行环境诊断行：出问题时这一行就能看出是哪一环不对，
    // 细节（含 libnx 的 Result 错误码）在 log.txt 里。
    Label* diag = new Label(LabelStyle::DESCRIPTION,
        "环境：" + netx::describe() + "\n日志：" + std::string(appcfg::LOG_FILE), true);
    this->addView(diag);

    this->setButtonsEnabled(true);
    this->setCancelEnabled(false);

    // 控件都建好之后再回填上次保存的内容
    if (!this->savedUpdate.empty())
        this->updateRow->input->setText(appcfg::normalizeUrl(this->savedUpdate));
    if (!this->savedDownload.empty())
        this->downloadRow->input->setText(appcfg::normalizeUrl(this->savedDownload));

    //------------------------- 轮询任务 -------------------------//
    // 常驻：空闲时 onPoll() 会在第一行直接返回，代价只是每 100ms 读几个原子量。
    // 这样做是为了彻底避免「任务对象的创建/回收」这类生命周期问题。
    PollTask* poll = new PollTask(this);
    poll->start();
    this->pollTask = poll;

    logx::ui("主界面已创建");
}

MainView::~MainView()
{
    // 轮询任务常驻在任务管理器里，必须先断开它对 this 的引用，
    // 否则本对象销毁之后任务再被调度一次就会访问已释放内存。
    if (this->pollTask != nullptr)
    {
        static_cast<PollTask*>(this->pollTask)->detach();
        this->pollTask->stop();
        this->pollTask = nullptr;
    }
}

//------------------------------ 界面搭建 ------------------------------//

void MainView::buildRows()
{
    //=========================== 第一行：检查更新 ===========================//
    this->updateRow = new InputRow("更新链接", "按 A 输入检查更新的链接（可留空）", false);
    this->addView(this->updateRow);

    //=========================== 第二行：下载文件 ===========================//
    this->downloadRow = new InputRow("下载链接", "按 A 输入要下载的文件直链", true);
    this->addView(this->downloadRow);

    // 每行的输入框改动后都落盘
    const auto saveHook = [this] { this->saveSettings(); };
    this->updateRow->input->onChange   = saveHook;
    this->downloadRow->input->onChange = saveHook;

    // 输入框自己产生的提示（例如「Applet 模式不能用系统键盘」）统一转到这里显示
    const auto noticeHook = [this](const std::string& text) {
        this->setStatus(text);
        this->setDetail(text);
    };
    this->updateRow->input->onNotice   = noticeHook;
    this->downloadRow->input->onNotice = noticeHook;

    // X 键绑定在「行」上：焦点在该行的任意控件（输入框 / 图标 / 访问按钮）时都生效
    this->updateRow->registerAction("读 url.txt", Key::X, [this] {
        this->loadFromProjectUrlFile(true);
        return true;
    });

    this->downloadRow->registerAction("读 url.txt", Key::X, [this] {
        this->loadFromProjectUrlFile(false);
        return true;
    });

    this->updateRow->fileButton->getClickEvent()->subscribe([this](View*) {
        this->openTextFilePicker(true);
    });

    this->downloadRow->fileButton->getClickEvent()->subscribe([this](View*) {
        this->openTextFilePicker(false);
    });

    this->downloadRow->folderButton->getClickEvent()->subscribe([this](View*) {
        this->openOutputDirPicker();
    });

    this->updateRow->goButton->getClickEvent()->subscribe([this](View*) {
        this->startUpdateCheck();
    });

    this->downloadRow->goButton->getClickEvent()->subscribe([this](View*) {
        this->startDownload();
    });
}

//------------------------------ 只改文本的显示接口 ------------------------------//

void MainView::setStatus(const std::string& text)
{
    // 内容没变就不碰控件：setText 会让父级（这个 List）置脏，
    // 白白触发一次全量布局。
    if (this->shownStatus == text)
        return;

    this->shownStatus = text;

    if (this->statusLabel != nullptr)
        this->statusLabel->setText("状态：" + text);
}

void MainView::setDetail(const std::string& text)
{
    if (this->shownDetail == text)
        return;

    this->shownDetail = text;

    if (this->detailLabel == nullptr)
        return;

    this->detailLabel->setText(text);

    // ⚠️ borealis 的 Label::setText 只把「父级」标脏，自己不会重新测量高度，
    //    于是文本变长后高度不更新，会压到下面的控件上。
    //    这里显式把自己也标脏：下一帧 List 排好宽度之后它会自己重算高度。
    //    （注意只能用非立即模式：立即布局会在绘制阶段之外调用 nvg*）
    this->detailLabel->invalidate(false);
}

void MainView::setProgress(int percent)
{
    const int clamped = std::max(0, std::min(100, percent));

    if (this->shownPercent == clamped)
        return;

    this->shownPercent = clamped;

    if (this->bar != nullptr)
        this->bar->setProgress(clamped, 100);
}

void MainView::setButtonsEnabled(bool enabled)
{
    const ButtonState state = enabled ? ButtonState::ENABLED : ButtonState::DISABLED;

    const auto apply = [state](const InputRow* row) {
        if (row == nullptr)
            return;

        row->goButton->setState(state);
        row->fileButton->setState(state);

        if (row->folderButton != nullptr)
            row->folderButton->setState(state);
    };

    apply(this->updateRow);
    apply(this->downloadRow);
}

void MainView::setCancelEnabled(bool enabled)
{
    if (this->cancelButton != nullptr)
        this->cancelButton->setState(enabled ? ButtonState::ENABLED : ButtonState::DISABLED);
}

void MainView::heartbeat()
{
    this->pollCount++;

    if (this->pollCount - this->lastHeartbeatAt < 10) // 10 × 100ms ≈ 1 秒
        return;

    this->lastHeartbeatAt = this->pollCount;

    logx::uif("轮询心跳 #%d：任务=%d 进度=%d%% 已收 %lld 字节",
        this->pollCount, static_cast<int>(this->task),
        this->downloader != nullptr ? this->downloader->percent() : 0,
        this->downloader != nullptr ? static_cast<long long>(this->downloader->downloaded()) : 0LL);
}

//------------------------------ 文件 / 目录选择 ------------------------------//

void MainView::openTextFilePicker(bool forUpdate)
{
    // 默认从项目文件夹开始找，那里就放着自动生成的 url.txt
    const std::string startPath = fsx::isDirectory(appcfg::PROJECT_DIR) ? appcfg::PROJECT_DIR : "sdmc:/";

    TextFilePickerView* picker = new TextFilePickerView(startPath, [this, forUpdate](const std::string& path) {
        // 这个回调是「选择器出栈」之后被调用的，只改文本、不动视图栈
        this->loadFromFile(forUpdate, path);
    });

    Application::pushView(picker);
}

void MainView::openOutputDirPicker()
{
    PathPickerView* picker = new PathPickerView(this->outputDir, [this](const std::string& path) {
        this->setOutputDir(path, true);
    });

    Application::pushView(picker);
}

void MainView::setOutputDir(const std::string& path, bool persist)
{
    this->outputDir = fsx::normalize(path);

    if (this->dirLabel != nullptr)
        this->dirLabel->setText("下载保存到：" + this->outputDir + "（第二行的文件夹图标可更换）");

    if (persist)
        this->saveSettings();

    logx::uif("保存目录改为：%s", this->outputDir.c_str());
    this->setStatus("保存目录已改为 " + this->outputDir);
}

//------------------------------ 从 url.txt / 任意 txt 取值 ------------------------------//

bool MainView::loadFromFile(bool forUpdate, const std::string& filePath)
{
    std::string text;
    if (!fsx::readWholeFile(filePath, &text))
    {
        logx::uif("读不到文件：%s", filePath.c_str());
        this->setStatus("读不到文件");
        this->setDetail("无法读取：\n" + filePath);
        return false;
    }

    appcfg::UrlEntry entry;

    // 带上输入框里已有的内容：文件里只写了其中一项时，另一项不会被清空
    entry.update   = this->updateRow->input->text();
    entry.download = this->downloadRow->input->text();

    if (!appcfg::parseUrlFile(text, &entry))
    {
        logx::uif("解析失败：%s", filePath.c_str());
        this->setStatus("解析失败");
        this->setDetail("没有从文件里找到 updata / download。\n\n文件：\n" + filePath +
                        "\n\n期望格式：\n{ updata: 链接1 , download: 链接2 }");
        return false;
    }

    if (forUpdate)
    {
        if (entry.update.empty())
        {
            this->setStatus("文件里没有 updata");
            this->setDetail("这个文件里没有可用于检查更新的链接。\n\n" + filePath);
            return false;
        }

        this->updateRow->input->setText(entry.update);
        logx::uif("已读取 updata：%s", entry.update.c_str());
        this->setStatus("已读取 updata");
    }
    else
    {
        if (entry.download.empty())
        {
            this->setStatus("文件里没有 download");
            this->setDetail("这个文件里没有可用于下载的链接。\n\n" + filePath);
            return false;
        }

        this->downloadRow->input->setText(entry.download);
        logx::uif("已读取 download：%s", entry.download.c_str());
        this->setStatus("已读取 download");
    }

    this->saveSettings();
    return true;
}

bool MainView::loadFromProjectUrlFile(bool forUpdate)
{
    if (this->downloader != nullptr && this->downloader->running())
        return false;

    return this->loadFromFile(forUpdate, appcfg::URL_FILE);
}

//------------------------------ 任务启动 ------------------------------//

void MainView::beginTask(Task kind, const std::string& statusText)
{
    this->task = kind;

    logx::uif("任务开始：%s", statusText.c_str());

    this->setButtonsEnabled(false);
    this->setCancelEnabled(true);
    this->setProgress(0);
    this->setStatus(statusText);
    this->setDetail("（任务进行中，完成后结果会显示在这里）");

    this->pollCount       = 0;
    this->lastHeartbeatAt = 0;
}

void MainView::startUpdateCheck()
{
    if (this->downloader == nullptr)
        return;

    const std::string url = appcfg::normalizeUrl(trim(this->updateRow->input->text()));

    if (url.empty())
    {
        this->setStatus("还没有链接");
        this->setDetail("请按 A 手动输入，或按 X 直接读取 " + std::string(appcfg::URL_FILE) + " 里的 updata。");
        return;
    }

    if (!Downloader::isSupportedUrl(url))
    {
        this->setStatus("链接无效");
        this->setDetail("只支持 http:// 与 https:// 开头的链接。\n\n当前内容：\n" + url);
        return;
    }

    if (this->task != Task::None || this->downloader->running())
    {
        this->setStatus("已有任务在进行");
        return;
    }

    if (!netx::ready() && !netx::init())
    {
        this->setStatus("网络不可用");
        this->setDetail("socket 初始化失败 " + logx::result(netx::socketError()) +
                        "（小配置重试 " + logx::result(netx::retryError()) + "）。\n\n"
                        "可以试试：\n"
                        "1) 确认主机已连上 Wi-Fi；\n"
                        "2) 按住 R 键从游戏图标启动本程序（完整内存模式）；\n"
                        "3) 重启主机后再试（上次异常退出可能还占着 bsd 服务）。\n\n"
                        "详细日志：\n" + std::string(appcfg::LOG_FILE));
        return;
    }

    logx::uif("开始检查更新: %s", url.c_str());

    this->updateRow->input->setText(url);
    this->saveSettings();

    if (!this->downloader->startFetchText(url))
    {
        this->setStatus("无法开始");
        this->setDetail("请检查链接是否正确。");
        return;
    }

    this->beginTask(Task::Update, "正在检查更新…");
}

void MainView::startDownload()
{
    if (this->downloader == nullptr)
        return;

    const std::string url = appcfg::normalizeUrl(trim(this->downloadRow->input->text()));

    if (url.empty())
    {
        this->setStatus("还没有链接");
        this->setDetail("请按 A 手动输入，或按 X 直接读取 " + std::string(appcfg::URL_FILE) + " 里的 download。");
        return;
    }

    if (!Downloader::isSupportedUrl(url))
    {
        this->setStatus("链接无效");
        this->setDetail("只支持 http:// 与 https:// 开头的链接。\n\n当前内容：\n" + url);
        return;
    }

    if (this->task != Task::None || this->downloader->running())
    {
        this->setStatus("已有任务在进行");
        return;
    }

    if (!netx::ready() && !netx::init())
    {
        this->setStatus("网络不可用");
        this->setDetail("socket 初始化失败 " + logx::result(netx::socketError()) +
                        "（小配置重试 " + logx::result(netx::retryError()) + "）。\n\n"
                        "可以试试：\n"
                        "1) 确认主机已连上 Wi-Fi；\n"
                        "2) 按住 R 键从游戏图标启动本程序（完整内存模式）；\n"
                        "3) 重启主机后再试（上次异常退出可能还占着 bsd 服务）。\n\n"
                        "详细日志：\n" + std::string(appcfg::LOG_FILE));
        return;
    }

    logx::uif("开始下载: %s", url.c_str());

    this->downloadRow->input->setText(url);
    this->saveSettings();

    this->beginDownload(url);
}

void MainView::beginDownload(const std::string& url)
{
    if (!fsx::ensureDirectory(this->outputDir))
    {
        this->setStatus("保存目录不可用");
        this->setDetail("无法创建或访问目录：\n" + this->outputDir);
        return;
    }

    std::string fileName = fsx::fileNameFromUrl(url);
    if (fileName.empty())
        fileName = "download.bin";

    const std::string target = fsx::join(this->outputDir, fileName);

    // 同名文件会被直接覆盖（写入侧是「先删再建」）。提前说清楚，
    // 免得用户以为「打开就成功了、其实没下载」。
    logx::uif("目标文件：%s（已存在=%d，将被覆盖）", target.c_str(), fsx::exists(target) ? 1 : 0);

    if (!this->downloader->start(url, this->outputDir))
    {
        this->setStatus("无法开始下载");
        this->setDetail("请检查链接与保存目录是否正确。");
        return;
    }

    this->beginTask(Task::Download, "正在下载 " + fileName);
}

//------------------------------ 轮询与收尾 ------------------------------//

void MainView::onPoll()
{
    if (this->downloader == nullptr)
        return;

    const Downloader::State state = this->downloader->state();

    if (state == Downloader::State::Idle)
        return;

    //========================= 进行中 =========================//
    if (state == Downloader::State::Running)
    {
        if (this->task == Task::Update)
        {
            // 取文本没有字节数可显示，保持一行稳定的文字即可
            this->setStatus("正在检查更新…");
        }
        else
        {
            this->setProgress(this->downloader->percent());

            const long long got = this->downloader->downloaded();
            const long long all = this->downloader->total();

            if (all > 0)
                this->setStatus("进行中 " + std::to_string(this->downloader->percent()) + "% · " +
                                fsx::formatBytes(got) + " / " + fsx::formatBytes(all));
            else
                this->setStatus("进行中 · 已接收 " + fsx::formatBytes(got));
        }

        this->heartbeat();
        return;
    }

    //========================= 终态 =========================//
    // 轮询任务常驻，所以必须靠 task 判断「这一轮是否已经收尾过」，
    // 否则会反复处理同一个结果。
    if (this->task == Task::None)
        return;

    this->finishTask(state);
}

void MainView::finishTask(Downloader::State state)
{
    const Task kind = this->task;
    this->task      = Task::None;

    // 每一步都留一条面包屑：万一真机还是卡住，日志里能直接看出卡在哪个调用上。
    logx::uif("任务收尾开始：kind=%d state=%d", static_cast<int>(kind), static_cast<int>(state));

    this->setButtonsEnabled(true);
    this->setCancelEnabled(false);

    //========================= 检查更新 =========================//
    if (kind == Task::Update)
    {
        if (state == Downloader::State::Finished)
        {
            const long code       = this->downloader->httpStatus();
            const std::string all = this->downloader->bodyText();
            logx::uif("读到返回内容 %u 字节", static_cast<unsigned>(all.size()));

            const std::string shown = capText(all, MAX_RESULT_DISPLAY);

            this->setStatus("检查完成：HTTP " + std::to_string(code) + " · " + fsx::formatBytes(static_cast<s64>(all.size())));
            logx::uif("状态行已更新");

            this->setDetail(shown.empty() ? "（服务器返回了空内容）" : shown);
            logx::uif("详情区已更新");

            this->setProgress(100);
            logx::uif("检查更新收尾完成");
            return;
        }

        std::string status = "检查更新失败";
        std::string text   = this->downloader->errorText();

        if (state == Downloader::State::Cancelled)
        {
            status = "已取消";
            text   = "检查更新已取消。";
        }
        else if (!this->downloader->bodyText().empty())
        {
            // 有些服务器在 4xx/5xx 里也返回有用的说明，一并带出来
            text += "\n\n服务器返回：\n" + capText(this->downloader->bodyText(), MAX_RESULT_DISPLAY);
        }

        if (text.empty())
            text = "未知错误（可连电脑查看 SD 卡上的 " + std::string(appcfg::LOG_FILE) + "）。";

        logx::uif("检查更新未成功：%s", text.c_str());

        this->setStatus(status);
        this->setDetail(text);
        this->setProgress(0);
        return;
    }

    //========================= 下载文件 =========================//
    switch (state)
    {
        case Downloader::State::Finished:
        {
            const std::string path = this->downloader->outputPath();

            logx::uif("下载完成：%s", path.empty() ? "(路径为空)" : path.c_str());

            this->setStatus("下载完成 · " + fsx::formatBytes(this->downloader->downloaded()));

            std::string detail = "已保存到：\n" + path;

            if (!this->downloader->tlsVerified())
                detail += "\n\n⚠ 本次连接未校验证书（设备上没有可用的 CA 证书）。"
                          "如需严格校验，可把 cacert.pem 放到 " + std::string(appcfg::PROJECT_DIR) + "/ 下。";

            this->setDetail(detail);
            this->setProgress(100);
            break;
        }

        case Downloader::State::Cancelled:
        {
            logx::ui("下载已取消");

            this->setStatus("已取消");
            this->setDetail("下载已取消，未完成的半成品文件已经删除。");
            this->setProgress(0);
            break;
        }

        default:
        {
            std::string text = this->downloader->errorText();

            if (text.empty())
                text = "未知错误（可连电脑查看 SD 卡上的 " + std::string(appcfg::LOG_FILE) + "）。";

            logx::uif("下载失败：%s", text.c_str());

            this->setStatus("下载失败");
            this->setDetail(text);
            this->setProgress(0);
            break;
        }
    }
}

//------------------------------ 设置持久化 ------------------------------//

void MainView::readSettings()
{
    this->savedUpdate.clear();
    this->savedDownload.clear();
    this->savedDir.clear();

    std::string text;
    if (!fsx::readWholeFile(appcfg::SETTINGS_FILE, &text))
        return;

    const auto takeValue = [](const std::string& entry, const char* key, std::string* out) {
        const size_t length = std::strlen(key);
        if (entry.size() >= length && entry.compare(0, length, key) == 0)
            *out = trim(entry.substr(length));
    };

    std::string line;
    for (size_t i = 0; i <= text.size(); i++)
    {
        if (i == text.size() || text[i] == '\n')
        {
            const std::string entry = trim(line);
            line.clear();

            takeValue(entry, "update=", &this->savedUpdate);
            takeValue(entry, "download=", &this->savedDownload);
            takeValue(entry, "dir=", &this->savedDir);

            continue;
        }

        line.push_back(text[i]);
    }
}

void MainView::saveSettings() const
{
    if (this->updateRow == nullptr || this->downloadRow == nullptr)
        return;

    std::string data;
    data += "# NX Downloader 设置（界面里改过之后会自动写回这里）\n";
    data += "update=" + this->updateRow->input->text() + "\n";
    data += "download=" + this->downloadRow->input->text() + "\n";
    data += "dir=" + this->outputDir + "\n";

    if (!fsx::ensureDirectory(appcfg::PROJECT_DIR))
        return;

    fsx::writeWholeFile(appcfg::SETTINGS_FILE, data);
}
