/*
    NX Downloader - 下载引擎

    设计要点：
      * 所有网络 + 写盘都在【工作线程】里完成，绝不碰任何 UI 对象；
      * UI 线程只通过 shared_ptr<Progress> 里的原子变量读取进度，天然线程安全；
      * shared_ptr 保证工作线程运行期间 Progress 一定存活（不会 use-after-free）；
      * 支持取消（curl 的 xferinfo 回调返回非 0 即中止）。
*/
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>

class Downloader
{
  public:
    enum class State
    {
        Idle = 0,   // 未开始
        Running,    // 下载中
        Finished,   // 完成
        Failed,     // 失败
        Cancelled,  // 被取消
    };

    /// 工作线程与 UI 线程共享的状态，读取方只需要看原子变量
    struct Progress
    {
        std::atomic<State> state{State::Idle};
        std::atomic<bool> cancelRequested{false};
        std::atomic<int> percent{0};
        std::atomic<long long> downloaded{0};
        std::atomic<long long> total{0};

        // 是否真正校验过证书。UI 线程在工作线程运行期间就可能读到它，
        // 所以必须是原子的，不能放在下面那组「持锁访问」的字段里。
        std::atomic<bool> tlsVerified{false};

        //------------------------- 以下字段必须持 mtx 访问 -------------------------
        // url / destDir 是个例外：它们只在 start() 里写一次，且写入发生在
        // std::thread 创建之前，因此工作线程读取它们是安全的（有 happens-before 保证）。
        std::mutex mtx;
        std::string url;
        std::string destDir;
        std::string fileName;
        std::string outputPath;
        std::string finalUrl;
        std::string error;
    };

    Downloader() = default;
    ~Downloader();

    Downloader(const Downloader&)            = delete;
    Downloader& operator=(const Downloader&) = delete;

    /// 开始下载。url 必须是 http/https，destDirectory 形如 "sdmc:/downloads"
    bool start(const std::string& url, const std::string& destDirectory);

    /// 请求取消当前下载（不阻塞）
    void requestCancel();

    /// 等待工作线程退出（若已退出则立即返回）
    void join();

    /// 线程是否仍在跑
    bool running() const;

    //---------------- 供 UI 线程读取的只读接口 ----------------//
    State state() const;
    int percent() const;
    long long downloaded() const;
    long long total() const;
    bool tlsVerified() const;
    std::string errorText() const;
    std::string outputPath() const;
    std::string fileName() const;
    std::string finalUrl() const;

    /// 检查 URL 是否是我们支持的协议
    static bool isSupportedUrl(const std::string& url);

  private:
    // pthread 入口：C++ 调用约定与 void*(*)(void*) 兼容（ARM/EABI 上无差别）
    static void* threadEntry(void* arg);

    void run();

    std::shared_ptr<Progress> progress;

    // 工作线程本身。libnx 上 std::thread 创建的线程默认栈只有 128KB，
    // 而 libcurl + mbedTLS 的 TLS 握手栈用量很容易超过它 → 栈溢出（2168-0002）。
    // 因此这里改用手动 pthread_create，并显式指定一个宽裕的栈（2MiB）。
    pthread_t worker  = {};
    bool workerValid  = false;
};
