#include "fsx.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

namespace fsx
{

namespace
{
    FsFileSystem g_sd = {};
    bool g_opened    = false;

    // SD 卡 I/O 串行化：libnx 的 FsFileSystem 是 IPC 会话，不能并发使用。
    // 用 recursive_mutex：公开接口之间会互相调用（如 createAndOpenFile → exists）。
    std::recursive_mutex g_ioMutex;

    // 一次读取的目录项数量，FsDirectoryEntry 较大（约 0x310 字节），别开太大
    constexpr size_t DIR_BATCH = 16;

    bool isDigitHex(char c)
    {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    }

    std::string trim(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }
} // namespace

//============================== 生命周期 ==============================//

bool init()
{
    if (g_opened)
        return true;

    FsFileSystem fs = {};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs)))
        return false;

    g_sd     = fs;
    g_opened = true;

    // ⚠️ 这里**不要**再挂 `fsdevMountSdmc()`。
    //    它会再开一个 SD 卡会话（多一个 FS client）。
    //    本程序自己的读写全部走上面的裸 FsFileSystem，第三方库（nanovg/stb）读图片
    //    也已经改成「自己把字节读出来再解码」，**根本不需要 devoptab**。
    //    而 Applet 模式（从相册启动）下 SD 卡本来就被父 applet / 系统共用，
    //    多开一个会话只会增加互相干扰的机会（真机上出现的是整机死机）。
    //    真需要 stdio 访问 sdmc: 时再单独加，并配合焦点状态判断。

    return true;
}

void exit()
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return;

    fsFsClose(&g_sd);
    g_sd     = {};
    g_opened = false;
}

//============================== 查询 / 操作 ==============================//

bool exists(const std::string& sdmcPath)
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    FsDirEntryType type = {};
    return R_SUCCEEDED(fsFsGetEntryType(&g_sd, toFsPath(sdmcPath).c_str(), &type));
}

bool isDirectory(const std::string& sdmcPath)
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    FsDirEntryType type = {};
    if (R_FAILED(fsFsGetEntryType(&g_sd, toFsPath(sdmcPath).c_str(), &type)))
        return false;

    return type == FsDirEntryType_Dir;
}

bool removeFile(const std::string& sdmcPath)
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    Result rc = fsFsDeleteFile(&g_sd, toFsPath(sdmcPath).c_str());
    // 0x202（未找到）等错误直接忽略
    return R_SUCCEEDED(rc) || !exists(sdmcPath);
}

bool createDirectory(const std::string& sdmcPath)
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    std::string path = toFsPath(sdmcPath);
    if (path == "/")
        return true;
    if (isDirectory(path))
        return true;

    return R_SUCCEEDED(fsFsCreateDirectory(&g_sd, path.c_str()));
}

bool ensureDirectory(const std::string& sdmcPath)
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    std::string full  = normalize(sdmcPath);
    std::string fsPath = toFsPath(full);

    if (fsPath == "/")
        return true;
    if (isDirectory(full))
        return true;

    // 逐级创建
    std::string built = "/";
    size_t start      = 1;
    while (start <= fsPath.size())
    {
        size_t slash = fsPath.find('/', start);
        std::string seg = (slash == std::string::npos) ? fsPath.substr(start) : fsPath.substr(start, slash - start);

        if (!seg.empty())
        {
            if (built.size() > 1)
                built += "/";
            built += seg;

            Result rc = fsFsCreateDirectory(&g_sd, built.c_str());
            if (R_FAILED(rc) && !isDirectory(built))
                return false;
        }

        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    return isDirectory(full);
}

bool createAndOpenFile(const std::string& sdmcPath, FsFile* out, s64 preallocSize)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened || out == nullptr)
        return false;

    std::string path = toFsPath(sdmcPath);

    // 已存在先删掉，保证是全新文件（避免续写导致内容错乱）
    if (exists(sdmcPath))
    {
        if (!removeFile(sdmcPath))
            return false;
    }

    // 预先把文件长度定下来（能给就给）。
    // 好处：后续 fsFileWrite 全在长度之内，根本不会触发「隐式扩大文件」那条路，
    // 同时也少了一堆簇分配的抖动。预分配失败就退回 0 字节，由 Append 位兜底。
    Result rc = 0;
    if (preallocSize > 0)
        rc = fsFsCreateFile(&g_sd, path.c_str(), preallocSize, 0);
    else
        rc = fsFsCreateFile(&g_sd, path.c_str(), 0, 0);

    if (R_FAILED(rc))
        rc = fsFsCreateFile(&g_sd, path.c_str(), 0, 0);

    if (R_FAILED(rc))
        return false;

    // ★★ 必须带 FsOpenMode_Append —— 这不是「只能追加」，而是 Nintendo FS 的
    //    **OpenMode_AllowAppend**：允许 WriteFile 隐式扩大文件长度。
    //    官方错误码表原文：
    //      0x307202  6201  "OpenMode_AllowAppend is required for implicit
    //                        extension of file size by WriteFile()."
    //    真机实测就是踩在这里：只开 FsOpenMode_Write 时，往新文件里写数据被拒，
    //    下载直接报「写入 SD 卡失败（0x00307202）」。
    //    libnx 自己的 fsdev 也是这么开的：
    //      case O_WRONLY: fsdev_flags |= FsOpenMode_Write | FsOpenMode_Append;
    //    写入偏移仍然按我们传的 offset 走，不是强制追加到末尾。
    rc = fsFsOpenFile(&g_sd, path.c_str(), FsOpenMode_Write | FsOpenMode_Append, out);
    if (R_FAILED(rc))
    {
        fsFsDeleteFile(&g_sd, path.c_str());
        return false;
    }

    return true;
}

Result writeFileChunk(FsFile* file, s64 offset, const void* data, u64 size, bool flush)
{
    if (file == nullptr || data == nullptr || size == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    return fsFileWrite(file, offset, data, size, flush ? FsWriteOption_Flush : FsWriteOption_None);
}

void flushAndCloseFile(FsFile* file)
{
    if (file == nullptr)
        return;

    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    fsFileFlush(file);
    fsFileClose(file);
}

bool openForAppend(const std::string& sdmcPath, FsFile* out, s64* outOffset)
{
    // createAndOpenFile 会「先删再建」，也就是每次启动把日志清空重来（我们就是要这样），
    // 而且它打开的正是 FsOpenMode_Write | FsOpenMode_Append。
    if (!createAndOpenFile(sdmcPath, out, 0))
        return false;

    if (outOffset != nullptr)
        *outOffset = 0;

    return true;
}

bool listSubDirectories(const std::string& sdmcPath, std::vector<std::string>* names)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened || names == nullptr)
        return false;

    names->clear();

    FsDir dir = {};
    Result rc = fsFsOpenDirectory(&g_sd, toFsPath(sdmcPath).c_str(), FsDirOpenMode_ReadDirs, &dir);
    if (R_FAILED(rc))
        return false;

    FsDirectoryEntry entries[DIR_BATCH];

    // fsDirRead 的第二个参数是 in/out：返回本次实际读到的条目数，0 表示目录已读完
    s64 count = 0;
    while (true)
    {
        count = 0;
        rc    = fsDirRead(&dir, &count, DIR_BATCH, entries);
        if (R_FAILED(rc) || count == 0)
            break;

        for (s64 i = 0; i < count; i++)
        {
            if (entries[i].type != FsDirEntryType_Dir)
                continue;

            std::string name = entries[i].name;
            if (name.empty() || name == "." || name == "..")
                continue;

            names->push_back(name);
        }
    }

    fsDirClose(&dir);

    std::sort(names->begin(), names->end());
    return true;
}

bool listDirectory(const std::string& sdmcPath, std::vector<std::string>* dirs, std::vector<std::string>* files)
{
    // 所有 SD 卡访问都必须经过同一把锁：
    //   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的**。
    //   下载工作线程在不停地写盘，而 UI 线程同时在写 settings.txt / log.txt，
    //   两个线程往同一个会话上发请求会导致响应错配 —— 真机表现就是卡死或写坏文件。
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    if (dirs != nullptr)
        dirs->clear();
    if (files != nullptr)
        files->clear();

    FsDir dir = {};
    // FsDirOpenMode 是位掩码：ReadDirs = BIT(0)、ReadFiles = BIT(1)。
    // 注意新版 libnx 里**没有** FsDirOpenMode_ReadAll 这个枚举值，
    // 想要「目录 + 文件都要」必须按位或，否则编译不过。
    const u32 mode = FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles;
    Result rc = fsFsOpenDirectory(&g_sd, toFsPath(sdmcPath).c_str(), mode, &dir);
    if (R_FAILED(rc))
        return false;

    FsDirectoryEntry entries[DIR_BATCH];

    s64 count = 0;
    while (true)
    {
        count = 0;
        rc    = fsDirRead(&dir, &count, DIR_BATCH, entries);
        if (R_FAILED(rc) || count == 0)
            break;

        for (s64 i = 0; i < count; i++)
        {
            std::string name = entries[i].name;
            if (name.empty() || name == "." || name == "..")
                continue;

            if (entries[i].type == FsDirEntryType_Dir)
            {
                if (dirs != nullptr)
                    dirs->push_back(name);
            }
            else
            {
                if (files != nullptr)
                    files->push_back(name);
            }
        }
    }

    fsDirClose(&dir);

    if (dirs != nullptr)
        std::sort(dirs->begin(), dirs->end());
    if (files != nullptr)
        std::sort(files->begin(), files->end());

    return true;
}

bool getFreeSpace(const std::string& sdmcPath, s64* out)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened || out == nullptr)
        return false;

    return R_SUCCEEDED(fsFsGetFreeSpace(&g_sd, toFsPath(sdmcPath).c_str(), out));
}

bool accessible(const std::string& devoptabPath)
{
    if (devoptabPath.empty())
        return false;

    return ::access(devoptabPath.c_str(), F_OK) == 0;
}

bool readWholeFile(const std::string& sdmcPath, std::string* out)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened || out == nullptr)
        return false;

    out->clear();

    FsFile file = {};
    if (R_FAILED(fsFsOpenFile(&g_sd, toFsPath(sdmcPath).c_str(), FsOpenMode_Read, &file)))
        return false;

    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&file, &size)) || size <= 0)
    {
        fsFileClose(&file);
        return true; // 空文件也算成功
    }

    const s64 limit = 64 * 1024;
    if (size > limit)
        size = limit;

    out->resize(static_cast<size_t>(size));

    u64 read = 0;
    Result rc = fsFileRead(&file, 0, &(*out)[0], static_cast<u64>(size), FsReadOption_None, &read);
    fsFileClose(&file);

    if (R_FAILED(rc))
    {
        out->clear();
        return false;
    }

    out->resize(static_cast<size_t>(read));
    return true;
}

bool readBinaryFile(const std::string& sdmcPath, std::string* out, size_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened || out == nullptr)
        return false;

    out->clear();

    FsFile file = {};
    if (R_FAILED(fsFsOpenFile(&g_sd, toFsPath(sdmcPath).c_str(), FsOpenMode_Read, &file)))
        return false;

    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&file, &size)) || size <= 0)
    {
        fsFileClose(&file);
        return false;
    }

    // 超过上限**直接失败**，绝不截断：截断后的 JPEG/PNG 会被解码成半边图像，
    // 比明确报错更难查。
    if (static_cast<size_t>(size) > maxBytes)
    {
        fsFileClose(&file);
        return false;
    }

    out->resize(static_cast<size_t>(size));

    u64 read = 0;
    Result rc = fsFileRead(&file, 0, &(*out)[0], static_cast<u64>(size), FsReadOption_None, &read);
    fsFileClose(&file);

    if (R_FAILED(rc) || read == 0)
    {
        out->clear();
        return false;
    }

    out->resize(static_cast<size_t>(read));
    return true;
}

s64 fileSize(const std::string& sdmcPath)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return -1;

    FsFile file = {};
    if (R_FAILED(fsFsOpenFile(&g_sd, toFsPath(sdmcPath).c_str(), FsOpenMode_Read, &file)))
        return -1;

    s64 size = 0;
    const Result rc = fsFileGetSize(&file, &size);
    fsFileClose(&file);

    return R_SUCCEEDED(rc) ? size : -1;
}

bool writeWholeFile(const std::string& sdmcPath, const std::string& data)
{
    std::lock_guard<std::recursive_mutex> lock(g_ioMutex);

    if (!g_opened)
        return false;

    const std::string path = toFsPath(sdmcPath);

    // 目标已存在时 fsFsCreateFile 会失败，先删掉重来
    if (R_FAILED(fsFsCreateFile(&g_sd, path.c_str(), 0, 0)))
    {
        fsFsDeleteFile(&g_sd, path.c_str());
        if (R_FAILED(fsFsCreateFile(&g_sd, path.c_str(), 0, 0)))
            return false;
    }

    FsFile file = {};
    // 同 createAndOpenFile：必须带 Append（= AllowAppend，允许写入扩大文件长度），
    // 否则往新建的空文件里写内容会被 FS 拒绝（0x00307202 / 6201）。
    if (R_FAILED(fsFsOpenFile(&g_sd, path.c_str(), FsOpenMode_Write | FsOpenMode_Append, &file)))
        return false;

    Result rc = fsFileWrite(&file, 0, data.data(), data.size(), FsWriteOption_Flush);

    fsFileFlush(&file);
    fsFileClose(&file);

    return R_SUCCEEDED(rc);
}

//============================== 路径工具 ==============================//

std::string normalize(const std::string& path)
{
    std::string s = path;

    // 统一分隔符
    for (char& c : s)
    {
        if (c == '\\')
            c = '/';
    }

    // 去掉设备前缀，本程序只操作 SD 卡
    const std::string device = "sdmc:";
    if (s.compare(0, device.size(), device) == 0)
        s = s.substr(device.size());

    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size())
    {
        size_t slash    = s.find('/', start);
        std::string seg = (slash == std::string::npos) ? s.substr(start) : s.substr(start, slash - start);

        if (seg == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!seg.empty() && seg != ".")
        {
            parts.push_back(seg);
        }

        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    std::string out = "sdmc:/";
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (i > 0)
            out += "/";
        out += parts[i];
    }

    return out;
}

std::string parent(const std::string& sdmcPath)
{
    std::string full = normalize(sdmcPath);
    if (full == "sdmc:/")
        return full;

    size_t slash = full.find_last_of('/');
    if (slash == std::string::npos || slash <= 5) // "sdmc:" 之后就是根
        return "sdmc:/";

    return full.substr(0, slash);
}

std::string join(const std::string& dir, const std::string& name)
{
    if (name.empty())
        return normalize(dir);

    if (!name.empty() && name[0] == '/')
        return normalize(name);

    std::string base = normalize(dir);
    if (base.back() != '/')
        base += "/";

    return normalize(base + name);
}

std::string toFsPath(const std::string& sdmcPath)
{
    std::string full = normalize(sdmcPath);

    const std::string device = "sdmc:";
    if (full.compare(0, device.size(), device) == 0)
        full = full.substr(device.size());

    if (full.empty())
        full = "/";

    if (full[0] != '/')
        full = "/" + full;

    return full;
}

std::string sanitizeFileName(const std::string& raw)
{
    // 只取最后一段，防止 "a/../../evil" 之类的路径穿越
    std::string name = raw;
    size_t pos       = name.find_last_of("/\\");
    if (pos != std::string::npos)
        name = name.substr(pos + 1);

    // 简单的 URL 解码（%20 之类）
    std::string decoded;
    decoded.reserve(name.size());
    for (size_t i = 0; i < name.size(); i++)
    {
        if (name[i] == '%' && i + 2 < name.size() && isDigitHex(name[i + 1]) && isDigitHex(name[i + 2]))
        {
            decoded.push_back(static_cast<char>(std::strtol(name.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        }
        else
        {
            decoded.push_back(name[i]);
        }
    }

    // 过滤控制字符与文件系统不允许的字符
    std::string out;
    out.reserve(decoded.size());
    for (char c : decoded)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f)
            continue;
        if (std::strchr("<>:\"|?*", c) != nullptr)
            continue;
        out.push_back(c);
    }

    // 去掉首尾空格，以及结尾的点（FAT/exFAT 上很麻烦）
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();

    size_t begin = out.find_first_not_of(' ');
    if (begin == std::string::npos)
        return "";
    out = out.substr(begin);

    if (out.empty() || out == "." || out == "..")
        return "";

    // 文件名长度上限（字节）
    if (out.size() > 200)
        out.resize(200);

    return out;
}

std::string fileNameFromUrl(const std::string& url)
{
    std::string u = trim(url);

    // 去掉 query 与 fragment
    size_t cut = u.find_first_of("?#");
    if (cut != std::string::npos)
        u = u.substr(0, cut);

    // 去掉协议
    size_t scheme = u.find("://");
    if (scheme != std::string::npos)
        u = u.substr(scheme + 3);

    // 去掉主机名，只保留路径部分
    size_t slash = u.find('/');
    std::string path = (slash == std::string::npos) ? "" : u.substr(slash);

    return sanitizeFileName(path);
}

std::string formatBytes(s64 bytes)
{
    char buf[64];

    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;

    double v = static_cast<double>(bytes);

    if (v >= gb)
        std::snprintf(buf, sizeof(buf), "%.2f GB", v / gb);
    else if (v >= mb)
        std::snprintf(buf, sizeof(buf), "%.1f MB", v / mb);
    else if (v >= kb)
        std::snprintf(buf, sizeof(buf), "%.0f KB", v / kb);
    else
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));

    return std::string(buf);
}

} // namespace fsx
