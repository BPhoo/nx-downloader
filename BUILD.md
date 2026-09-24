# NX Downloader（v2.2.0）

Nintendo Switch 自制程序（`.nro`）：上行检查更新、下行下载文件。

```
更新链接  [ 输入框 ]  [txt图标]              [ 访问 ]
下载链接  [ 输入框 ]  [txt图标] [dir图标]    [ 访问 ]
状态：就绪
[ 进度条 ]  [ 取消当前任务 ]
详情 / 返回内容：……
下载保存到：sdmc:/（第二行的文件夹图标可更换）
提示 · 环境 · 日志路径
```

---

## 1. 这个版本修了什么（v2.2.0）

v2.1.1 的真机日志显示：**网络其实已经成功**（`curl_easy_perform = 0`、HTTP 200、取回 1014 字节），
日志停在「网络任务成功：」之后就没了 —— 说明**卡死在 UI 侧收尾的那几行代码里**，和 curl / 网络无关。
顺着这个线索把收尾路径整段重写，同时修掉几处真实缺陷：

| # | 问题 | 说明与修法 |
| --- | --- | --- |
| 1 | **收尾路径是「在动画回调里改视图栈」** | v2.1.x 的写法是 `RepeatingTask → Dialog::close(cb) → menu_animation 回调 → Application::pushView(结果页)`。borealis 的 `View::show/hide` 每次都 `menu_animation_kill_by_tag()`，而 `Dialog::close(cb)` 本身就是「动画播完再回调」，两者嵌套（push/pop + kill 同一个 tag）非常脆。<br>**v2.2.0 把这条路径整个删掉**：不再用 Dialog、不再用通知，任务收尾只改 Label 文本，**绝不改动视图栈**。唯一保留 `pushView` 的地方是「点图标打开文件/目录选择器」这类按键触发的常规路径。 |
| 2 | **工作线程「先置终态、后写日志」** | 原顺序是先 `state.store(Finished)` 再 `logx::linef(...)`，UI 可能在 worker 还在写盘时就开始处理结果。<br>现在**终态一律最后一步置位**：UI 一旦看到终态，就说明该做的都做完了（含日志落盘）。 |
| 3 | **部分 SD 卡访问没有走锁** | `removeFile`（失败清理时由工作线程调用）与 `getFreeSpace`（下载写入回调里调用）当时是裸调用。libnx 的 `FsFileSystem` 是 IPC 会话、**不是并发安全的**，与会写日志/设置的 UI 线程并发使用会导致响应错配。<br>现在 `fsx` 里所有公开接口统一过同一把 `recursive_mutex`。 |
| 4 | **轮询任务退化成每帧都跑** | 自定义的 `RepeatingTask::run()` 没有调用基类实现，而基类那句 `lastRun = currentTime` 是唯一的节流依据 → `lastRun` 恒为 0，任务变成 60 次/秒而不是每 100ms。<br>现在显式调用 `RepeatingTask::run(currentTime)`。 |
| 5 | **日志满 24KB 后静默停止** | 出问题时最有价值的是最后几行，静默停止反而让诊断失效。现在改成**丢掉前半段、保留最近的**。 |
| 6 | **诊断不够细** | 新增 UI 侧面包屑日志（`[UI]` 前缀）：`任务收尾开始 → 读到返回内容 N 字节 → 状态行已更新 → 详情区已更新 → 收尾完成`，以及任务进行期间每秒一行的「轮询心跳」。**万一还卡，日志的最后一行就是卡住的那个调用。** |

另外：检查更新的返回内容会**完整存到 `update_result.txt`**（页面只显示前 2000 字节），
即使界面出问题，内容也不会丢。

> 诚实说明：本轮**没有**找到能 100% 复现「冻死」的单点，但收尾路径上唯一不寻常的写法（在动画回调里改视图栈）
> 已经整段移除，同时补上了能精确定位的日志。如果仍复现，请把 `log.txt` 发回：最后一行的 `[UI]` 面包屑会直接指出卡在哪个调用。

---

## 2. 目录结构

```
nx-downloader/
├── Makefile                      devkitPro/libnx 标准模板（已接好 borealis 与 switch-curl）
├── icon.jpg                      主图标
├── BUILD.md                      本文件
├── BUILD-v2.1.1.md               上一版说明（存档）
├── .github/workflows/build-nro.yml    在 GitHub 服务器云编译，产出 nx-downloader.nro
├── romfs/
│   ├── i18n/en-US/brls.json      borealis 文案（底部按键提示等）
│   ├── icon/txt.png              「选 txt」图标（128×128）
│   ├── icon/folder.png           「选保存目录」图标（128×128）
│   └── material/MaterialIcons-Regular.ttf
└── source/
    ├── main.cpp                  启动顺序：romfs → SD 卡 → 网络 → curl → 项目文件夹 → UI
    ├── app_config.{hpp,cpp}      项目文件夹 / url.txt 的生成与宽松解析
    ├── fsx.{hpp,cpp}             libnx FsFileSystem 封装（**所有公开接口统一串行化**）
    ├── logx.{hpp,cpp}            SD 卡日志（多线程安全、带 [UI] 标记、满了保留最近部分）
    ├── netx.{hpp,cpp}            nifm + socket 初始化（AlreadyInitialized 按成功处理）
    ├── downloader.{hpp,cpp}      libcurl 下载/取文本；pthread 2MiB 栈；终态最后置位
    ├── main_view.{hpp,cpp}       主界面（本版本重写的核心）
    ├── file_picker.{hpp,cpp}     选任意 .txt
    └── path_picker.{hpp,cpp}     选保存目录
```

> `source/progress_dialog.*` 在 v2.2.0 已废弃（进度改为页内显示）。
> 仓库里可能还留着这两个文件，因为当前的上传通道（GitHub 网页 `/upload`）只能新增/覆盖、不能删除文件；
> 流水线里有一步 `rm -f source/progress_dialog.{cpp,hpp}` 保证它们不参与编译。

---

## 3. SD 卡上的文件

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro              程序本体
├── url.txt                        首次启动自动生成，格式见下
├── settings.txt                   界面里改过的链接与保存目录（自动写回）
├── log.txt                        运行日志（每次启动重建；真机排错就看它）
├── update_result.txt              上次检查更新拿到的完整返回内容
└── cacert.pem                     可选：放了它就严格校验 HTTPS 证书
```

`url.txt` 格式（解析很宽松：`updata`/`update` 都认、`:` 与 `=` 都认、可加引号、支持注释、省略协议会自动补 `https://`）：

```
{ updata: https://example.com/version.txt , download: https://example.com/app.zip }
```

- 第一行「访问」用 `updata`（检查更新，显示返回内容）
- 第二行「访问」用 `download`（下载文件）
- 按 **X** 键：焦点在哪一行，就读该行对应的值

---

## 4. 安装与操作

1. 把 `nx-downloader.nro` 放到 `SD:/switch/nx-downloader/`。
2. 从**游戏图标**启动（按住 <kbd>R</kbd> 打开任意游戏进入 hbmenu）—— 这是完整内存模式。
   - 从「相册」启动也能用，但属于 Applet 模式：**程序会禁用系统键盘**（真机实测按 A 调键盘有死机风险），
     此时请用 `X` 或文件图标读 `url.txt`。
3. 首次启动自动创建项目文件夹与 `url.txt`（模板里是示例链接，用 `X` 读完再改自己的即可）。
4. 操作：
   - **A**：打开键盘手动输入链接（最长 100 字符，无下限）
   - **X**：直接读 `url.txt` 里对应的值
   - **txt 图标**：打开文件浏览器，选任意 `.txt`，自动取出里面的 `updata` / `download`
   - **dir 图标**（仅第二行）：选下载保存目录，默认 `sdmc:/`
   - **十字键 / 左摇杆**：移动焦点；**+**：退出

**同名文件会被直接覆盖**（写入侧是「先删再建」），界面上不再弹确认框 —— 这是本轮去掉弹窗的一部分。

---

## 5. 从源码编译

### 5.1 云端编译（推荐，本机无需装工具链）

推送代码到 GitHub 即触发 `.github/workflows/build-nro.yml`：

- 先尝试用 devkitPro 官方脚本装工具链；若 `apt.devkitpro.org` 被 Cloudflare 拦（部分地区常见），
  自动退回官方镜像 `devkitpro/devkita64` 编译。
- 编译用 `make -k`，并把所有 `error:` 行写进 Step Summary —— 一次 CI 就能看到全部编译错误。
- 产物在 Actions 运行页的 Artifacts 里（`nx-downloader-nro`）。
  **注意：即使是公开仓库，下载 artifact 也必须登录 GitHub。**

### 5.2 本地编译

```bash
# 依赖：devkitPro（switch-dev + switch-curl）
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$PATH

git clone --depth=1 --recursive -b master https://github.com/XITRIX/borealis.git borealis
make sync-resources      # 把 borealis 的 i18n / material 资源同步进 romfs/
make -j$(nproc)          # 产出 nx-downloader.nro
```

关键点（都踩过坑）：

- **borealis 必须用 `-b master`**：默认分支是 `moonlight_wiliwili`，结构不同、找不到 `library/borealis.mk`。
- **链接参数用 `curl-config --libs`**：`LIBS := $(CURL_LIBS) -lnx -lm`，这样 mbedTLS / zlib 等传递依赖不会漏。
- **必须保留 RTTI 与异常**：不要加 `-fno-rtti` / `-fno-exceptions`（borealis 需要）。
- **`.DEFAULT_GOAL := all`**：Makefile 里 `sync-resources` 写在 `all` 之前，不写这行 `make` 会 1 秒结束且不出包。
- **CI 里不要再升级工具链**：镜像自带 libnx 4.12.0（≥4.10.0，满足固件 21.x 的 TLS ABI 要求），
  升级会把 gcc 从 15 换到 16，平白引入变量。

---

## 6. 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| 界面卡死 | v2.2.0 已移除收尾路径上的「动画回调里改视图栈」。请把 `sdmc:/switch/nx-downloader/log.txt` 发回：日志最后一行的 `[UI]` 面包屑会指出卡在哪个调用（例如停在「任务收尾开始」就说明卡在「读正文 / 改状态行」之间）。 |
| 启动就提示「网络不可用」 | v2.1.1 起 `LibnxError_AlreadyInitialized`（`0x0F59`）按**成功**处理（它恰恰说明 socket 已经可用）。其它错误码按下面第 6.1 节对照。 |
| 输入框只能输 32 个字符 | 已修：v2.0.0 的 CI 补丁把 `maxStringLength` 覆盖成了 32，现改成整行删除并加了断言。 |
| 按 A 输入就死机 | Applet 模式（相册启动）下调系统键盘有风险。v2.1.0 起在 Applet 模式下**不再调用键盘**，请用 `X` / 文件图标，或按住 R 从游戏图标启动。 |
| 中文显示成方块 | 程序会挂载系统简体中文字体作为 fallback（日志里记录 `plGetSharedFontByType` 的返回值）；主机缺该字体则无解。 |
| HTTPS 报证书错误 | 设备没有 CA 链时程序会自动降级为「不校验证书」，并在完成提示里注明；要严格校验就把 `cacert.pem` 放到项目文件夹。 |
| HTTP 4xx/5xx 也报「下载失败」 | 这是刻意的：避免把服务器的错误页面当成文件存下来。 |
| 下载很慢 / 进度条不动 | 网络太慢或服务器无响应，点「取消当前任务」；超时阈值是 15s 连接、60s 低速。 |
| 日志里出现「前文过长已丢弃」 | 正常：日志满 24KB 后只保留最近部分（出问题时最后几行最有价值）。 |

### 6.1 libnx 错误码

`Result` 的编码是 `(module << 9) | description`，可用 `log.txt` 里的 `0x……` 反查：

- `0x00000F59` = `module 345 (Module_Libnx) | 描述 7 (LibnxError_AlreadyInitialized)`
  → **不是失败**：`soc:` 设备只在 `bsdInitialize` 成功之后才登记，
  能拿到这个码恰恰说明 socket 早就初始化好、而且可用。
- 其它错误码对照 `libnx/nx/include/switch/result.h`。

---

## 7. 版本历史

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 单行下载器：一个链接输入框 + 保存目录 + 下载按钮 |
| 2.0.0 | 双行结构：上行检查更新、下行下载；启动自动建项目文件夹与 `url.txt`；`X` 快捷读取；两行各有 txt 图标；左摇杆可导航 |
| 2.1.0 | 修输入框上限被 CI 补丁覆盖成 32；补 `nifmInitialize`；Applet 模式禁用系统键盘；新增 `log.txt`；SD 卡 I/O 串行化；去掉每帧文字测量 |
| 2.1.1 | `AlreadyInitialized` 按成功处理（网络不再被误禁用）；去掉未导出的 `bsdSocket` 符号；CI 失败时也保证生成 Step Summary |
| **2.2.0** | **移除收尾路径上的全部动画/视图栈操作（对话框、通知、嵌套 pushView）→ 进度与结果改为页内显示**；工作线程终态最后置位；`fsx` 全部接口统一加锁；修轮询任务每帧都跑；日志满后保留最近部分；新增 UI 面包屑与轮询心跳日志；返回内容另存 `update_result.txt` |
