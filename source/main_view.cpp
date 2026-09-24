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

/// 最多加载几张图片（每张都要占显存，别放任用户写一长串）
constexpr size_t MAX_IMAGES = 6;

/// 图片显示高度（宽度由 List 给，按 FIT 缩放居中）
constexpr unsigned IMAGE_VIEW_HEIGHT = 300;

/// 诊断页里最多显示多少字节的返回内容
constexpr size_t MAX_DETAIL_BYTES = 6000;

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

/// 按「字符数」粗略截断（UTF-8 安全）
std::string truncateChars(const std::string& text, size_t maxChars)
{
    if (text.empty() || maxChars == 0)
        return "";

    size_t chars = 0;
    size_t index = 0;

    while (index < text.size())
    {
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

/// url / 本地路径 的简单判定
bool looksLikeUrl(const std::string& s)
{
    const std::string lower = [&s] {
        std::string t = s;
        for (char& c : t)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    }();

    return lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0;
}

} // namespace

//=====================================================================
// 输入框
//
// 注意：UrlInputItem / InputRow 必须放在全局作用域，不能塞进上面的匿名
// 命名空间 —— main_view.hpp 里对 InputRow 做的是全局作用域的前置声明，
// 匿名命名空间里的同名类是完全不同的类型，会编译不过。
//=====================================================================

/// 一行里的链接输入框（继承 ListItem 整段自绘：左标题 + 右圆角框内左对齐文本）
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

    /// 需要提示用户时回调（由 MainView 接到状态栏/详情区）。
    /// 刻意不用 Application::notify：通知是「多一个动画 + 多一个额外视图」，
    /// 而本版本的目标就是把界面上的动画/视图栈操作减到最少。
    std::function<void(const std::string&)> onNotice;

    bool onClick() override
    {
        //--------------------------------------------------------------
        // ⚠️ Applet 模式（从相册启动）下**不要**调系统键盘。
        //    libnx 的 swkbd 本质是「再起一个 applet」，在 applet 环境里
        //    既可能直接失败，也可能把界面卡住（真机实测过「按 A 就死机」）。
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
            // 标题宽度只测一次（两个输入框就是两次 nvgTextBounds，之后一直复用）
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

        // 用 scissor 把文本裁在输入框内：不需要任何文字测量
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
    std::string display;
    unsigned cachedLabelWidth = 0;

    void notify(const std::string& text)
    {
        if (this->onNotice)
            this->onNotice(text);
        else
            Application::notify(text);
    }
};

//=====================================================================
// 一行：输入框 + 文件图标 (+ 目录图标) + 访问按钮
//=====================================================================

class InputRow : public BoxLayout
{
  public:
    InputRow(const std::string& labelText, const std::string& placeholder, bool withFolderButton)
        : BoxLayout(BoxLayoutOrientation::HORIZONTAL)
    {
        const unsigned h = listItemHeight();

        this->setHeight(h);
        this->setSpacing(h / 6 > 8 ? h / 6 : 8);

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
        unsigned h = this->getHeight(false);
        if (h < 20)
            h = listItemHeight();

        const unsigned gap = static_cast<unsigned>(this->getSpacing());

        const unsigned iconWidth = h;
        const unsigned goWidth   = h * 2;

        const unsigned count = static_cast<unsigned>(this->getViewsCount());
        unsigned fixed       = iconWidth + goWidth + gap * (count > 0 ? count - 1 : 0);
        if (this->folderButton != nullptr)
            fixed += iconWidth;

        const unsigned total      = this->getWidth();
        const unsigned inputWidth = (total > fixed + MIN_INPUT_WIDTH) ? (total - fixed) : MIN_INPUT_WIDTH;

        // BoxLayout 的水平布局用的是各子视图「已存」的宽度，必须在调基类前定好
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
// 取消按钮
//
// borealis 判定「某视图能不能被聚焦」的唯一依据是 `getDefaultFocus()` 是否返回非空，
// 所以这里就复用它：**只有任务进行中才允许被聚焦**。
// 另外它在 List 里没人会给高度，不给就会只剩一条横线（真机截图就是这样）。
//=====================================================================
class CancelButton : public Button
{
  public:
    CancelButton()
        : Button(ButtonStyle::REGULAR)
    {
        this->setLabel("取消当前任务");

        Style* style = Application::getStyle();
        if (style != nullptr)
            this->setHeight(style->List.Item.height);
    }

    View* getDefaultFocus() override
    {
        return this->active ? this : nullptr;
    }

    /// true：有任务，显示且可聚焦；false：完全收起（不留「可以选中的横线」）
    void setActive(bool value)
    {
        this->active = value;
        this->setState(value ? ButtonState::ENABLED : ButtonState::DISABLED);

        // 用**非动画**的收起/展开：这里不需要过渡效果，也少一份动画风险
        if (value)
            this->expand(false);
        else
            this->collapse(false);
    }

  private:
    bool active = false;
};

//=====================================================================
// 诊断信息页（按键触发才 pushView，属于允许的常规路径）
//=====================================================================
class DetailsView : public List
{
  public:
    DetailsView(const std::string& imageSummary, const std::string& imageDetail)
        : List()
    {
        this->registerAction("返回", Key::B, [] {
            Application::popView();
            return true;
        });

        this->addView(new Header("诊断信息与提示", true, "按 B 返回"));

        this->addView(new Label(LabelStyle::DESCRIPTION,
            "操作：A 输入链接 · X 读取 url.txt · 文件图标选 .txt · 文件夹图标选保存目录 · "
            "十字键/左摇杆 移动 · + 退出\n"
            "下载会覆盖同名文件；进度、结果与错误都显示在主页面里，不再弹窗。",
            true));

        this->addView(new Label(LabelStyle::DESCRIPTION,
            "运行环境：" + netx::describe() + "\n"
            "日志文件：" + std::string(appcfg::LOG_FILE) + "\n"
            "项目目录：" + std::string(appcfg::PROJECT_DIR) + "\n"
            "图片缓存：" + std::string(appcfg::IMG_DIR),
            true));

        this->addView(new Label(LabelStyle::DESCRIPTION, imageSummary + "\n" + imageDetail, true));

        // 上次检查更新的完整返回内容
        std::string result;
        if (fsx::readWholeFile(appcfg::RESULT_FILE, &result) && !result.empty())
        {
            this->addView(new Label(LabelStyle::DESCRIPTION,
                "上次检查更新的完整返回内容（" + fsx::formatBytes(static_cast<s64>(result.size())) + "）：", true));
            this->addView(new Label(LabelStyle::DESCRIPTION, capText(result, MAX_DETAIL_BYTES), true));
        }
        else
        {
            this->addView(new Label(LabelStyle::DESCRIPTION,
                "还没有检查更新的记录（在主页面第一行点「访问」试一次）。", true));
        }
    }
};

//=====================================================================
// 轮询任务：把工作线程的状态搬到界面上
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
        //   不调用它，lastRun 恒为 0，节流条件恒成立，任务会退化成每帧都跑。
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
    this->readSettings();

    if (!this->savedDir.empty())
        this->outputDir = fsx::normalize(this->savedDir);

    this->addView(new Header("NX Downloader", true, "上行检查更新 · 下行下载文件"));

    this->buildRows();
    this->buildStatusArea();
    this->buildGallery();
    this->buildFooter(startupNotice);

    this->setButtonsEnabled(true);
    this->setCancelEnabled(false);

    if (!this->savedUpdate.empty())
        this->updateRow->input->setText(appcfg::normalizeUrl(this->savedUpdate));
    if (!this->savedDownload.empty())
        this->downloadRow->input->setText(appcfg::normalizeUrl(this->savedDownload));

    // 轮询任务常驻：空闲时 onPoll() 第一行就返回，代价只是每 100ms 读几个原子量
    PollTask* poll = new PollTask(this);
    poll->start();
    this->pollTask = poll;

    logx::uif("主界面已创建（图片位 %u 个）", static_cast<unsigned>(this->imageSlots.size()));
}

MainView::~MainView()
{
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
    this->updateRow = new InputRow("更新链接", "按 A 输入检查更新的链接（可留空）", false);
    this->addView(this->updateRow);

    this->downloadRow = new InputRow("下载链接", "按 A 输入要下载的文件直链", true);
    this->addView(this->downloadRow);

    const auto saveHook = [this] { this->saveSettings(); };
    this->updateRow->input->onChange   = saveHook;
    this->downloadRow->input->onChange = saveHook;

    const auto noticeHook = [this](const std::string& text) {
        this->setStatus(text);
        this->setDetail(text);
    };
    this->updateRow->input->onNotice   = noticeHook;
    this->downloadRow->input->onNotice = noticeHook;

    // X 键绑在「行」上：焦点在该行任意控件时都生效
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

void MainView::buildStatusArea()
{
    this->statusLabel = new Label(LabelStyle::REGULAR, "状态：就绪", false);
    this->shownStatus = "就绪";
    this->addView(this->statusLabel);

    this->bar = new ProgressDisplay(ProgressDisplayFlags::PERCENTAGE);
    this->bar->setHeight(60);
    this->addView(this->bar);

    CancelButton* cancel = new CancelButton();
    cancel->getClickEvent()->subscribe([this](View*) {
        if (this->downloader == nullptr)
            return;

        logx::ui("用户点了「取消当前任务」");
        this->downloader->requestCancel();
        this->setStatus("正在取消…");
    });
    cancel->setActive(false);
    this->cancelButton = cancel;
    this->addView(cancel);

    this->detailLabel = new Label(LabelStyle::DESCRIPTION,
        "（这里会显示检查更新的返回内容、以及错误详情）", true);
    this->shownDetail = "（这里会显示检查更新的返回内容、以及错误详情）";
    this->addView(this->detailLabel);
}

void MainView::buildGallery()
{
    // 从 url.txt 里取 img: 列表
    std::vector<std::string> items;

    std::string urlText;
    if (fsx::readWholeFile(appcfg::URL_FILE, &urlText))
    {
        appcfg::UrlEntry entry;
        appcfg::parseUrlFile(urlText, &entry);
        items = entry.images;
    }

    if (items.size() > MAX_IMAGES)
    {
        logx::uif("img: 配置了 %u 项，只加载前 %u 张（显存有限）",
            static_cast<unsigned>(items.size()), static_cast<unsigned>(MAX_IMAGES));
        items.resize(MAX_IMAGES);
    }

    this->imageLabel = new Label(LabelStyle::DESCRIPTION,
        items.empty() ? "图片：未配置（在 url.txt 里用 img: 指定，逗号分隔）" : "图片：准备中…", true);
    this->addView(this->imageLabel);

    for (size_t i = 0; i < items.size(); i++)
    {
        ImageSlot slot;
        slot.item = items[i];

        slot.view = new Image();
        slot.view->setHeight(IMAGE_VIEW_HEIGHT);
        slot.view->setScaleType(ImageScaleType::FIT);
        // 先收起：等图片真的加载出来再展开，避免失败时留下一大片空白
        slot.view->collapse(false);

        this->addView(slot.view);
        this->imageSlots.push_back(slot);
    }

    if (!items.empty())
        logx::uif("img: 共 %u 项", static_cast<unsigned>(items.size()));
}

void MainView::buildFooter(const std::string& startupNotice)
{
    this->dirLabel = new Label(LabelStyle::DESCRIPTION,
        "下载保存到：" + this->outputDir + "（第二行的文件夹图标可更换）", true);
    this->addView(this->dirLabel);

    if (!startupNotice.empty())
        this->addView(new Label(LabelStyle::DESCRIPTION, startupNotice, true));

    //-----------------------------------------------------------------
    // ★ 底部锚点：borealis 的 List 只会「滚动到当前焦点」，页面最底下如果
    //   没有可聚焦的控件，再往下就永远滚不动（实测就是「底部内容看不全」）。
    //   这个按钮既是有用的入口，也顺手把滚动范围撑到底。
    //-----------------------------------------------------------------
    this->detailsButton = new Button(ButtonStyle::REGULAR);
    this->detailsButton->setLabel("诊断信息与提示");
    this->detailsButton->setHeight(listItemHeight());
    this->detailsButton->getClickEvent()->subscribe([this](View*) {
        this->openDetailsView();
    });
    this->addView(this->detailsButton);
}

//------------------------------ 只改文本的显示接口 ------------------------------//

void MainView::setStatus(const std::string& text)
{
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

    // borealis 的 Label::setText 只把「父级」标脏，自己不重算高度 →
    // 文本变长后会压到下面的控件上。这里把自己也标脏（只能用非立即模式）。
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
    if (this->cancelButton == nullptr)
        return;

    static_cast<CancelButton*>(this->cancelButton)->setActive(enabled);

    // 收起/展开改变了这一行占的高度，必须让 List 重新排版（invalidate 不会自动上溯）
    this->invalidate();
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

//------------------------------ 图片 ------------------------------//

void MainView::loadImageIntoSlot(size_t index, const std::string& path)
{
    if (index >= this->imageSlots.size())
        return;

    ImageSlot& slot = this->imageSlots[index];

    slot.cached = path;
    slot.view->setImage(path);
    slot.view->expand(false); // 展开成固定高度，交给 FIT 缩放

    slot.loaded  = true;
    slot.pending = false;

    this->imagesLoaded++;

    // ⚠️ borealis 的 `View::invalidate(bool)` **不会**往上通知父级
    //    （它只置自己的 dirty）。展开/换图之后必须显式让 List 重新排版，
    //    否则这一格仍然按 0 高度排位，图片会盖在下面的控件上。
    this->invalidate();

    logx::uif("图片 %u 已加载：%s", static_cast<unsigned>(index + 1), path.c_str());
}

void MainView::refreshImageSummary()
{
    if (this->imageLabel == nullptr)
        return;

    if (this->imageSlots.empty())
    {
        this->imageLabel->setText("图片：未配置（在 url.txt 里用 img: 指定，逗号分隔）");
        return;
    }

    std::string text = "图片：" + std::to_string(this->imagesLoaded) + "/" +
                       std::to_string(this->imageSlots.size()) + " 已加载";

    if (this->imagesFailed > 0)
        text += "，" + std::to_string(this->imagesFailed) + " 张失败";

    if (this->task == Task::Images && this->imageLoadingIndex >= 0)
        text += "（正在下载第 " + std::to_string(this->imageLoadingIndex + 1) + " 张）";

    this->imageLabel->setText(text);
    this->imageLabel->invalidate(false);
}

void MainView::startStartupImageLoad()
{
    this->startupImagesStarted = true;

    if (this->imageSlots.empty())
        return;

    // 先把「本地已经有」的图片直接加载进来（不联网）
    for (size_t i = 0; i < this->imageSlots.size(); i++)
    {
        ImageSlot& slot = this->imageSlots[i];

        if (looksLikeUrl(slot.item))
        {
            // 有缓存就直接用：省一次下载，也让「启动即显示」成立
            const std::string cached = fsx::join(appcfg::IMG_DIR, appcfg::imageFileName(i, slot.item));

            if (fsx::exists(cached))
                this->loadImageIntoSlot(i, cached);
            else
                slot.pending = true;

            continue;
        }

        // 不是链接 → 当成本地文件名：先按原文，再按项目目录，最后按 SD 根目录找
        std::string local = slot.item;

        if (!fsx::exists(local))
        {
            const std::string inProject = fsx::join(appcfg::PROJECT_DIR, slot.item);
            const std::string inRoot    = fsx::join("sdmc:/", slot.item);

            if (fsx::exists(inProject))
                local = inProject;
            else if (fsx::exists(inRoot))
                local = inRoot;
            else
                local.clear();
        }

        if (!local.empty())
        {
            this->loadImageIntoSlot(i, local);
        }
        else
        {
            logx::uif("图片 %u 找不到本地文件：%s", static_cast<unsigned>(i + 1), slot.item.c_str());
            this->imagesFailed++;
        }
    }

    this->refreshImageSummary();

    // 再处理需要联网的（没有网络就跳过，别白等 15s 超时）
    bool anyPending = false;
    for (const ImageSlot& slot : this->imageSlots)
        anyPending = anyPending || slot.pending;

    if (!anyPending)
        return;

    if (!netx::ready() && !netx::init())
    {
        logx::ui("网络不可用：跳过图片下载");
        this->imagesFailed += static_cast<int>(std::count_if(this->imageSlots.begin(), this->imageSlots.end(),
            [](const ImageSlot& s) { return s.pending; }));
        this->refreshImageSummary();
        this->setDetail("网络不可用，图片没能下载（其它功能仍可使用）。");
        return;
    }

    this->startNextPendingImage();
}

bool MainView::startNextPendingImage()
{
    for (size_t i = 0; i < this->imageSlots.size(); i++)
    {
        ImageSlot& slot = this->imageSlots[i];

        if (!slot.pending || slot.loaded)
            continue;

        const std::string fileName = appcfg::imageFileName(i, slot.item);

        if (!this->downloader->startAs(slot.item, appcfg::IMG_DIR, fileName))
        {
            logx::uif("图片 %u 无法开始下载：%s", static_cast<unsigned>(i + 1), slot.item.c_str());
            slot.pending = false;
            this->imagesFailed++;
            continue;
        }

        this->imageLoadingIndex = static_cast<int>(i);

        logx::uif("开始下载图片 %u/%u：%s",
            static_cast<unsigned>(i + 1), static_cast<unsigned>(this->imageSlots.size()), slot.item.c_str());

        this->beginTask(Task::Images, "正在下载图片 " + std::to_string(i + 1) + "/" +
                                          std::to_string(this->imageSlots.size()));
        this->refreshImageSummary();
        return true;
    }

    this->imageLoadingIndex = -1;
    return false;
}

//------------------------------ 文件 / 目录选择 ------------------------------//

void MainView::openTextFilePicker(bool forUpdate)
{
    const std::string startPath = fsx::isDirectory(appcfg::PROJECT_DIR) ? appcfg::PROJECT_DIR : "sdmc:/";

    TextFilePickerView* picker = new TextFilePickerView(startPath, [this, forUpdate](const std::string& path) {
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

void MainView::openDetailsView()
{
    std::string summary = "图片：未配置";
    std::string detail  = "（在 " + std::string(appcfg::URL_FILE) +
                          " 里用 img: 写图片链接或文件名，逗号分隔；重启程序后生效）";

    if (!this->imageSlots.empty())
    {
        summary = "图片：" + std::to_string(this->imagesLoaded) + "/" +
                  std::to_string(this->imageSlots.size()) + " 已加载";
        if (this->imagesFailed > 0)
            summary += "，" + std::to_string(this->imagesFailed) + " 张失败";

        detail.clear();
        for (size_t i = 0; i < this->imageSlots.size(); i++)
        {
            const ImageSlot& slot = this->imageSlots[i];

            detail += std::to_string(i + 1) + ". " + slot.item + "\n";
            if (slot.loaded)
                detail += "   → 已加载：" + slot.cached + "\n";
            else
                detail += "   → 未加载\n";
        }
    }

    Application::pushView(new DetailsView(summary, detail));
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
    entry.update   = this->updateRow->input->text();
    entry.download = this->downloadRow->input->text();

    if (!appcfg::parseUrlFile(text, &entry))
    {
        logx::uif("解析失败：%s", filePath.c_str());
        this->setStatus("解析失败");
        this->setDetail("没有从文件里找到 updata / download / img。\n\n文件：\n" + filePath +
                        "\n\n期望格式：\n{ updata: 链接1 , download: 链接2 , img: a.jpg, b.jpg }");
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

    if (kind != Task::Images)
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
        this->setStatus(this->task == Task::Images ? "正在下载图片，请稍候（可点取消）" : "已有任务在进行");
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
        this->setStatus(this->task == Task::Images ? "正在下载图片，请稍候（可点取消）" : "已有任务在进行");
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

    // 启动后第一次轮询：加载图片（界面这时已经画出来了）
    if (!this->startupImagesStarted)
    {
        this->startStartupImageLoad();
        return;
    }

    const Downloader::State state = this->downloader->state();

    if (state == Downloader::State::Idle)
        return;

    if (state == Downloader::State::Running)
    {
        if (this->task == Task::Update)
        {
            this->setStatus("正在检查更新…");
        }
        else if (this->task == Task::Images)
        {
            this->setProgress(this->downloader->percent());
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

    // 终态：靠 task 判断这一轮是否已经收尾过（轮询任务常驻，不能重复收尾）
    if (this->task == Task::None)
        return;

    if (this->task == Task::Images)
        this->finishImageTask(state);
    else
        this->finishTask(state);
}

void MainView::finishImageTask(Downloader::State state)
{
    const int index = this->imageLoadingIndex;
    this->imageLoadingIndex = -1;

    logx::uif("图片下载收尾：index=%d state=%d", index, static_cast<int>(state));

    if (state == Downloader::State::Finished && index >= 0)
    {
        const std::string path = this->downloader->outputPath();

        if (!path.empty())
            this->loadImageIntoSlot(static_cast<size_t>(index), path);
        else
            this->imagesFailed++;
    }
    else if (index >= 0)
    {
        this->imagesFailed++;
        logx::uif("图片 %d 下载未成功：%s", index + 1, this->downloader->errorText().c_str());
    }

    this->refreshImageSummary();

    // 用户取消 → 整个批量就此收住，别继续下一张
    if (state == Downloader::State::Cancelled)
    {
        size_t remaining = 0;
        for (ImageSlot& slot : this->imageSlots)
        {
            if (slot.pending && !slot.loaded)
            {
                slot.pending = false;
                remaining++;
            }
        }

        this->task = Task::None;
        this->setButtonsEnabled(true);
        this->setCancelEnabled(false);
        this->setProgress(0);
        this->setStatus("图片下载已取消");
        this->setDetail("已取消，还剩 " + std::to_string(remaining) + " 张没下载。下次启动会继续。");
        logx::ui("图片批量下载被取消");
        return;
    }

    // 有失败就先停：多半是网络/地址的问题，继续试只会让用户干等
    if (state != Downloader::State::Finished)
    {
        this->task = Task::None;
        this->setButtonsEnabled(true);
        this->setCancelEnabled(false);
        this->setProgress(0);
        this->setStatus("图片下载中断");
        this->setDetail("第 " + std::to_string(index + 1) + " 张失败：" +
                        this->downloader->errorText() + "\n\n已加载成功的图片会保留，下次启动会重试其余的。");
        return;
    }

    if (this->startNextPendingImage())
        return;

    // 全部处理完
    this->task = Task::None;
    this->setButtonsEnabled(true);
    this->setCancelEnabled(false);
    this->setProgress(100);
    this->setStatus("图片已加载 " + std::to_string(this->imagesLoaded) + "/" +
                    std::to_string(this->imageSlots.size()));

    if (this->imagesFailed > 0)
        this->setDetail("有 " + std::to_string(this->imagesFailed) + " 张图片没能加载（详见日志）。");
    else
        this->setDetail("图片都在下面了。要换图就改 " + std::string(appcfg::URL_FILE) +
                        " 里的 img: 行，然后重启程序。");

    logx::ui("图片批量下载完成");
}

void MainView::finishTask(Downloader::State state)
{
    const Task kind = this->task;
    this->task      = Task::None;

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

            this->setStatus("检查完成：HTTP " + std::to_string(code) + " · " +
                            fsx::formatBytes(static_cast<s64>(all.size())));
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
