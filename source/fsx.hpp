/*
    NX Downloader - SD 卡文件系统辅助模块

    统一用 libnx 的 FsFileSystem 接口操作 SD 卡（比 devoptab 的 stdio 更可控、
    也不会因为 hbmenu 已经挂载过 sdmc 而失败）。
*/
#pragma once

#include <switch.h>

#include <string>
#include <vector>

namespace fsx
{

//============================== 生命周期 ==============================//
/// 打开 SD 卡文件系统，必须在其它接口之前调用
bool init();

/// 关闭 SD 卡文件系统
void exit();

//============================== 查询 / 操作 ==============================//
/// 路径是否已存在（文件或目录）
bool exists(const std::string& sdmcPath);

/// 是否为目录
bool isDirectory(const std::string& sdmcPath);

/// 删除文件（文件不存在也返回 true）
bool removeFile(const std::string& sdmcPath);

/// 创建目录（最后一级，父目录必须已存在）
bool createDirectory(const std::string& sdmcPath);

/// 递归创建目录，已存在则直接返回 true
bool ensureDirectory(const std::string& sdmcPath);

/// 创建并打开文件用于写入。
///
/// preallocSize：期望的文件总长度（能预先知道就给），>0 时会先把文件长度
/// 定到这个值，后续写入都在长度之内 —— 这样就不会走「隐式扩大文件」那条路。
/// ★ 内部固定用 `FsOpenMode_Write | FsOpenMode_Append`：
///   Nintendo 的 FS 把 Append 位解释为 **AllowAppend**（允许 WriteFile 隐式扩大文件），
///   缺了它会返回 0x00307202（6201）。
bool createAndOpenFile(const std::string& sdmcPath, FsFile* out, s64 preallocSize = 0);

//----------------------------------------------------------------------
// ⚠️ 为什么写入要单独走这两个接口：
//   libnx 的 FsFileSystem 是一个 IPC 会话，**不是并发安全的** ——
//   下载工作线程在不停地 fsFileWrite，而 UI 线程可能同时在写 settings.txt /
//   log.txt。两个线程往同一个会话上发请求会导致响应错配
//   （表现就是卡死、或者写出错乱的文件）。
//   所以所有 SD 卡 I/O 都必须经过 fsx 内部的互斥锁串行化，
//   不要绕过 fsx 直接调 fsFileWrite。
//----------------------------------------------------------------------

/// 在已打开的文件上写一段数据（内部串行化）。返回 libnx Result，0 = 成功。
Result writeFileChunk(FsFile* file, s64 offset, const void* data, u64 size);

/// 冲刷并关闭文件（同样串行化）
void flushAndCloseFile(FsFile* file);

/// 列出某个目录下的子目录名（不含文件），按名称排序
bool listSubDirectories(const std::string& sdmcPath, std::vector<std::string>* names);

/// 列出目录内容：子目录名与文件名分别收集，各自按名称排序
/// dirs / files 任一为 nullptr 表示不关心该项
bool listDirectory(const std::string& sdmcPath, std::vector<std::string>* dirs, std::vector<std::string>* files);

/// 查询剩余空间（字节）
bool getFreeSpace(const std::string& sdmcPath, s64* out);

/// 读取整个文本文件（超过 64KB 只读前 64KB）
bool readWholeFile(const std::string& sdmcPath, std::string* out);

/// 读取整个**二进制**文件（图片用）。
///
/// 与 readWholeFile 的区别：不截断。文件大于 maxBytes 时直接返回 false
/// （截断后的图片会被解码成半张图，比报错更难排查）。
/// maxBytes 默认 16MB —— 常见截图/照片够用，也避免把显存撑爆。
bool readBinaryFile(const std::string& sdmcPath, std::string* out, size_t maxBytes = 16u * 1024u * 1024u);

/// 文件大小（字节）；文件不存在或读取失败返回 -1
s64 fileSize(const std::string& sdmcPath);

/// 覆盖写入文本文件
bool writeWholeFile(const std::string& sdmcPath, const std::string& data);

/// 用 stdio 判断任意 devoptab 路径是否可访问（可用于 romfs:/ 之类的路径）
bool accessible(const std::string& devoptabPath);

//============================== 路径工具 ==============================//
/// 规范化成 "sdmc:/a/b" 形式：统一分隔符、去掉 "." 与 ".."、去掉重复斜杠
std::string normalize(const std::string& path);

/// 取父目录，已在根目录则返回自身
std::string parent(const std::string& sdmcPath);

/// 拼接目录与名称
std::string join(const std::string& dir, const std::string& name);

/// 转成 FsFileSystem 需要的路径："sdmc:/a/b" -> "/a/b"
std::string toFsPath(const std::string& sdmcPath);

/// 把任意字符串清洗成安全的文件名（防目录穿越），失败返回空串
std::string sanitizeFileName(const std::string& raw);

/// 从 URL 中推断文件名（去掉 query / fragment）
std::string fileNameFromUrl(const std::string& url);

/// 人类可读的字节数
std::string formatBytes(s64 bytes);

} // namespace fsx
