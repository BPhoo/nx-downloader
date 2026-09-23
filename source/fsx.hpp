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

/// 创建并打开文件用于写入
bool createAndOpenFile(const std::string& sdmcPath, FsFile* out);

/// 列出某个目录下的子目录名（不含文件），按名称排序
bool listSubDirectories(const std::string& sdmcPath, std::vector<std::string>* names);

/// 查询剩余空间（字节）
bool getFreeSpace(const std::string& sdmcPath, s64* out);

/// 读取整个文本文件（超过 64KB 只读前 64KB）
bool readWholeFile(const std::string& sdmcPath, std::string* out);

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
