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

    // B 键返回上一屏
    this->registerAction("返回", Key::B, [] {
        Application::popView();
        return true;
    });

    this->rebuild();
}

PathPickerView::~PathPickerView() = default;

void PathPickerView::requestNavigate(const std::string& path)
{
    // 只记录目标，真正的重建放到下一帧的 draw() 里做
    this->pendingPath = fsx::normalize(path);
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
    // 先把焦点摘掉，避免删掉当前被聚焦的列表项
    Application::giveFocus(nullptr);

    this->clear(true);

    this->addView(new Header("选择下载目录", true, this->currentPath));

    ListItem* useItem = new ListItem("使用此目录", "把文件保存到 " + this->currentPath);
    useItem->setValue("确定");
    useItem->getClickEvent()->subscribe([this](View*) {
        this->confirmCurrent();
    });
    this->addView(useItem);

    // 上级目录
    if (this->currentPath != "sdmc:/")
    {
        std::string parentPath = fsx::parent(this->currentPath);
        ListItem* upItem       = new ListItem("上一级目录", parentPath);
        upItem->setValue("进入");
        upItem->getClickEvent()->subscribe([this, parentPath](View*) {
            this->requestNavigate(parentPath);
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
                this->requestNavigate(childPath);
            });
            this->addView(item);
        }
    }

    // 重建完成后重新给焦点（默认落在「使用此目录」上）
    Application::giveFocus(this->getDefaultFocus());
}

void PathPickerView::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style, FrameContext* ctx)
{
    if (!this->pendingPath.empty())
    {
        this->currentPath = this->pendingPath;
        this->pendingPath.clear();
        this->rebuild();
    }

    List::draw(vg, x, y, width, height, style, ctx);
}
