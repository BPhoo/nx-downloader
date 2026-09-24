#include "file_picker.hpp"

#include <cctype>
#include <vector>

#include "app_config.hpp"
#include "fsx.hpp"

using namespace brls;

namespace
{

/// 文件名是否是我们关心的文本配置文件
bool isTextFile(const std::string& name)
{
    if (name.size() <= 4)
        return false;

    std::string tail = name.substr(name.size() - 4);
    for (char& c : tail)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return tail == ".txt";
}

/// 目录项显示时过长会破坏排版，截断一下
std::string ellipsize(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;

    size_t cut = maxBytes;
    // 不要切在 UTF-8 续字节上
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        cut--;

    return s.substr(0, cut) + "…";
}

} // namespace

TextFilePickerView::TextFilePickerView(const std::string& startPath, std::function<void(const std::string&)> onPick)
    : List()
    , onPick(std::move(onPick))
{
    this->currentPath = fsx::normalize(startPath);
    if (!fsx::isDirectory(this->currentPath))
        this->currentPath = "sdmc:/";

    this->registerAction("返回", Key::B, [this] {
        Application::popView();
        return true;
    });

    // 此时尚未入栈：rebuild 内部不会去抢焦点
    this->rebuild();
}

TextFilePickerView::~TextFilePickerView() = default;

void TextFilePickerView::willAppear(bool resetState)
{
    List::willAppear(resetState);
    this->onStack = true;
}

void TextFilePickerView::rebuild()
{
    this->clear(true);

    this->addView(new Header("选择配置文件", true, this->currentPath));

    // 上一级目录
    if (this->currentPath != "sdmc:/")
    {
        const std::string parentPath = fsx::parent(this->currentPath);

        ListItem* upItem = new ListItem("上一级目录", parentPath);
        upItem->setValue("进入");
        upItem->getClickEvent()->subscribe([this, parentPath](View*) {
            this->navigateTo(parentPath);
        });
        this->addView(upItem);
    }

    std::vector<std::string> dirs;
    std::vector<std::string> files;
    const bool listed = fsx::listDirectory(this->currentPath, &dirs, &files);

    // 子目录
    for (const std::string& name : dirs)
    {
        const std::string childPath = fsx::join(this->currentPath, name);

        ListItem* item = new ListItem(ellipsize(name, 60));
        item->setValue("进入");
        item->getClickEvent()->subscribe([this, childPath](View*) {
            this->navigateTo(childPath);
        });
        this->addView(item);
    }

    // .txt 文件
    int textCount = 0;
    ListItem* firstFile = nullptr;

    for (const std::string& name : files)
    {
        if (!isTextFile(name))
            continue;

        const std::string filePath = fsx::join(this->currentPath, name);

        ListItem* item = new ListItem(ellipsize(name, 60));
        item->setValue("选择");
        item->getClickEvent()->subscribe([this, filePath](View*) {
            std::function<void(const std::string&)> callback = this->onPick;

            if (!callback)
            {
                Application::popView();
                return;
            }

            // 先让本视图出栈、再回调。
            // 反过来的话，回调里弹出的提示框会被紧随其后的 popView 顶掉
            // （popView / pushView 是异步动画，顺序错了视图栈就乱）。
            Application::popView(ViewAnimation::FADE, [callback, filePath] {
                callback(filePath);
            });
        });
        this->addView(item);

        if (firstFile == nullptr)
            firstFile = item;
        textCount++;
    }

    if (!listed)
    {
        this->addView(new Label(LabelStyle::DESCRIPTION, "（无法读取这个目录）", true));
    }
    else if (textCount == 0)
    {
        this->addView(new Label(LabelStyle::DESCRIPTION,
            std::string("（这个目录下没有 .txt 文件；配置文件默认是 ") + appcfg::URL_FILE + "）", true));
    }

    // 只有已经入栈时才重新给焦点：
    // 构造函数阶段（尚未入栈）碰焦点会把主界面的当前焦点错误地压进 focusStack，
    // 返回主界面后焦点悬空、所有按键失灵。
    if (this->onStack)
        Application::giveFocus(firstFile);
}

void TextFilePickerView::navigateTo(const std::string& path)
{
    this->currentPath = fsx::normalize(path);
    this->rebuild();
}
