#include "progress_dialog.hpp"

#include <algorithm>

#include "downloader.hpp"
#include "fsx.hpp"

using namespace brls;

namespace
{
    // 进度条高度，取样式表里的按钮高度，保证和整体 UI 缩放一致
    unsigned progressBarHeight()
    {
        Style* style = Application::getStyle();
        return style != nullptr ? style->CrashFrame.buttonHeight : 60;
    }
} // namespace

//=====================================================================
// 内容视图
//=====================================================================

DownloadProgressContent::DownloadProgressContent(const std::string& title)
    : BoxLayout(BoxLayoutOrientation::VERTICAL)
{
    this->setResize(true);
    this->setSpacing(24);

    this->nameLabel = new Label(LabelStyle::DIALOG, title, true);
    this->nameLabel->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->addView(this->nameLabel);

    this->bar = new ProgressDisplay(ProgressDisplayFlags::PERCENTAGE);
    this->bar->setHeight(progressBarHeight());
    this->addView(this->bar);

    this->detailLabel = new Label(LabelStyle::DIALOG, "正在连接…", true);
    this->detailLabel->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->addView(this->detailLabel);
}

void DownloadProgressContent::setDetail(const std::string& text)
{
    if (this->detailLabel != nullptr)
        this->detailLabel->setText(text);
}

void DownloadProgressContent::setProgress(int percent)
{
    if (this->bar == nullptr)
        return;

    int clamped = std::max(0, std::min(100, percent));
    this->bar->setProgress(clamped, 100);
}

//=====================================================================
// 对话框
//=====================================================================

DownloadProgressDialog::DownloadProgressDialog(const std::string& title, std::function<void()> onCancelRequested)
{
    this->content = new DownloadProgressContent(title);
    this->dialog  = new Dialog(this->content);

    this->dialog->addButton("取消", [onCancelRequested](View*) {
        if (onCancelRequested)
            onCancelRequested();
    });

    // B 键不做任何事，避免「对话框关了但下载还在跑」的错觉
    this->dialog->setCancelable(false);
}

void DownloadProgressDialog::open()
{
    if (this->dialog != nullptr)
        this->dialog->open();
}

void DownloadProgressDialog::close(std::function<void()> cb)
{
    if (this->dialog != nullptr)
        this->dialog->close(cb);
}

void DownloadProgressDialog::update(const Downloader* downloader)
{
    if (downloader == nullptr || this->content == nullptr)
        return;

    const long long downloaded = downloader->downloaded();
    const long long total      = downloader->total();

    switch (downloader->state())
    {
        case Downloader::State::Idle:
        case Downloader::State::Running:
        {
            this->content->setProgress(downloader->percent());

            if (total > 0)
                this->content->setDetail(fsx::formatBytes(downloaded) + " / " + fsx::formatBytes(total));
            else
                this->content->setDetail("已接收 " + fsx::formatBytes(downloaded));

            break;
        }

        case Downloader::State::Finished:
        {
            this->content->setProgress(100);
            this->content->setDetail("已完成 · " + fsx::formatBytes(downloaded));
            break;
        }

        case Downloader::State::Cancelled:
        {
            this->content->setDetail("已取消");
            break;
        }

        case Downloader::State::Failed:
        {
            std::string error = downloader->errorText();
            this->content->setDetail(error.empty() ? "下载失败" : error);
            break;
        }
    }
}
