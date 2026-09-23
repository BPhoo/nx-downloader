#include "main_view.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "fsx.hpp"
#include "path_picker.hpp"
#include "progress_dialog.hpp"

using namespace brls;

namespace
{

// 设置文件（同时也可以作为「Applet 模式下软键盘不可用」时的备用输入方式）
const char* SETTINGS_DIR  = "sdmc:/switch/nx-downloader";
const char* SETTINGS_PATH = "sdmc:/switch/nx-downloader/settings.txt";

// 链接最大长度（swkbd 的缓冲上限是 0x100 字节，这里保守一点）
constexpr int MAX_URL_LENGTH = 240;

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

/// 链接输入框：按 A 调起系统软键盘；按 X 从 url.txt / settings.txt 读取
class UrlInputItem : public ListItem
{
  public:
    UrlInputItem()
        : ListItem("下载链接", "按 A 输入 http/https 直链，按 X 从 url.txt 读取")
    {
        this->registerAction("从文件读取", Key::X, [this] {
            this->loadFromFile();
            return true;
        });
    }

    bool onClick() override
    {
        const std::string initial = stripLineBreaks(this->getValue());

        const bool ok = Swkbd::openForText(
            [this](std::string text) { this->setValue(stripLineBreaks(text), false); },
            "下载链接",
            "例如 https://example.com/file.zip",
            MAX_URL_LENGTH,
            initial);

        if (!ok)
            Application::notify("没有拿到输入。若系统键盘不可用，可按 X 从 url.txt 读取");

        return true;
    }

  private:
    void loadFromFile()
    {
        std::string text;
        const char* candidates[] = {
            "sdmc:/switch/nx-downloader/url.txt",
            "sdmc:/switch/nx-downloader/settings.txt",
        };

        for (const char* path : candidates)
        {
            if (!fsx::readWholeFile(path, &text))
                continue;

            // settings.txt 里可能带 "url=" 前缀
            for (const std::string& line : splitLines(text))
            {
                std::string value = trim(line);
                if (value.empty())
                    continue;
                if (value.compare(0, 4, "url=") == 0)
                    value = trim(value.substr(4));
                if (value.compare(0, 1, "#") == 0)
                    continue;
                if (Downloader::isSupportedUrl(value))
                {
                    this->setValue(value, false);
                    Application::notify("已载入链接");
                    return;
                }
            }
        }

        Application::notify("没找到可用链接（可写入 sdmc:/switch/nx-downloader/url.txt）");
    }

    static std::vector<std::string> splitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::string current;

        for (char c : text)
        {
            if (c == '\n')
            {
                lines.push_back(current);
                current.clear();
            }
            else if (c != '\r')
            {
                current.push_back(c);
            }
        }

        if (!current.empty())
            lines.push_back(current);

        return lines;
    }
};

/// 周期性把下载进度刷到 UI 上的任务（必须由 UI 线程执行）
class DownloadPollTask : public RepeatingTask
{
  public:
    DownloadPollTask(Downloader* downloader, DownloadProgressDialog* dialog, MainView* view)
        : RepeatingTask(100)
        , downloader(downloader)
        , dialog(dialog)
        , view(view)
    {
    }

    void run(retro_time_t currentTime) override
    {
        if (this->downloader == nullptr)
        {
            this->stop();
            return;
        }

        const Downloader::State state = this->downloader->state();

        if (state == Downloader::State::Idle || state == Downloader::State::Running)
        {
            if (this->dialog != nullptr)
                this->dialog->update(this->downloader);

            return;
        }

        // 进入终态：先把最后一帧刷新完，再把手里的指针取出来
        DownloadProgressDialog* dialog = this->dialog;
        MainView* view                 = this->view;

        if (dialog != nullptr)
            dialog->update(this->downloader);

        // stop() 之后 this 随时可能被任务管理器回收，后面不能再访问成员
        this->stop();

        if (view != nullptr)
            view->onDownloadFinished();
    }

  private:
    Downloader* downloader;
    DownloadProgressDialog* dialog;
    MainView* view;
};

} // namespace

//=====================================================================
// MainView
//=====================================================================

MainView::MainView(Downloader* downloader)
    : List()
    , downloader(downloader)
{
    // 先读设置（此时还没有控件，所以只把值取出来）
    std::string savedUrl;
    std::string savedDir;
    this->readSettings(&savedUrl, &savedDir);

    if (!savedDir.empty())
        this->downloadDir = fsx::normalize(savedDir);

    this->addView(new Header("NX Downloader", true, "粘贴直链 → 选择目录 → 开始下载"));

    // 1. 链接输入框
    this->urlItem = new UrlInputItem();
    this->addView(this->urlItem);

    // 2. 保存目录按钮（带文件夹图标）
    this->pathButton = new Button(ButtonStyle::REGULAR);
    this->pathButton->setImage(BOREALIS_ASSET("icon/folder.png"));
    this->pathButton->setLabel("保存到：" + this->downloadDir);
    this->pathButton->setHeight(listItemHeight());
    this->pathButton->getClickEvent()->subscribe([this](View*) {
        this->openPathPicker();
    });
    this->addView(this->pathButton);

    // 3. 下载按钮
    this->downloadButton = new Button(ButtonStyle::PRIMARY);
    this->downloadButton->setLabel("开始下载");
    this->downloadButton->setHeight(listItemHeight() + listItemHeight() / 3);
    this->downloadButton->getClickEvent()->subscribe([this](View*) {
        this->startDownload();
    });
    this->addView(this->downloadButton);

    Label* tip = new Label(LabelStyle::DESCRIPTION,
        "提示：按 A 打开系统键盘输入链接，按 X 可从 sdmc:/switch/nx-downloader/url.txt 读取。"
        "\nApplet 模式下系统键盘可能无法调起，此时请按住 R 键从游戏图标启动本程序（完整内存模式）。",
        true);
    this->addView(tip);

    // 控件都建好之后再回填上次保存的链接
    if (!savedUrl.empty() && Downloader::isSupportedUrl(savedUrl))
        this->urlItem->setValue(savedUrl, false);
}

MainView::~MainView() = default;

//------------------------------ 设置持久化 ------------------------------//

void MainView::readSettings(std::string* url, std::string* dir) const
{
    if (url != nullptr)
        url->clear();
    if (dir != nullptr)
        dir->clear();

    std::string text;
    if (!fsx::readWholeFile(SETTINGS_PATH, &text))
        return;

    std::string line;
    for (size_t i = 0; i <= text.size(); i++)
    {
        if (i == text.size() || text[i] == '\n')
        {
            std::string entry = trim(line);
            line.clear();

            if (entry.compare(0, 4, "url=") == 0)
            {
                if (url != nullptr)
                    *url = trim(entry.substr(4));
            }
            else if (entry.compare(0, 4, "dir=") == 0)
            {
                if (dir != nullptr)
                    *dir = trim(entry.substr(4));
            }

            continue;
        }

        line.push_back(text[i]);
    }
}

void MainView::saveSettings() const
{
    if (this->urlItem == nullptr)
        return;

    const std::string url = stripLineBreaks(this->urlItem->getValue());

    std::string data;
    data += "# NX Downloader 设置（可以直接改，也可以在界面里改）\n";
    data += "url=" + url + "\n";
    data += "dir=" + this->downloadDir + "\n";

    if (!fsx::ensureDirectory(SETTINGS_DIR))
        return;

    fsx::writeWholeFile(SETTINGS_PATH, data);
}

//------------------------------ 目录选择 ------------------------------//

void MainView::setDownloadDir(const std::string& path, bool persist)
{
    this->downloadDir = fsx::normalize(path);

    if (this->pathButton != nullptr)
        this->pathButton->setLabel("保存到：" + this->downloadDir);

    if (persist)
        this->saveSettings();

    Application::notify("保存目录：" + this->downloadDir);
}

void MainView::openPathPicker()
{
    PathPickerView* picker = new PathPickerView(this->downloadDir, [this](const std::string& path) {
        this->setDownloadDir(path, true);
    });

    Application::pushView(picker);
}

//------------------------------ 下载流程 ------------------------------//

void MainView::showMessage(const std::string& title, const std::string& text)
{
    Dialog* dialog = new Dialog(title + "\n\n" + text);

    dialog->addButton("好的", [dialog](View*) {
        dialog->close();
    });

    dialog->open();
}

void MainView::startDownload()
{
    if (this->downloader == nullptr)
        return;

    const std::string url = trim(this->urlItem->getValue());

    if (url.empty())
    {
        this->showMessage("还没有链接", "请先按 A 输入一个 http/https 直链。");
        return;
    }

    if (!Downloader::isSupportedUrl(url))
    {
        this->showMessage("链接无效", "只支持 http:// 与 https:// 开头的链接。\n\n当前内容：\n" + url);
        return;
    }

    if (this->downloader->running())
    {
        this->showMessage("正在下载", "请等待当前下载结束，或先在上一个对话框里点「取消」。");
        return;
    }

    // 先落盘保存，方便下次启动时保留
    this->saveSettings();

    std::string fileName = fsx::fileNameFromUrl(url);
    if (fileName.empty())
        fileName = "download.bin";

    const std::string target = fsx::join(this->downloadDir, fileName);

    if (fsx::exists(target))
    {
        Dialog* dialog = new Dialog("目标文件已存在，是否覆盖？\n\n" + target);

        dialog->addButton("覆盖", [this, dialog, url, fileName](View*) {
            // 必须等对话框真正出栈之后再压入新视图，否则会搞乱视图栈
            dialog->close([this, url, fileName]() {
                this->beginDownload(url, fileName);
            });
        });

        dialog->addButton("取消", [dialog](View*) {
            dialog->close();
        });

        dialog->open();
        return;
    }

    this->beginDownload(url, fileName);
}

void MainView::beginDownload(const std::string& url, const std::string& fileName)
{
    if (!fsx::ensureDirectory(this->downloadDir))
    {
        this->showMessage("保存目录不可用", "无法创建或访问目录：\n" + this->downloadDir);
        return;
    }

    if (!this->downloader->start(url, this->downloadDir))
    {
        this->showMessage("无法开始下载", "请检查链接与保存目录是否正确。");
        return;
    }

    this->downloadButton->setState(ButtonState::DISABLED);

    this->progressDialog = new DownloadProgressDialog(fileName, [this] {
        // 「取消」按钮 / 只是请求取消，真正的收尾交给轮询任务
        if (this->downloader != nullptr)
            this->downloader->requestCancel();
    });

    this->progressDialog->open();

    this->pollTask = new DownloadPollTask(this->downloader, this->progressDialog, this);
    this->pollTask->start();
}

void MainView::onDownloadFinished()
{
    if (this->downloader == nullptr)
        return;

    const Downloader::State state = this->downloader->state();

    std::string title;
    std::string text;

    switch (state)
    {
        case Downloader::State::Finished:
        {
            title = "下载完成";
            text  = this->downloader->outputPath();

            if (!this->downloader->tlsVerified())
                text += "\n\n⚠ 本次连接未校验证书（设备上没有可用的 CA 证书）。"
                        "如果要严格校验，可把 cacert.pem 放到 sdmc:/switch/nx-downloader/ 下。";

            Application::notify("下载完成：" + this->downloader->fileName());
            break;
        }

        case Downloader::State::Cancelled:
        {
            title = "已取消";
            text  = "下载已取消，未完成的文件已经删除。";
            break;
        }

        default:
        {
            title = "下载失败";
            text  = this->downloader->errorText();
            if (text.empty())
                text = "未知错误（可打开日志查看详情）。";
            break;
        }
    }

    DownloadProgressDialog* dialog = this->progressDialog;

    this->progressDialog = nullptr;
    this->pollTask       = nullptr; // 任务由 borealis 的任务管理器自行回收

    if (this->downloadButton != nullptr)
        this->downloadButton->setState(ButtonState::ENABLED);

    if (dialog == nullptr)
    {
        this->showMessage(title, text);
        return;
    }

    // 关掉进度对话框之后再弹结果提示（popView 是异步动画，必须用回调串起来）
    dialog->close([this, title, text]() {
        this->showMessage(title, text);
    });
}
