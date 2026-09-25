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
#include "logx.hpp"

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

        // 期望长度（xferInfoCallback 从 Content-Length 拿到的，存在 prog->total）能预先
        // 知道时，直接让 fsx 把文件长度定好：后续写入都落在长度之内，就不会触发 FS 的
        // 「隐式扩大文件」检查（缺 Append 位时那个检查会返回 0x00307202）。
        // 上限 256MB —— 再大就让 Append 位随写随扩，免得建文件时长时间阻塞。
        const long long hintTotal = ctx->prog->total.load();
        const s64 prealloc        = (hintTotal > 0 && hintTotal <= 256LL * 1024 * 1024)
                                        ? static_cast<s64>(hintTotal)
                                        : 0;

        if (!fsx::createAndOpenFile(target, &ctx->file, prealloc))
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

    // 必须走 fsx::writeFileChunk：它内部持锁，避免和 UI 线程的 SD 写入
    // 撞在同一个 libnx IPC 会话上（那种竞争会表现为卡死/写坏文件）
    const Result writeRc = fsx::writeFileChunk(&ctx->file, ctx->offset, ptr, len);
    if (R_FAILED(writeRc))
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "写入 SD 卡失败 (%s)", logx::result(writeRc).c_str());
        ctx->writeError = true;
        setError(ctx->prog, buf);
        logx::linef("写入 SD 卡失败 = %s", logx::result(writeRc).c_str());
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

/// 这个错误值不值得「换一条更保守的传输配置」再试一次。
///
/// ★ 覆盖范围必须包含 CURLE_SSL_CONNECT_ERROR(35) —— 这是真机日志里踩到的：
///   Steam 的 CDN（Fastly）在 Switch 的 mbedTLS 上握手失败，报的正是 35；
///   而国内网盘（证书链不全）报的是 60。
///   v2.6.0 只认 60/58/77，所以那 4 张图片一次就放弃了
///   （日志里每张只有一行 curl_easy_perform，等于没走降级）。
bool isSslRetryable(CURLcode code)
{
    switch (code)
    {
        case CURLE_PEER_FAILED_VERIFICATION: // 60 证书链验证失败（最典型）
        case CURLE_SSL_CONNECT_ERROR:        // 35 SSL 握手失败（ALPN / TLS 版本 / CDN 差异）
        case CURLE_SSL_CERTPROBLEM:          // 58
        case CURLE_SSL_CIPHER:               // 59
        case CURLE_SSL_CACERT_BADFILE:       // 77
        case CURLE_SSL_ISSUER_ERROR:         // 83
        case CURLE_SSL_CRL_BADFILE:          // 82
        case CURLE_SSL_INVALIDCERTSTATUS:    // 91
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH: // 90
        case CURLE_SSL_SHUTDOWN_FAILED:      // 80
        case CURLE_SSL_ENGINE_NOTFOUND:      // 53
        case CURLE_SSL_ENGINE_SETFAILED:     // 54
            return true;
        default:
            return false;
    }
}

/// 传输档位：一档比一档保守，失败就降一档重试
enum class TlsStage
{
    Verify = 0,      // ① 正常校验 HTTPS 证书
    NoVerify,        // ② 关掉证书校验（设备上没有 CA 链时的兜底）
    Tls12Only,       // ③ 不校验 + HTTP/1.1 + **把 TLS 钉死在 1.2**
    Tls10Only,       // ④ 不校验 + HTTP/1.1 + **把 TLS 钉死在 1.0**（最保守的一档）
};

const char* tlsStageName(TlsStage stage)
{
    switch (stage)
    {
        case TlsStage::Verify:
            return "校验证书";
        case TlsStage::NoVerify:
            return "不校验证书";
        case TlsStage::Tls12Only:
            return "不校验+HTTP1.1+仅TLS1.2";
        default:
            return "不校验+HTTP1.1+仅TLS1.0";
    }
}

void applyOptions(CURL* curl, Transfer* ctx, const std::string& url, const std::string& caBundle, TlsStage stage)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 第一次给足 15 秒；降档重试时 8 秒就够 ——
    // 既然前一档已经失败过，多半是连不上，没必要每次都等满（4 档下来也不过 ~39 秒）
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (stage == TlsStage::Verify) ? 15L : 8L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nx-downloader/2.1 (Nintendo Switch; libnx)");
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

    if (stage == TlsStage::Verify)
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

    // 后面两档：把协议栈压到最保守的组合。
    //
    //   ★ 为什么要把 TLS 版本**钉死**（而不只是设最低版本）：
    //     `CURLOPT_SSLVERSION = CURL_SSLVERSION_TLSv1_2` 在 libcurl 里的含义是
    //    「**最低** 1.2」，仍然允许协商到 1.3。而 switch-curl 走的是 mbedTLS 2.x，
    //     它对 TLS1.3 的支持并不完整 —— 一旦握手往 1.3 走，失败时 libcurl 只报一句
    //     CURLE_SSL_CONNECT_ERROR(35)，看不出是版本问题。
    //     真机实测 shared.fastly.steamstatic.com 三档（校验 / 不校验 /
    //     不校验+HTTP1.1+最低TLS1.2）全部 35，所以这里补两档把上限也钉死。
    //
    //   写法：`min | max`，libcurl 7.54.1 起支持 CURL_SSLVERSION_MAX_*。
    if (stage == TlsStage::Tls12Only || stage == TlsStage::Tls10Only)
    {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));

        const bool v12 = (stage == TlsStage::Tls12Only);

        // ★ MAX_* 这一族的名字带版本号后缀：是 CURL_SSLVERSION_MAX_TLSv1_0，
        //   不是 CURL_SSLVERSION_MAX_TLSv1（后者不存在，编译期就会报未声明）。
        const long minVer = v12 ? CURL_SSLVERSION_TLSv1_2 : CURL_SSLVERSION_TLSv1;
        const long maxVer = v12 ? CURL_SSLVERSION_MAX_TLSv1_2 : CURL_SSLVERSION_MAX_TLSv1_0;

        curl_easy_setopt(curl, CURLOPT_SSLVERSION, minVer | maxVer);
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

bool Downloader::startAs(const std::string& url, const std::string& destDirectory, const std::string& fileName)
{
    return this->startInternal(url, destDirectory, Mode::ToFile, fileName);
}

bool Downloader::startFetchText(const std::string& url)
{
    return this->startInternal(url, "", Mode::ToMemory);
}

bool Downloader::startInternal(const std::string& url, const std::string& destDirectory, Mode mode,
    const std::string& forcedFileName)
{
    // 上一轮线程理论上已经结束，这里只是兜底
    this->join();

    if (!isSupportedUrl(url))
        return false;

    // 只有落盘模式才需要目标目录
    if (mode == Mode::ToFile && !fsx::ensureDirectory(destDirectory))
        return false;

    auto prog            = std::make_shared<Progress>();
    prog->mode           = mode;
    prog->url            = trim(url);
    prog->destDir        = fsx::normalize(destDirectory);
    prog->forcedFileName = forcedFileName;

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

std::string Downloader::diagText() const
{
    std::shared_ptr<Progress> prog = this->progress;
    if (!prog)
        return "";

    std::lock_guard<std::mutex> lock(prog->mtx);
    return prog->diag;
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

    // ★ 拿到 curl / mbedTLS 的**原文**错误文本。
    //   curl_easy_strerror() 只有一句「SSL connect error」，看不出到底是
    //   连接被重置、对端不支持协议、还是证书问题。握不上手时这是唯一的线索。
    char errBuf[CURL_ERROR_SIZE];
    std::memset(errBuf, 0, sizeof(errBuf));
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errBuf);

    const std::string caBundle = findCaBundle();

    logx::linef("网络任务开始(%s): %s", prog->mode == Mode::ToMemory ? "取文本" : "下载文件",
        prog->url.c_str());
    logx::linef("目标目录=%s，CA 包=%s", prog->destDir.c_str(),
        caBundle.empty() ? "(无)" : caBundle.c_str());

    CURLcode rc = CURLE_OK;

    // 最多四档，一档比一档保守；每一档失败且错误属于「SSL 类」时，
    // 删掉半成品再降一档重试。真机实测的落点：
    //   国内网盘（证书链不全）      → 第 ② 档就通
    //   Steam CDN（mbedTLS 版本问题）→ 需要第 ③/④ 档把 TLS 版本钉死
    static const TlsStage STAGES[] = {
        TlsStage::Verify,    // ① 校验证书
        TlsStage::NoVerify,  // ② 不校验证书
        TlsStage::Tls12Only, // ③ 不校验 + HTTP/1.1 + 只用 TLS1.2
        TlsStage::Tls10Only, // ④ 不校验 + HTTP/1.1 + 只用 TLS1.0
    };
    constexpr int STAGE_COUNT = static_cast<int>(sizeof(STAGES) / sizeof(STAGES[0]));

    for (int attempt = 0; attempt < STAGE_COUNT; attempt++)
    {
        const TlsStage stage = STAGES[attempt];

        Transfer ctx;
        ctx.curl = curl;
        ctx.prog = prog.get();

        prog->percent.store(0);
        prog->cancelRequested.store(false);
        errBuf[0] = '\0';

        applyOptions(curl, &ctx, prog->url, caBundle, stage);

        rc = curl_easy_perform(curl);

        logx::linef("curl_easy_perform(第%d/%d次, %s) = %d (%s)，落盘 %lld 字节 / 内存 %u 字节",
            attempt + 1, STAGE_COUNT, tlsStageName(stage),
            static_cast<int>(rc), curl_easy_strerror(rc),
            static_cast<long long>(ctx.offset), static_cast<unsigned>(ctx.body.size()));

        if (rc != CURLE_OK)
        {
            // 握手/连接失败时，这四个数字是唯一定性的依据：
            //   OS errno  : 0=协议层失败；104(ECONNRESET)/110(ETIMEDOUT)/113=被中途掐断
            //   对端 IP   : 能连上说明 DNS 与 TCP 都通，问题在 TLS 之后
            //   SSL verify: 证书验证结果（非 0 时才是证书问题）
            long osErrno = 0;
            curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &osErrno);

            const char* ip = nullptr;
            curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);

            long sslResult = 0;
            curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &sslResult);

            logx::linef("  | curl 原文：%s", errBuf[0] != '\0' ? errBuf : "(curl 未给出文本)");
            logx::linef("  + OS errno=%ld，对端 IP=%s，SSL verify=%ld",
                osErrno, (ip != nullptr && ip[0] != '\0') ? ip : "(未连上)", sslResult);

            // ★ 第一次（正常配置那一档）的细节最有诊断价值，存下来给界面显示：
            //   用户不用连电脑取 log.txt，在诊断区里截个图就行。
            //   SSL 类错误只报一句「35」根本判断不出原因，这四个数字才是依据。
            if (attempt == 0)
            {
                std::lock_guard<std::mutex> lock(prog->mtx);

                char head[256];
                std::snprintf(head, sizeof(head), "curl=%d(%s) OS errno=%ld 对端 IP=%s SSL verify=%ld",
                    static_cast<int>(rc), curl_easy_strerror(rc), osErrno,
                    (ip != nullptr && ip[0] != '\0') ? ip : "(未连上)", sslResult);

                prog->diag = std::string(head);
                if (errBuf[0] != '\0')
                    prog->diag += "\n原文：" + std::string(errBuf);
            }
        }

        if (ctx.fileOpen)
        {
            fsx::flushAndCloseFile(&ctx.file);
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
            prog->tlsVerified.store(stage == TlsStage::Verify);

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

        // 用户取消 / 写盘失败 / 错误不属于 SSL 类 → 不再折腾
        if (prog->cancelRequested.load() || ctx.writeError || !isSslRetryable(rc))
            break;

        // 已经是最后一档 → 结束
        if (attempt + 1 >= STAGE_COUNT)
            break;

        logx::linef("降档重试：%s → %s（原因：%s）",
            tlsStageName(stage), tlsStageName(STAGES[attempt + 1]), curl_easy_strerror(rc));

        // 删掉半成品，并清掉这一轮定下的文件名，让下一轮重新来
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

    logx::linef("HTTP 响应码 = %ld", httpCode);

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

        // 取文本模式：把正文落到 SD 卡。
        // 这样即使界面侧出了任何问题，返回内容也一定拿得到，方便离线查看。
        if (prog->mode == Mode::ToMemory)
        {
            std::string body;
            bool truncated = false;
            {
                std::lock_guard<std::mutex> lock(prog->mtx);
                body      = prog->body;
                truncated = prog->truncated;
            }

            if (body.empty())
                logx::line("返回内容为空");
            else if (fsx::writeWholeFile(appcfg::RESULT_FILE, body))
                logx::linef("返回内容已存入 %s（%u 字节，截断=%d）",
                    appcfg::RESULT_FILE, static_cast<unsigned>(body.size()), truncated ? 1 : 0);
            else
                logx::linef("返回内容写入 %s 失败", appcfg::RESULT_FILE);
        }

        logx::linef("网络任务成功：%s", currentOutputPath(prog.get()).c_str());

        // ★★ 终态必须是**最后一步**。
        //    UI 线程一旦看到 Finished，就说明工作线程该做的都做完了
        //    （包括日志落盘、返回内容存盘），此后不会再写任何共享数据 ——
        //    这是跨线程交接里最重要的一条顺序约定。
        //    v2.1.1 及以前是先置状态再写日志，UI 可能在 worker 还在写盘时
        //    就开始处理结果。
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
        logx::line("网络任务被取消");

        // 终态最后置位（同成功路径的理由）
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

    logx::linef("网络任务失败：curl=%d (%s)", static_cast<int>(rc), curl_easy_strerror(rc));

    // 终态最后置位（同成功路径的理由）：
    // UI 看到 Failed 时，error 文本一定已经写好、日志也已经落盘。
    prog->state.store(State::Failed);
}
