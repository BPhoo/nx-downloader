#include "downloader.hpp"

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include <curl/curl.h>

#include "app_config.hpp"
#include "fsx.hpp"

namespace
{

// 文本模式最多保留多少响应体（超出部分丢弃，但仍然继续读连接）
constexpr size_t MAX_TEXT_BYTES = 512 * 1024;

//=====================================================================
// 一次传输的上下文，只在工作线程内使用（curl 的回调是同线程串行调用的）
//=====================================================================
struct Transfer
{
    CURL* curl                 = nullptr;
    Downloader::Progress* prog = nullptr;

    FsFile file   = {};
    bool fileOpen = false;
    s64 offset    = 0;

    // 内存模式（检查更新）攒下来的响应体
    std::string body;
    bool truncated = false;

    // 从响应头里抓到的信息
    std::string contentDisposition;

    bool spaceChecked = false;
    bool writeError   = false;
};

//---------------------------------------------------------------------
// 工具函数
//---------------------------------------------------------------------
std::string toLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void setError(Downloader::Progress* prog, const std::string& text)
{
    std::lock_guard<std::mutex> lock(prog->mtx);
    if (prog->error.empty())
        prog->error = text;
}

/// 从 Content-Disposition 里取文件名（支持 filename 与 filename*）
std::string parseDispositionFilename(const std::string& value)
{
    std::string lower = toLower(value);

    // filename*=UTF-8''xxx 优先
    size_t star = lower.find("filename*=");
    if (star != std::string::npos)
    {
        std::string raw = trim(value.substr(star + 10));
        size_t quote    = raw.find("''");
        if (quote != std::string::npos)
            raw = raw.substr(quote + 2);
        else
        {
            size_t eq = raw.find('=');
            if (eq != std::string::npos)
                raw = raw.substr(eq + 1);
        }
        return raw;
    }

    size_t plain = lower.find("filename=");
    if (plain != std::string::npos)
    {
        std::string raw = trim(value.substr(plain + 9));
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
            raw = raw.substr(1, raw.size() - 2);
        return raw;
    }

    return "";
}

/// 查找可用的 CA 证书包（可选，放了就用）
std::string findCaBundle()
{
    static const char* candidates[] = {
        "romfs:/cacert.pem",
        appcfg::CA_BUNDLE,
    };

    for (const char* path : candidates)
    {
        if (fsx::accessible(path))
            return std::string(path);
    }

    return "";
}

std::string currentOutputPath(Downloader::Progress* prog)
{
    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->outputPath;
}

//---------------------------------------------------------------------
// 回调
//---------------------------------------------------------------------
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    Transfer* ctx = static_cast<Transfer*>(userdata);
    size_t len    = size * nmemb;

    if (len == 0)
        return 0;

    if (ctx->prog->cancelRequested.load())
        return 0; // 通知 curl 中止

    //-----------------------------------------------------------------
    // 内存模式（检查更新）：响应体直接攒在内存里，完全不落盘
    //-----------------------------------------------------------------
    if (ctx->prog->mode == Downloader::Mode::ToMemory)
    {
        if (ctx->body.size() < MAX_TEXT_BYTES)
        {
            const size_t room = MAX_TEXT_BYTES - ctx->body.size();
            ctx->body.append(ptr, len < room ? len : room);
        }
        else
        {
            // 超出上限就丢弃多余数据 —— 但仍然返回 len 把连接读完，
            // 否则 curl 会以写错误收场，把一次正常的传输判成失败。
            ctx->truncated = true;
        }

        return len;
    }

    // 第一次拿到数据时才能确定最终文件名
    // （可能来自重定向后的 URL，或 Content-Disposition）
    if (!ctx->fileOpen)
    {
        std::string name = fsx::sanitizeFileName(parseDispositionFilename(ctx->contentDisposition));

        if (name.empty() && ctx->curl != nullptr)
        {
            char* effective = nullptr;
            if (curl_easy_getinfo(ctx->curl, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective != nullptr)
                name = fsx::fileNameFromUrl(effective);
        }

        if (name.empty())
            name = "download.bin";

        std::string target = fsx::join(ctx->prog->destDir, name);

        if (!fsx::createAndOpenFile(target, &ctx->file))
        {
            ctx->writeError = true;
            setError(ctx->prog, "无法在目标目录创建文件：" + target);
            return 0;
        }

        ctx->fileOpen = true;

        {
            std::lock_guard<std::mutex> lock(ctx->prog->mtx);
            ctx->prog->fileName   = name;
            ctx->prog->outputPath = target;
        }
    }

    Result rc = fsFileWrite(&ctx->file, ctx->offset, ptr, len, FsWriteOption_None);
    if (R_FAILED(rc))
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "写入 SD 卡失败 (0x%08x)", rc);
        ctx->writeError = true;
        setError(ctx->prog, buf);
        return 0;
    }

    ctx->offset += static_cast<s64>(len);
    return len;
}

int xferInfoCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    Transfer* ctx = static_cast<Transfer*>(userdata);

    if (ctx->prog->cancelRequested.load())
        return 1;

    if (dltotal > 0)
    {
        ctx->prog->total.store(static_cast<long long>(dltotal));

        long long percent = static_cast<long long>(dlnow) * 100 / static_cast<long long>(dltotal);
        percent           = std::max<long long>(0, std::min<long long>(100, percent));
        ctx->prog->percent.store(static_cast<int>(percent));

        // 剩余空间只检查一次（只有落盘模式才有意义）
        if (ctx->prog->mode == Downloader::Mode::ToFile && !ctx->spaceChecked)
        {
            ctx->spaceChecked = true;

            s64 freeSpace = 0;
            if (fsx::getFreeSpace(ctx->prog->destDir, &freeSpace))
            {
                const s64 margin = 16LL * 1024 * 1024; // 预留 16MB
                if (freeSpace < static_cast<s64>(dltotal) + margin)
                {
                    setError(ctx->prog,
                        "SD 卡剩余空间不足：需要 " + fsx::formatBytes(static_cast<s64>(dltotal)) +
                            "，可用 " + fsx::formatBytes(freeSpace));
                    return 1;
                }
            }
        }
    }

    ctx->prog->downloaded.store(static_cast<long long>(dlnow));
    return 0;
}

size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    Transfer* ctx = static_cast<Transfer*>(userdata);
    size_t len    = size * nitems;

    std::string line(buffer, len);
    const std::string key = "content-disposition:";

    if (line.size() > key.size() && toLower(line.substr(0, key.size())) == key)
        ctx->contentDisposition = trim(line.substr(key.size()));

    return len;
}

/// 证书链问题：值得用「不校验证书」再试一次
bool isCertError(CURLcode code)
{
    return code == CURLE_PEER_FAILED_VERIFICATION ||
           code == CURLE_SSL_CACERT_BADFILE ||
           code == CURLE_SSL_CERTPROBLEM;
}

void applyOptions(CURL* curl, Transfer* ctx, const std::string& url, const std::string& caBundle, bool verify)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nx-downloader/1.0 (Nintendo Switch; libnx)");
    // 刻意不设置 CURLOPT_ACCEPT_ENCODING：
    //   一旦启用自动解压，CURLINFO_CONTENT_LENGTH_DOWNLOAD_T 给出的是「服务器声明的
    //   压缩后长度」，而实际写盘的是解压后的数据，两者对不上会被下面的完整性校验
    //   误判成 PARTIAL_FILE。下载器要的就是原始字节流，不请求压缩最简单也最稳。
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // 只允许 http/https，避免被恶意服务器重定向到 file:// 之类的协议
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    if (verify)
    {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!caBundle.empty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
    }
    else
    {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

} // namespace

//=====================================================================
// Downloader
//=====================================================================

Downloader::~Downloader()
{
    this->requestCancel();
    this->join();
}

bool Downloader::isSupportedUrl(const std::string& url)
{
    std::string lower = toLower(trim(url));
    return lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0;
}

void* Downloader::threadEntry(void* arg)
{
    static_cast<Downloader*>(arg)->run();
    return nullptr;
}

bool Downloader::start(const std::string& url, const std::string& destDirectory)
{
    return this->startInternal(url, destDirectory, Mode::ToFile);
}

bool Downloader::startFetchText(const std::string& url)
{
    return this->startInternal(url, "", Mode::ToMemory);
}

bool Downloader::startInternal(const std::string& url, const std::string& destDirectory, Mode mode)
{
    // 上一轮线程理论上已经结束，这里只是兜底
    this->join();

    if (!isSupportedUrl(url))
        return false;

    // 只有落盘模式才需要目标目录
    if (mode == Mode::ToFile && !fsx::ensureDirectory(destDirectory))
        return false;

    auto prog     = std::make_shared<Progress>();
    prog->mode    = mode;
    prog->url     = trim(url);
    prog->destDir = fsx::normalize(destDirectory);

    this->progress = prog;

    // 关键：用 pthread 显式指定一个宽裕的栈（2MiB）。
    // libnx 的 std::thread 默认栈只有 128KB，跑 libcurl + mbedTLS 的 TLS 握手会
    // 直接栈溢出（表现为 2168-0002 数据中止，且崩溃现场 PC 指向乱七八糟的地方）。
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // 2 * 1024 * 1024 = 2MiB，足够 TLS 握手 + curl 内部栈帧，并留足余量。
        pthread_attr_setstacksize(&attr, 2u * 1024u * 1024u);
        const int rc = pthread_create(&this->worker, &attr, &Downloader::threadEntry, this);
        pthread_attr_destroy(&attr);

        if (rc != 0)
        {
            // 线程都没起得来：直接报错，不要让 UI 卡在「下载中」
            setError(prog.get(), "无法创建工作线程（pthread_create 失败）");
            prog->state.store(State::Failed);
            this->workerValid = false;
            return false;
        }
        this->workerValid = true;
    }

    return true;
}

void Downloader::requestCancel()
{
    std::shared_ptr<Progress> prog = this->progress;
    if (prog)
        prog->cancelRequested.store(true);
}

void Downloader::join()
{
    if (this->workerValid)
    {
        pthread_join(this->worker, nullptr);
        this->workerValid = false;
    }
}

bool Downloader::running() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return false;

    State s = prog->state.load();
    return s == State::Running || s == State::Idle;
}

Downloader::State Downloader::state() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->state.load() : State::Idle;
}

Downloader::Mode Downloader::mode() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->mode : Mode::ToFile;
}

long Downloader::httpStatus() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->httpStatus.load() : 0;
}

std::string Downloader::bodyText() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return "";

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->body;
}

bool Downloader::bodyTruncated() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return false;

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->truncated;
}

int Downloader::percent() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->percent.load() : 0;
}

long long Downloader::downloaded() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->downloaded.load() : 0;
}

long long Downloader::total() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->total.load() : 0;
}

bool Downloader::tlsVerified() const
{
    std::shared_ptr<Progress> prog = this->progress;
    return prog ? prog->tlsVerified.load() : false;
}

std::string Downloader::errorText() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return "";

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->error;
}

std::string Downloader::outputPath() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return "";

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->outputPath;
}

std::string Downloader::fileName() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return "";

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->fileName;
}

std::string Downloader::finalUrl() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return "";

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->finalUrl;
}

void Downloader::run()
{
    // 自己持有一份 shared_ptr：保证整个下载过程中状态对象一定存活
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return;

    prog->state.store(State::Running);

    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        setError(prog.get(), "初始化 libcurl 失败");
        prog->state.store(State::Failed);
        return;
    }

    const std::string caBundle = findCaBundle();

    CURLcode rc = CURLE_OK;

    // 第一次带证书校验；如果设备上没有可用的证书链导致失败，
    // 再降级重试一次，并在 UI 上明确提示「未校验证书」。
    for (int attempt = 0; attempt < 2; attempt++)
    {
        const bool verify = (attempt == 0);

        Transfer ctx;
        ctx.curl = curl;
        ctx.prog = prog.get();

        prog->percent.store(0);
        prog->cancelRequested.store(false);

        applyOptions(curl, &ctx, prog->url, caBundle, verify);

        rc = curl_easy_perform(curl);

        if (ctx.fileOpen)
        {
            fsFileFlush(&ctx.file);
            fsFileClose(&ctx.file);
            ctx.fileOpen = false;
        }

        // 内存模式：把这一轮拿到的正文交给共享状态（UI 线程随后可读）
        if (prog->mode == Mode::ToMemory)
        {
            std::lock_guard<std::mutex> lock(prog->mtx);
            prog->body      = ctx.body;
            prog->truncated = ctx.truncated;
        }

        if (rc == CURLE_OK)
        {
            prog->tlsVerified.store(verify);

            // 尺寸校验：服务器声明了长度就必须完全一致。
            // 内存模式不适用 —— offset 恒为 0，而且可能被主动截断。
            if (prog->mode == Mode::ToFile)
            {
                curl_off_t expected = -1;
                curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected);

                if (expected > 0 && ctx.offset != static_cast<s64>(expected))
                    rc = CURLE_PARTIAL_FILE;
            }

            break;
        }

        if (prog->cancelRequested.load() || ctx.writeError || !verify || !isCertError(rc))
            break;

        // 证书问题：删掉半成品，换成「不校验证书」再试一次
        std::string partial = currentOutputPath(prog.get());
        if (!partial.empty())
            fsx::removeFile(partial);

        {
            std::lock_guard<std::mutex> lock(prog->mtx);
            prog->outputPath.clear();
            prog->fileName.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(prog->mtx);
        if (prog->finalUrl.empty())
            prog->finalUrl = prog->url;
    }

    // 记下 HTTP 响应码（必须在 curl_easy_cleanup 之前取）
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    prog->httpStatus.store(httpCode);

    curl_easy_cleanup(curl);

    // HTTP 层面的错误（404 / 500 之类）也必须当失败处理：
    // 否则会把服务器返回的错误页面当成「下载成功」存到 SD 卡上。
    if (rc == CURLE_OK && httpCode >= 400)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "服务器返回 HTTP %ld", httpCode);
        setError(prog.get(), buf);
        rc = CURLE_HTTP_RETURNED_ERROR;
    }

    if (rc == CURLE_OK)
    {
        prog->percent.store(100);
        prog->state.store(State::Finished);
        return;
    }

    // 失败 / 取消：删掉半成品，避免留下损坏文件
    {
        std::string partial = currentOutputPath(prog.get());
        if (!partial.empty())
        {
            fsx::removeFile(partial);
            std::lock_guard<std::mutex> lock(prog->mtx);
            prog->outputPath.clear();
        }
    }

    if (prog->cancelRequested.load())
    {
        prog->state.store(State::Cancelled);
        return;
    }

    // 失败路径：如果还没有具体错误信息，就按 curl 的返回码补一条
    bool hasError = false;
    {
        // error 是受 mtx 保护的 std::string，读之前必须持锁（否则是数据竞争）
        std::lock_guard<std::mutex> lock(prog->mtx);
        hasError = !prog->error.empty();
    }

    if (!hasError)
    {
        std::string text;
        if (rc == CURLE_PARTIAL_FILE)
            text = "下载不完整：文件长度与服务器声明不符";
        else
            text = std::string("下载失败：") + curl_easy_strerror(rc);

        setError(prog.get(), text);
    }

    prog->state.store(State::Failed);
}
