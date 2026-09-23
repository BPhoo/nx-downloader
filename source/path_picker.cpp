#include "path_picker.hpp"

#include <vector>

#include "fsx.hpp"

using namespace brls;

PathPickerView::PathPickerView(const std::string& startPath, std::function<void(const std::string&)> onSelect)
    : List()
    , onSelect(std::move(onSelect))
{
    this->currentPath = fsx::normalize(startPath);
    if (!fsx::isDirectory(this->currentPath))
        this->currentPath = "sdmc:/";

    // B 键返回上一屏：交给 Application 把本视图弹出，焦点会自动交还给主界面
    this->registerAction("返回", Key::B, [this] {
        Application::popView();
        return true;
    });

    // 此时本视图尚未入栈：rebuild 内部不会去抢主界面的焦点
    this->rebuild();
}

PathPickerView::~PathPickerView() = default;

void PathPickerView::willAppear(bool resetState)
{
    List::willAppear(resetState);
    // 入栈完成，之后 rebuild（切换目录）时才可以安全地重新给焦点
    this->onStack = true;
}

void PathPickerView::confirmCurrent()
{
    auto callback = this->onSelect;
    if (callback)
        callback(this->currentPath);

    Application::popView();
}

void PathPickerView::rebuild()
{
    this->clear(true);

    this->addView(new Header("选择下载目录", true, this->currentPath));

    ListItem* useItem = new ListItem("使用此目录", "把文件保存到 " + this->currentPath);
    useItem->setValue("确定");
    useItem->getClickEvent()->subscribe([this](View*) {
        this->confirmCurrent();
    });
    this->addView(useItem);

    // 上一级目录
    if (this->currentPath != "sdmc:/")
    {
        std::string parentPath = fsx::parent(this->currentPath);
        ListItem* upItem       = new ListItem("上一级目录", parentPath);
        upItem->setValue("进入");
        upItem->getClickEvent()->subscribe([this, parentPath](View*) {
            this->navigateTo(parentPath);
        });
        this->addView(upItem);
    }

    std::vector<std::string> dirs;
    bool listed = fsx::listSubDirectories(this->currentPath, &dirs);

    if (!listed || dirs.empty())
    {
        Label* empty = new Label(LabelStyle::DESCRIPTION,
            listed ? "（这个目录下没有子目录）" : "（无法读取这个目录）", true);
        this->addView(empty);
    }
    else
    {
        for (const std::string& name : dirs)
        {
            const std::string childPath = fsx::join(this->currentPath, name);

            ListItem* item = new ListItem(name);
            item->setValue("进入");
            item->getClickEvent()->subscribe([this, childPath](View*) {
                this->navigateTo(childPath);
            });
            this->addView(item);
        }
    }

    // 只有本视图已经在视图栈上时才重新给焦点。
    // 构造函数阶段（尚未入栈）不要碰焦点，否则会把「主界面当前焦点」错误地
    // 存进 focusStack，导致返回主界面后焦点悬空、所有按键失灵。
    // 注意：删除正被聚焦的子项时，borealis 会自动把全局焦点清空，因此这里
    // 重新聚焦是安全的。
    if (this->onStack)
        Application::giveFocus(useItem);
}

void PathPickerView::navigateTo(const std::string& path)
{
    this->currentPath = fsx::normalize(path);
    this->rebuild();
}
