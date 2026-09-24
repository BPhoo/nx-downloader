#include "app_config.hpp"

#include <cctype>

#include "fsx.hpp"

namespace appcfg
{

const char* PROJECT_DIR     = "sdmc:/switch/nx-downloader";
const char* URL_FILE        = "sdmc:/switch/nx-downloader/url.txt";
const char* SETTINGS_FILE   = "sdmc:/switch/nx-downloader/settings.txt";
const char* CA_BUNDLE       = "sdmc:/switch/nx-downloader/cacert.pem";
const char* LOG_FILE        = "sdmc:/switch/nx-downloader/log.txt";
const char* RESULT_FILE     = "sdmc:/switch/nx-downloader/update_result.txt";
const char* IMG_DIR         = "sdmc:/switch/nx-downloader/img";
const char* DEFAULT_OUT_DIR = "sdmc:/";

namespace
{

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string toLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// 以 // 、# 或 ; 开头的行整行当注释丢掉
bool isCommentLine(const std::string& line)
{
    size_t i = line.find_first_not_of(" \t\r\n");
    if (i == std::string::npos)
        return false;

    const char c = line[i];
    if (c == '#' || c == ';')
        return true;
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
        return true;

    return false;
}

std::string stripComments(const std::string& text)
{
    std::string out;
    size_t start = 0;

    while (start <= text.size())
    {
        size_t nl        = text.find('\n', start);
        std::string line = (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);

        if (!isCommentLine(line))
        {
            out += line;
            out += '\n';
        }

        if (nl == std::string::npos)
            break;

        start = nl + 1;
    }

    return out;
}

/// 在 text 里找 key（词边界匹配），把它后面的值取出来
bool extractValue(const std::string& text, const std::string& key, std::string* out)
{
    const std::string lower = toLower(text);
    size_t pos              = 0;

    while (true)
    {
        pos = lower.find(key, pos);
        if (pos == std::string::npos)
            return false;

        // 键的左右必须是「非字母数字」边界，免得把别的单词里的片段当键
        const bool leftOk  = (pos == 0) || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
        const size_t after = pos + key.size();
        const bool rightOk = (after >= lower.size()) || !std::isalnum(static_cast<unsigned char>(lower[after]));

        if (!leftOk || !rightOk)
        {
            pos = after;
            continue;
        }

        size_t i = after;

        // 跳过键与分隔符之间的空白
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
            i++;

        // 分隔符：冒号或等号（也可以没有）
        if (i < text.size() && (text[i] == ':' || text[i] == '='))
        {
            i++;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                i++;
        }

        // 读到 , } 换行 或文本末尾；支持引号包裹
        std::string value;
        bool inQuote = false;
        char quote   = 0;

        for (; i < text.size(); i++)
        {
            const char c = text[i];

            if (inQuote)
            {
                if (c == quote)
                    break;
                value.push_back(c);
                continue;
            }

            if (c == '"' || c == '\'')
            {
                inQuote = true;
                quote   = c;
                continue;
            }

            if (c == ',' || c == '}' || c == '\n' || c == '\r')
                break;

            value.push_back(c);
        }

        value = trim(value);
        if (!value.empty())
        {
            *out = value;
            return true;
        }

        // 这个键后面没值，继续往后找（可能用户写了两次）
        pos = after;
    }
}

/// 按逗号拆成多项（英文 , 与中文 ，都认），去掉空白项
std::vector<std::string> splitCommas(const std::string& raw)
{
    std::vector<std::string> out;
    std::string current;

    for (size_t i = 0; i < raw.size(); i++)
    {
        // 中文逗号 "，" 是 3 字节 UTF-8: EF BC 8C
        const bool cnComma = (i + 2 < raw.size() &&
                              static_cast<unsigned char>(raw[i]) == 0xEF &&
                              static_cast<unsigned char>(raw[i + 1]) == 0xBC &&
                              static_cast<unsigned char>(raw[i + 2]) == 0x8C);

        if (raw[i] == ',' || cnComma)
        {
            const std::string item = trim(current);
            if (!item.empty())
                out.push_back(item);
            current.clear();

            if (cnComma)
                i += 2;
            continue;
        }

        current.push_back(raw[i]);
    }

    const std::string last = trim(current);
    if (!last.empty())
        out.push_back(last);

    return out;
}

/// 读「列表型」的值：一直读到 } 或行尾，再按逗号拆开。
/// 用在 url.txt 的 `img: a.jpg, b.jpg, c.jpg` 这种写法上（普通标量值用 extractValue）。
bool extractList(const std::string& text, const std::string& key, std::vector<std::string>* out)
{
    const std::string lower = toLower(text);
    size_t pos              = 0;
    bool found              = false;

    while (true)
    {
        pos = lower.find(key, pos);
        if (pos == std::string::npos)
            break;

        const bool leftOk  = (pos == 0) || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
        const size_t after = pos + key.size();
        const bool rightOk = (after >= lower.size()) || !std::isalnum(static_cast<unsigned char>(lower[after]));

        if (!leftOk || !rightOk)
        {
            pos = after;
            continue;
        }

        size_t i = after;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
            i++;

        if (i < text.size() && (text[i] == ':' || text[i] == '='))
        {
            i++;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                i++;
        }

        // 读到 } 或换行；引号内的内容原样保留（引号里允许逗号）
        std::string raw;
        bool inQuote = false;
        char quote   = 0;

        for (; i < text.size(); i++)
        {
            const char c = text[i];

            if (inQuote)
            {
                if (c == quote)
                {
                    inQuote = false;
                    continue;
                }
                raw.push_back(c);
                continue;
            }

            if (c == '"' || c == '\'')
            {
                inQuote = true;
                quote   = c;
                continue;
            }

            if (c == '}' || c == '\n' || c == '\r')
                break;

            raw.push_back(c);
        }

        const std::vector<std::string> items = splitCommas(raw);
        if (!items.empty())
        {
            out->insert(out->end(), items.begin(), items.end());
            found = true;
        }

        pos = after;
    }

    return found;
}

} // namespace

std::string urlFileTemplate()
{
    std::string text;
    text += "// NX Downloader 配置文件（直接用文本编辑器改就行，改完重启程序生效）\n";
    text += "// updata  ：检查更新用的链接，对应界面第一行的「访问」\n";
    text += "// download：下载文件用的链接，对应界面第二行的「访问」\n";
    text += "// img     ：要显示的图片，逗号分隔。可以写 http(s) 链接（程序会自动下载并缓存到\n";
    text += "//           img/ 目录），也可以写已经放在 SD 卡上的文件名\n";
    text += "// 链接可以省略 https:// ，程序会自动补上\n";
    text += "{\n";
    text += "    updata:   https://example.com/version.txt ,\n";
    text += "    download: https://example.com/app.nro ,\n";
    text += "    img:      https://example.com/a.jpg, https://example.com/b.jpg\n";
    text += "}\n";
    return text;
}

bool parseUrlFile(const std::string& text, UrlEntry* out)
{
    if (out == nullptr)
        return false;

    const std::string clean = stripComments(text);
    bool any                = false;
    std::string value;

    if (extractValue(clean, "updata", &value) || extractValue(clean, "update", &value))
    {
        out->update = normalizeUrl(value);
        any         = true;
    }

    if (extractValue(clean, "download", &value))
    {
        out->download = normalizeUrl(value);
        any           = true;
    }

    // img: 是「列表值」（可以写多个），而且**刻意不做 normalizeUrl**：
    // 它也可能是本地文件名而不是链接，硬加 https:// 反而会把它弄坏。
    {
        std::vector<std::string> images;
        extractList(clean, "img", &images);
        extractList(clean, "image", &images);
        extractList(clean, "images", &images);

        if (!images.empty())
        {
            out->images = images;
            any         = true;
        }
    }

    return any;
}

std::string imageFileName(size_t index, const std::string& item)
{
    std::string name = fsx::fileNameFromUrl(item);
    if (name.empty())
        name = "image.jpg";

    // 序号前缀：不同图片重名也不会互相覆盖；改了 url.txt 的顺序/内容后
    // 文件名随之变化，也就不会读到上一次的旧缓存。
    return std::to_string(static_cast<unsigned>(index + 1)) + "-" + name;
}

bool readUrlFile(UrlEntry* out, std::string* errorText)
{
    if (out == nullptr)
        return false;

    std::string text;
    if (!fsx::readWholeFile(URL_FILE, &text))
    {
        if (errorText != nullptr)
            *errorText = std::string("读不到配置文件 ") + URL_FILE;
        return false;
    }

    UrlEntry parsed;

    if (!parseUrlFile(text, &parsed))
    {
        if (errorText != nullptr)
            *errorText = std::string("没有从 ") + URL_FILE + " 里解析出 updata / download";
        return false;
    }

    if (!parsed.update.empty())
        out->update = parsed.update;
    if (!parsed.download.empty())
        out->download = parsed.download;

    return true;
}

std::string normalizeUrl(const std::string& raw)
{
    std::string v = trim(raw);

    // 去掉包裹的引号
    if (v.size() >= 2)
    {
        const char f = v.front();
        const char b = v.back();
        if ((f == '"' && b == '"') || (f == '\'' && b == '\''))
            v = trim(v.substr(1, v.size() - 2));
    }

    if (v.empty())
        return v;

    const std::string lower = toLower(v);
    if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0)
        return v;

    return "https://" + v;
}

bool ensureProjectLayout(bool* createdDir, bool* createdFile)
{
    if (createdDir != nullptr)
        *createdDir = false;
    if (createdFile != nullptr)
        *createdFile = false;

    const bool dirExisted = fsx::isDirectory(PROJECT_DIR);

    if (!fsx::ensureDirectory(PROJECT_DIR))
        return false;

    if (!dirExisted && createdDir != nullptr)
        *createdDir = true;

    if (!fsx::exists(URL_FILE))
    {
        if (fsx::writeWholeFile(URL_FILE, urlFileTemplate()) && createdFile != nullptr)
            *createdFile = true;
    }

    return true;
}

} // namespace appcfg
