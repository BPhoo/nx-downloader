/*
    NX Downloader - 项目目录与 url.txt 配置

    约定：
      * 程序启动时，如果 SD 卡上还没有「项目文件夹」，就创建它，
        并在里面生成一份 url.txt 模板；
      * url.txt 的格式为：  { updata: xxx , download: xxx }
          - updata   第一行「访问」按钮用的链接（检查更新）
          - download 第二行「访问」按钮用的链接（下载文件）
      * 解析刻意做得宽松：键名大小写不敏感、: 与 = 都认、值可加引号、
        分号/井号/双斜杠开头的整行按注释忽略、省略 https:// 会自动补上。
*/
#pragma once

#include <string>
#include <vector>

namespace appcfg
{

/// 项目文件夹（.nro 也放在这里）
extern const char* PROJECT_DIR; // "sdmc:/switch/nx-downloader"
/// 配置文件
extern const char* URL_FILE; // "sdmc:/switch/nx-downloader/url.txt"
/// 界面设置（上次的链接、保存目录）
extern const char* SETTINGS_FILE; // "sdmc:/switch/nx-downloader/settings.txt"
/// 可选的 CA 证书包
extern const char* CA_BUNDLE; // "sdmc:/switch/nx-downloader/cacert.pem"
/// 运行日志（每次启动重建，用于真机排错）
extern const char* LOG_FILE; // "sdmc:/switch/nx-downloader/log.txt"
/// 检查更新的返回内容（每次检查后覆盖写入，方便离线查看，也是个兜底）
extern const char* RESULT_FILE; // "sdmc:/switch/nx-downloader/update_result.txt"
/// 网络图片缓存目录（url.txt 里 img: 的图片下载到这里）
extern const char* IMG_DIR; // "sdmc:/switch/nx-downloader/img"
/// 默认下载目录：SD 卡根目录
extern const char* DEFAULT_OUT_DIR; // "sdmc:/"

struct UrlEntry
{
    std::string update;   // url.txt 里的 updata
    std::string download; // url.txt 里的 download
    /// url.txt 里的 img:（逗号分隔，项可以是 http(s) 链接，也可以是本地文件名）
    std::vector<std::string> images;
};

/// 图片缓存的落盘文件名：`<序号>-<原名>`，序号从 1 开始。
/// 带上序号是为了：① 不同图片即使重名也不会互相覆盖；② 改动 url.txt 里的顺序/内容后，
/// 文件名随之改变，不会读到上一次的旧缓存。
std::string imageFileName(size_t index, const std::string& item);

/// 启动时调用：确保项目文件夹存在；没有 url.txt 就写入模板
/// createdDir / createdFile 用来告诉调用方「这次新建了什么」（可为 nullptr）
bool ensureProjectLayout(bool* createdDir = nullptr, bool* createdFile = nullptr);

/// url.txt 模板文本
std::string urlFileTemplate();

/// 宽松解析 "{ updata: a , download: b }"
/// 只要认出任意一项就返回 true，并把结果写进 out（未识别的项保持不变）
bool parseUrlFile(const std::string& text, UrlEntry* out);

/// 从 URL_FILE 读取并解析
bool readUrlFile(UrlEntry* out, std::string* errorText = nullptr);

/// 补全协议：没有 http:// 或 https:// 前缀时按 https:// 处理
std::string normalizeUrl(const std::string& raw);

} // namespace appcfg
