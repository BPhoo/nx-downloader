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

/// 图片显示高度（宽度由 List 给，按等比缩放居中）
constexpr unsigned IMAGE_VIEW_HEIGHT = 300;

/// 单张图片文件的字节上限：超过就跳过（绝不截断 —— 半张图比报错更难查）
constexpr size_t MAX_IMAGE_BYTES = 12u * 1024u * 1024u;

/// 单张图片的像素上限（4M 像素 ≈ 16MB 显存）。相机大图能到 8~12M 像素，
/// 直接拒掉远好过把显存/内存吃光（Applet 模式内存小得多）。
constexpr size_t MAX_IMAGE_PIXELS_EACH = 4u * 1024u * 1024u;

/// 图片占用的**总**像素预算：完整内存模式可以放宽，Applet 模式必须保守
constexpr size_t MAX_IMAGE_PIXELS_FULL   = 16u * 1024u * 1024u;
constexpr size_t MAX_IMAGE_PIXELS_APPLET = 6u * 1024u * 1024u;

/// 图片圆角
constexpr float IMAGE_CORNER_RADIUS = 8.0f;

// （v2.7.0 起诊断信息改为「就地展开」，两段加起来只有几百字节；
//   不再有「诊断页里塞 6000 字节返回内容」这种做法 ——
//   完整返回内容在 update_result.txt，完整过程在 log.txt。）

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

/// 图片占用的总像素预算（Applet 模式内存小得多，必须更保守）
size_t imagePixelBudget()
{
    return netx::fullMemoryMode() ? MAX_IMAGE_PIXELS_FULL : MAX_IMAGE_PIXELS_APPLET;
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
        this->addView(this->fileButton);

        if (withFolderButton)
        {
            this->folderButton = new Button(ButtonStyle::REGULAR);
            this->addView(this->folderButton);
        }

        this->goButton = new Button(ButtonStyle::PRIMARY);
        this->goButton->setLabel("访问");
        this->addView(this->goButton);

        //---------------------------------------------------------------------------
        // ★ 这里刻意**不**加载图标。
        //
        //   Button::setImage() 会立刻创建 GL 纹理（nvgCreateImage → glGenTextures /
        //   glTexImage2D），而 MainView 的构造函数是本程序唯一会在「第一帧之前」
        //   碰 GPU 的地方 —— 真机 Applet 模式（从相册启动）下，恰恰就是在这段区间里
        //   一启动就死机（日志停在构造函数之前/之中，一条构造日志都没有）。
        //   改成首帧之后再补图标（肉眼完全看不出差别），构造函数里就彻底不碰 GPU 了。
        //   加载结果同样写进日志：这样「GL 调用到底安不安全」下次就有结论了。
        //---------------------------------------------------------------------------
    }

    /// 补上两个图标按钮的贴图（幂等）。首帧之后调用。
    void loadIcons()
    {
        if (this->iconsLoaded)
            return;

        this->iconsLoaded = true;

        this->fileButton->setImage(BOREALIS_ASSET("icon/txt.png"));

        if (this->folderButton != nullptr)
            this->folderButton->setImage(BOREALIS_ASSET("icon/folder.png"));

        this->invalidate();
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

    /// 图标是否已经补上（见构造函数里那段说明）
    bool iconsLoaded = false;
};

//=====================================================================
// 图片视图（自绘，替代 brls::Image）
//
// 为什么不用 brls::Image：
//   ① 它只接受「文件路径」，内部走 nvgCreateImage(path) → stb_image 的 fopen。
//      本程序的 SD 卡访问一直走裸 FsFileSystem（更可控），sdmc: 并没有挂在
//      devoptab 上，于是 fopen 失败、nvgCreateImage 返回 0 —— **而且不报错**。
//      真机与模拟器上呈现的就是「日志说 4/4 已加载，屏幕上却什么都没有」。
//   ② 它的 layout() 会把「首次布局时的高度」缓存进 origViewHeight。而
//      Application::pushView() 里会先 `view->invalidate(true)` 强制布局一次 ——
//      那一刻图片还处于折叠状态（高度 0），0 就被永久缓存了；再用 0×0 的纹理
//      尺寸算出 0/0 = NaN 的缩放比，把 NaN 一路灌进 paint 与 setHeight。
//      真机上的表现就是随机崩/白屏，且很难查。
//
// 自己画就没有这些坑：字节自己读（fsx 的裸接口）、失败有明确返回值、
// 尺寸全部用整数、纹理为空就什么都不画。
//=====================================================================
class RemoteImageView : public brls::View
{
  public:
    enum class LoadResult
    {
        Ok = 0,
        ReadFailed,     // 文件读不到（不存在 / SD 卡错误）
        TooBigFile,     // 文件超过 MAX_IMAGE_BYTES
        DecodeFailed,   // 不是能解码的图片（或损坏）
        TooManyPixels,  // 分辨率太高，放弃
    };

    RemoteImageView()
    {
        // 固定高度，宽度由 List 给；不可聚焦（View::getDefaultFocus 默认返回 nullptr）
        this->setHeight(IMAGE_VIEW_HEIGHT);
        // 先收起：等真的解码成功再展开，失败时不会留下一大片空白
        this->collapse(false);
    }

    ~RemoteImageView() override
    {
        this->unload();
    }

    LoadResult loadFromFile(const std::string& sdmcPath, size_t maxPixels)
    {
        this->unload();

        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg == nullptr)
            return LoadResult::DecodeFailed;

        const s64 fileBytes = fsx::fileSize(sdmcPath);
        if (fileBytes <= 0)
            return LoadResult::ReadFailed;

        if (static_cast<size_t>(fileBytes) > MAX_IMAGE_BYTES)
            return LoadResult::TooBigFile;

        std::string bytes;
        if (!fsx::readBinaryFile(sdmcPath, &bytes, MAX_IMAGE_BYTES))
            return LoadResult::ReadFailed;

        // 自己解码：nvgCreateImageMem 内部是 stb_image，JPEG / PNG 都支持
        const int tex = nvgCreateImageMem(vg, 0, reinterpret_cast<unsigned char*>(&bytes[0]),
            static_cast<int>(bytes.size()));
        if (tex <= 0)
            return LoadResult::DecodeFailed;

        // 问出真实尺寸：0×0 说明这文件根本不是能解码的图片（此时纹理必须删掉）
        int w = 0;
        int h = 0;
        nvgImageSize(vg, tex, &w, &h);
        if (w <= 0 || h <= 0)
        {
            nvgDeleteImage(vg, tex);
            return LoadResult::DecodeFailed;
        }

        const size_t px = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (px > maxPixels)
        {
            nvgDeleteImage(vg, tex);
            return LoadResult::TooManyPixels;
        }

        this->texture   = tex;
        this->texWidth  = w;
        this->texHeight = h;
        return LoadResult::Ok;
    }

    void unload()
    {
        if (this->texture > 0)
        {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (vg != nullptr)
                nvgDeleteImage(vg, this->texture);
        }

        this->texture   = -1;
        this->texWidth  = 0;
        this->texHeight = 0;
    }

    bool ready() const
    {
        return this->texture > 0 && this->texWidth > 0 && this->texHeight > 0;
    }

    int textureWidth() const { return this->texWidth; }
    int textureHeight() const { return this->texHeight; }

    /// 占用的像素数（用来算显存预算）
    size_t pixels() const
    {
        return static_cast<size_t>(this->texWidth) * static_cast<size_t>(this->texHeight);
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style,
        FrameContext* ctx) override
    {
        if (!this->ready() || width == 0 || height == 0)
            return;

        const float viewW = static_cast<float>(width);
        const float viewH = static_cast<float>(height);
        const float ratio = static_cast<float>(this->texWidth) / static_cast<float>(this->texHeight);
        if (!(ratio > 0.0f))
            return;

        // 等比缩放到视图内并居中（FIT）
        float drawW = viewW;
        float drawH = viewW / ratio;
        if (drawH > viewH)
        {
            drawH = viewH;
            drawW = viewH * ratio;
        }

        // 保险：任何一步算出非正数就不画（宁可空白也不把非法数值交给 GPU）
        if (!(drawW > 0.0f) || !(drawH > 0.0f))
            return;

        const float offX = static_cast<float>(x) + (viewW - drawW) / 2.0f;
        const float offY = static_cast<float>(y) + (viewH - drawH) / 2.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, offX, offY, drawW, drawH, IMAGE_CORNER_RADIUS);
        nvgFillPaint(vg, nvgImagePattern(vg, offX, offY, drawW, drawH, 0.0f, this->texture, 1.0f));
        nvgFill(vg);
    }

  private:
    int texture   = -1;
    int texWidth  = 0;
    int texHeight = 0;
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
// 诊断信息（v2.7.0 起改为「就地展开」，不再 pushView 新页面）
//
// 真机反馈：点「诊断信息与提示」会直接报错退出（不是死机）。
// 那个版本是新建一个 DetailsView（List 子树）再 pushView，
// 里面一次性塞了 5 个 Label，其中一段是 update_result.txt 的前 6000 字节。
//
// 与其继续猜是哪一层出的问题，不如**整条路去掉**：
//   * 诊断文本直接用**主页面已有的 Label** 就地展开/收起
//     （expand/collapse 是图片位一直在用、已验证的机制）；
//   * 文本量压到几百字节，并拆成两段，避免任何超长 Label；
//   * 构造/展开每一步都写 [UI] 面包屑，万一还出问题，log.txt 的最后一行就指明位置。
//=====================================================================

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

MainView::MainView(Downloader* downloader, const std::string& startupNotice,
    const appcfg::UrlEntry& urlEntry, const std::string& settingsText)
    : List()
    , downloader(downloader)
{
    //-----------------------------------------------------------------
    // 构造函数里每一步都留一行日志。
    // 真机崩溃时，日志的**最后一行**就指明了死在哪一步 ——
    // 上一版构造函数里一行日志都没有，日志停在 pushView 之前，
    // 完全看不出是构造的哪一段出的问题（只能靠猜，代价是一轮一轮试）。
    //
    // ★★ 构造函数里**不做任何 SD 卡访问**：
    //    url.txt / settings.txt 的内容由 main.cpp 提前读好传进来（读在这里也一样要花 I/O，
    //    但这样构造函数就变成纯 CPU；真机崩溃点正好在「一次 SD 写盘之后」，
    //    先把 SD 访问从这段区间里彻底拿掉）。
    //-----------------------------------------------------------------
    logx::ui("构造：解析设置");
    this->readSettings(settingsText);

    if (!this->savedDir.empty())
        this->outputDir = fsx::normalize(this->savedDir);

    logx::ui("构造：标题");
    this->addView(new Header("NX Downloader", true, "上行检查更新 · 下行下载文件"));

    logx::ui("构造：两行");
    this->buildRows();

    logx::ui("构造：状态区");
    this->buildStatusArea();

    logx::ui("构造：图片位");
    this->buildGallery(urlEntry.images);

    logx::ui("构造：底部");
    this->buildFooter(startupNotice);

    logx::ui("构造：按钮状态");
    this->setButtonsEnabled(true);
    this->setCancelEnabled(false);

    if (!this->savedUpdate.empty() || !this->savedDownload.empty())
    {
        logx::ui("构造：恢复上次的链接");
        this->applyingSaved = true;
        if (!this->savedUpdate.empty())
            this->updateRow->input->setText(appcfg::normalizeUrl(this->savedUpdate));
        if (!this->savedDownload.empty())
            this->downloadRow->input->setText(appcfg::normalizeUrl(this->savedDownload));
        this->applyingSaved = false;
    }

    logx::ui("构造：轮询任务");
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
    // 这一层面包屑是「按控件」打的：真机崩溃点曾经落在这个函数里，
    // 只有把每一步都写下来，才能一次定位到具体是哪个控件/调用。
    logx::ui("状态区：状态 Label");
    this->statusLabel = new Label(LabelStyle::REGULAR, "状态：就绪", false);
    this->shownStatus = "就绪";
    this->addView(this->statusLabel);

    logx::ui("状态区：进度条");
    this->bar = new ProgressDisplay(ProgressDisplayFlags::PERCENTAGE);
    this->bar->setHeight(60);
    this->addView(this->bar);

    logx::ui("状态区：取消按钮");
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

    logx::ui("状态区：详情 Label");
    this->detailLabel = new Label(LabelStyle::DESCRIPTION,
        "（这里会显示检查更新的返回内容、以及错误详情）", true);
    this->shownDetail = "（这里会显示检查更新的返回内容、以及错误详情）";
    this->addView(this->detailLabel);

    logx::ui("状态区：完成");
}

void MainView::buildGallery(const std::vector<std::string>& configured)
{
    // img: 列表由 main.cpp 提前从 url.txt 解析好传进来 —— 这里**不读 SD 卡**
    std::vector<std::string> items = configured;

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

        // 自绘的图片位：先收起，等真解码成功再展开
        slot.view = new RemoteImageView();

        this->addView(slot.view);
        this->imageSlots.push_back(slot);
    }

    if (!items.empty())
        logx::uif("img: 共 %u 项，图片位已建立", static_cast<unsigned>(items.size()));
}

void MainView::buildFooter(const std::string& startupNotice)
{
    // ★ 启动提示这里**只存不显示**。
    //   「Applet 模式」这类提示只在相册启动时有，如果它在构造函数里多建一个 Label，
    //   两种启动模式下构造出来的视图树就不一样了 —— 真机一崩就没法判断是哪条路径的问题。
    //   显示统一放到界面跑起来之后（showStartupNotice）。
    this->startupNotice = startupNotice;

    this->dirLabel = new Label(LabelStyle::DESCRIPTION,
        "下载保存到：" + this->outputDir + "（第二行的文件夹图标可更换）", true);
    this->addView(this->dirLabel);

    //-----------------------------------------------------------------
    // ★ 底部锚点：borealis 的 List 只会「滚动到当前焦点」，页面最底下如果
    //   没有可聚焦的控件，再往下就永远滚不动（实测就是「底部内容看不全」）。
    //   这个按钮既是有用的入口，也顺手把滚动范围撑到底。
    //
    //   ★ 诊断正文（两段，都很短）挂在按钮**上方**：
    //     List 的滚动永远以「当前焦点」为准，焦点就在这个按钮上，
    //     所以正文放在按钮上面才一定看得见；放在下面会被屏幕裁掉。
    //-----------------------------------------------------------------
    this->diagLabel = new Label(LabelStyle::DESCRIPTION, "", true);
    this->diagLabel->collapse(false); // 默认收起：不占位、不可见
    this->addView(this->diagLabel);

    this->diagImagesLabel = new Label(LabelStyle::DESCRIPTION, "", true);
    this->diagImagesLabel->collapse(false);
    this->addView(this->diagImagesLabel);

    this->detailsButton = new Button(ButtonStyle::REGULAR);
    this->detailsButton->setLabel("诊断信息与提示（A 展开 / 收起）");
    this->detailsButton->setHeight(listItemHeight());
    this->detailsButton->getClickEvent()->subscribe([this](View*) {
        this->toggleDiagnostics();
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

/// 把 RemoteImageView::LoadResult 翻成给用户看的话
static std::string describeLoadResult(RemoteImageView::LoadResult result, s64 fileBytes)
{
    switch (result)
    {
        case RemoteImageView::LoadResult::Ok:
            return "已加载";
        case RemoteImageView::LoadResult::ReadFailed:
            return "读不到文件（不存在或 SD 卡错误）";
        case RemoteImageView::LoadResult::TooBigFile:
            return "文件太大（" + fsx::formatBytes(fileBytes) + "，上限 " +
                   fsx::formatBytes(static_cast<s64>(MAX_IMAGE_BYTES)) + "）";
        case RemoteImageView::LoadResult::TooManyPixels:
            return "分辨率太高（上限 " + std::to_string(MAX_IMAGE_PIXELS_EACH / 1000000u) +
                   "M 像素），换小图再试";
        case RemoteImageView::LoadResult::DecodeFailed:
        default:
            return "不是能解码的图片（损坏 / 不是 JPEG·PNG）";
    }
}

void MainView::loadImageIntoSlot(size_t index, const std::string& path)
{
    if (index >= this->imageSlots.size())
        return;

    ImageSlot& slot = this->imageSlots[index];

    const s64 fileBytes = fsx::fileSize(path);
    const RemoteImageView::LoadResult result = slot.view->loadFromFile(path, MAX_IMAGE_PIXELS_EACH);

    if (result != RemoteImageView::LoadResult::Ok)
    {
        slot.loaded  = false;
        slot.pending = false;
        slot.note    = describeLoadResult(result, fileBytes);
        this->imagesFailed++;

        logx::uif("图片 %u 加载失败：%s（%s，%lld 字节）", static_cast<unsigned>(index + 1),
            path.c_str(), slot.note.c_str(), static_cast<long long>(fileBytes));

        // 失败就保持收起：不留空白
        this->invalidate();
        return;
    }

    // 总预算：Applet 模式下显存/内存都紧张，宁可少显示几张
    const size_t px     = slot.view->pixels();
    const size_t budget = imagePixelBudget();

    if (this->imagePixelsUsed + px > budget)
    {
        slot.view->unload();
        slot.loaded  = false;
        slot.pending = false;
        slot.note    = "显存预算已用完，跳过（已用 " + std::to_string(this->imagePixelsUsed / 1000000u) +
                       "M / 上限 " + std::to_string(budget / 1000000u) + "M 像素）";
        this->imagesFailed++;

        logx::uif("图片 %u 跳过：%s", static_cast<unsigned>(index + 1), slot.note.c_str());
        this->invalidate();
        return;
    }

    this->imagePixelsUsed += px;

    slot.cached = path;
    slot.loaded = true;
    slot.pending = false;
    slot.note   = "已加载（" + std::to_string(slot.view->textureWidth()) + "×" +
                  std::to_string(slot.view->textureHeight()) + "）";

    slot.view->expand(false); // 展开成固定高度

    this->imagesLoaded++;

    // ⚠️ borealis 的 `View::invalidate(bool)` **不会**往上通知父级
    //    （它只置自己的 dirty）。展开/换图之后必须显式让 List 重新排版，
    //    否则这一格仍然按 0 高度排位，内容会被下面的控件盖住。
    this->invalidate();

    logx::uif("图片 %u 已加载：%s（%d×%d）", static_cast<unsigned>(index + 1), path.c_str(),
        slot.view->textureWidth(), slot.view->textureHeight());
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

    logx::uif("图片加载开始：共 %u 项，显存预算 %.0fM 像素（模式=%s）",
        static_cast<unsigned>(this->imageSlots.size()),
        static_cast<double>(imagePixelBudget()) / 1000000.0,
        netx::fullMemoryMode() ? "完整内存" : "Applet");

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
            slot.note = "找不到本地文件（不是链接，也没在项目文件夹 / SD 根目录里找到）";
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

//------------------------------ 启动提示 ------------------------------//

void MainView::showStartupNotice()
{
    if (this->noticeShown)
        return;

    this->noticeShown = true;

    // 主循环已经跑起来了，这时候才补按钮图标（构造函数里不碰 GPU，见 InputRow 的说明）
    if (this->updateRow != nullptr)
        this->updateRow->loadIcons();
    if (this->downloadRow != nullptr)
        this->downloadRow->loadIcons();
    logx::ui("主循环第一轮：按钮图标已补上（GL 纹理创建正常）");

    // 日志写不进去时**必须在屏幕上说出来**：
    // 否则日志会静静地停在上一次成功的那一行，看起来像「程序死在这一步」。
    if (logx::writeFailed())
    {
        logx::ui("界面提示：日志写入失败");

        this->setStatus("⚠ 日志写入失败");
        this->setDetail("⚠ 无法写入 SD 卡日志：\n" + std::string(appcfg::LOG_FILE) +
                        "\n\n如果程序随后异常退出，请把屏幕上的内容一并告诉我 —— 日志文件可能不完整。");
        return;
    }

    if (!this->startupNotice.empty())
    {
        this->setStatus("就绪（有启动提示）");
        this->setDetail(this->startupNotice);
    }
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

void MainView::toggleDiagnostics()
{
    if (this->diagLabel == nullptr || this->diagImagesLabel == nullptr)
        return;

    //============================= 收起 =============================//
    if (this->diagOpen)
    {
        logx::ui("诊断信息：收起");

        this->diagLabel->collapse(false);
        this->diagImagesLabel->collapse(false);
        this->diagOpen = false;

        this->invalidate();
        return;
    }

    //============================= 展开 =============================//
    // ★ 文本刻意压得很短（两段加起来几百字节），而且到点按钮这一刻才生成。
    //   主界面单帧要排版的文本越少越不容易出问题；完整信息永远在文件里：
    //   log.txt（全过程诊断）+ update_result.txt（上次返回内容）。
    // ★ 每次展开都重新生成：文本量很小，但会随图片加载结果变化，
    //   缓存起来反而会让人看到上一次的旧状态。
    logx::ui("诊断信息：构建文本");

    std::string info;
    info += "环境：" + netx::describe() + "\n";
    info += "目录：" + std::string(appcfg::PROJECT_DIR) + "\n";
    info += "详细日志与返回内容：同目录的 log.txt / update_result.txt\n";

    std::string result;
    if (fsx::readWholeFile(appcfg::RESULT_FILE, &result) && !result.empty())
        info += "上次返回内容：" + fsx::formatBytes(static_cast<s64>(result.size())) + "\n";
    else
        info += "上次返回内容：还没有记录\n";

    if (logx::writeFailed())
        info += "⚠ 日志写入失败（SD 卡），log.txt 可能不完整\n";

    if (!this->startupNotice.empty())
        info += "启动提示：" + this->startupNotice + "\n";

    this->diagText = info;
    logx::uif("诊断信息：环境段 %u 字节", static_cast<unsigned>(info.size()));

    std::string images;

    if (this->imageSlots.empty())
    {
        images = "图片：未配置（在 url.txt 里用 img: 指定）";
    }
    else
    {
        images = "图片：" + std::to_string(this->imagesLoaded) + "/" +
                 std::to_string(this->imageSlots.size()) + " 已加载，显存 " +
                 std::to_string(this->imagePixelsUsed / 1000000u) + "M/" +
                 std::to_string(imagePixelBudget() / 1000000u) + "M 像素\n";

        for (size_t i = 0; i < this->imageSlots.size(); i++)
        {
            const ImageSlot& slot = this->imageSlots[i];

            images += std::to_string(i + 1) + ".";
            images += slot.loaded
                          ? std::string(" 已加载")
                          : (std::string(" 失败：") + (slot.note.empty() ? "原因未知" : slot.note));
            images += "\n";
        }

        // ★ 底层诊断：SSL 类错误只显示一句「35」是没法判断原因的，
        //   这里把 curl 原文 / OS errno / 对端 IP 直接写出来 —— 截图就能看。
        const std::string diag = this->downloader != nullptr ? this->downloader->diagText() : std::string();
        if (!diag.empty())
            images += "\n底层诊断：" + diag;
    }

    this->diagImagesText = images;
    logx::uif("诊断信息：图片段 %u 字节", static_cast<unsigned>(images.size()));

    logx::ui("诊断信息：展开");
    this->diagLabel->setText(this->diagText);
    this->diagLabel->expand(false);
    this->diagImagesLabel->setText(this->diagImagesText);
    this->diagImagesLabel->expand(false);

    this->diagOpen = true;
    this->invalidate();

    logx::ui("诊断信息：展开完成");
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

    this->aliveTicks++;

    // 第一次轮询 ≈ 界面已经进入主循环：这时候才动界面文案 / 碰图片
    if (this->aliveTicks == 1)
        this->showStartupNotice();

    // 前 6 次轮询把 applet 状态写进日志：这既是「渲染循环确实活着」的证据，
    // 也记录了**卡死前的焦点状态** —— Applet 模式失焦时系统会接管 SD 卡 / 显示，
    // 此时做 SD 读写最容易互相干扰，是这类「整机死机」问题的关键判据。
    if (this->aliveTicks <= 6)
        logx::uif("轮询第 %d 次：主循环正常，%s", this->aliveTicks, netx::appletStateText().c_str());
    else if (this->aliveTicks == 7)
        logx::ui("（之后不再逐次记录轮询）");

    // 图片加载刻意推迟到第 3 次轮询（约 300ms）：那时界面已经稳稳画了几帧，
    // 万一图片这条路上有问题，日志能明确区分「界面还没起来」和「界面起来后碰图片才出事」。
    if (!this->startupImagesStarted && this->aliveTicks >= 3)
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

        const std::string err = this->downloader->errorText();

        // 失败原因存进 slot：底部「诊断信息」会逐张列出来，用户不用猜
        if (static_cast<size_t>(index) < this->imageSlots.size())
        {
            ImageSlot& slot = this->imageSlots[static_cast<size_t>(index)];
            slot.note       = err;

            // SSL 类失败：三档（校验证书 / 不校验 / 不校验+HTTP1.1+TLS1.2）都试过了
            // 还是握不上手，说明这个地址在 Switch 上就是连不通。
            // 给一条能直接照做的建议，比只显示一句英文错误有用得多。
            if (err.find("SSL") != std::string::npos || err.find("证书") != std::string::npos)
                slot.note += "（已试 4 种传输配置仍握手失败：换图片地址，或先把图拷到 SD 卡用文件名引用）";

            logx::uif("图片 %d 下载未成功：%s", index + 1, slot.note.c_str());
        }
        else
        {
            logx::uif("图片 %d 下载未成功：%s", index + 1, err.c_str());
        }
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

    // ★ 这一张失败**不再中断整批**，继续下一张。
    //   旧逻辑是「有失败就停」，结果第 1 张失败后 2/3/4 张压根不试 ——
    //   而这些图很可能来自别的域名，或者本来就是本地文件（根本不需要联网），
    //   一竿子打死等于把能显示的也一起丢了。每张失败的原因各自记在 slot 里，
    //   诊断区会逐张列出来。
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
    {
        // 把**第一张失败的原因和底层诊断**直接写出来：
        // 用户不用连电脑取 log.txt，看这一屏就够了。
        std::string text = "有 " + std::to_string(this->imagesFailed) + " 张没能加载。\n";

        for (const ImageSlot& slot : this->imageSlots)
        {
            if (!slot.loaded && !slot.note.empty())
            {
                text += "失败原因：" + slot.note + "\n";
                break;
            }
        }

        const std::string diag = this->downloader != nullptr ? this->downloader->diagText() : std::string();
        if (!diag.empty())
            text += "\n底层诊断：" + diag;

        this->setDetail(text);
        logx::uif("图片批量结束：%d 张失败；诊断=%s", this->imagesFailed, diag.c_str());
    }
    else
    {
        this->setDetail("图片都在下面了。要换图就改 " + std::string(appcfg::URL_FILE) +
                        " 里的 img: 行，然后重启程序。");
    }

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

void MainView::readSettings(const std::string& text)
{
    this->savedUpdate.clear();
    this->savedDownload.clear();
    this->savedDir.clear();

    // 内容由 main.cpp 读好传进来（构造函数里不碰 SD 卡，原因见构造函数顶部注释）
    if (text.empty())
        return;

    // 记下原文：只要界面里没改动，saveSettings() 就不会再写一次盘
    this->lastSavedText = text;

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

    // 启动时把上次保存的值填回输入框会触发 onChange → 这里被调用。
    // 内容其实没变，白写一次 SD 卡没有任何意义（Applet 模式下 SD I/O 越少越安全）。
    if (this->applyingSaved)
        return;

    std::string data;
    data += "# NX Downloader 设置（界面里改过之后会自动写回这里）\n";
    data += "update=" + this->updateRow->input->text() + "\n";
    data += "download=" + this->downloadRow->input->text() + "\n";
    data += "dir=" + this->outputDir + "\n";

    // 与上次写出去的内容一致就不写（避免无意义的 SD 写入）
    if (data == this->lastSavedText)
        return;

    if (!fsx::ensureDirectory(appcfg::PROJECT_DIR))
        return;

    if (fsx::writeWholeFile(appcfg::SETTINGS_FILE, data))
        this->lastSavedText = data;
}
