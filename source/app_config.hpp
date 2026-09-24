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
/// 默认下载目录：SD 卡根目录
extern const char* DEFAULT_OUT_DIR; // "sdmc:/"

struct UrlEntry
{
    std::string update;   // url.txt 里的 updata
    std::string download; // url.txt 里的 download
};

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
