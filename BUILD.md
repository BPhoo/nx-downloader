# NX Downloader（v2.6.0）

Nintendo Switch 自制程序（`.nro`）。两种界面：

| 启动方式 | 界面 | 说明 |
| --- | --- | --- |
| **按住 R → 从游戏图标启动**（完整内存模式） | 图形界面（borealis） | 全部功能：双行链接、文件/目录选择器、进度、`img:` 图片 |
| **从相册启动**（Applet 模式） | **文字界面（libnx 控制台）** | 检查更新 / 下载（带百分比）/ 重读 `url.txt` / SD 卡 I/O 自检 |

> 为什么分两种：真机上 Applet 模式会在**构造图形界面**的过程中整机死机
> （冻结点落在纯 CPU 的视图树构造里、紧跟一次 SD 写盘之后，见 §2/§3）。
> Applet 模式本身又天生受限（内存小、无系统键盘、nifm 查询受限、SD 卡与系统共用），
> 所以那条路径直接改用控制台 —— 不再赌图形界面。

---

## 1. v2.6.0（本轮）：Applet 模式改用文字界面 + 关键定位

上一份真机日志把冻结点夹到了**一个函数里的三条语句**之间：

```
[023] [UI] 状态区：状态 Label
[024] [UI] 状态区：进度条      ← 之后什么都没有
```

对应代码：

```cpp
logx::ui("状态区：进度条");
this->bar = new ProgressDisplay(ProgressDisplayFlags::PERCENTAGE);   // 内部 new Label("0%")
this->bar->setHeight(60);
this->addView(this->bar);
logx::ui("状态区：取消按钮");        // ← 这一行没写出来
```

### 1.1 先说结论：**不是「图形渲染」的问题**

把证据摆齐：

- **GL 上下文、窗口、framebuffer、字体表**全部在 `Application::init()` 里创建 —— 它**成功返回**了（日志有 `Application::init = OK`）；
- 7.8 MB 系统中文共享字体也加载成功（日志有）；
- 冻结点所在的这段代码**没有任何 GL 调用、没有文字测量、没有文件访问** —— 只是 `new Label(...)` / `setHeight` / `addView`，
  纯 CPU 的视图树搭建；
- 它是**紧跟在一行日志的写盘之后**停住的。而日志是每一步都写的，所以「最后一行」实际上是在说
  **「冻结发生在写下一行日志的时候」**。

⇒ 所以「图形界面画错了导致死机」这个方向**不成立**；但**「borealis 这条 UI 栈在 Applet 模式下调不起来」这个方向仍然成立**，
因为它牵扯到内存配额、GL/显示初始化、以及 SD 卡与系统的争用，而这些东西在 Applet 模式下都不一样。

### 1.2 因此本轮做的事：给 Applet 模式一条**不依赖图形栈**的路

新增 `source/console_mode.{hpp,cpp}`：纯 libnx 控制台界面（`consoleInit` + 手柄按键），
**不创建 GL 上下文、不用 nanovg / borealis / 字体**。功能：

```
[A] 检查更新     拉 updata，把返回内容前 12 行直接打在屏幕上
[B] 下载文件     下载 download 到项目文件夹，屏幕上打百分比与已收字节
[X] 重新读 url.txt   手改完 url.txt 不用退出程序
[Y] SD 卡 I/O 自检   照日志的写法连续写 40 次（64 字节 + flush），每次打印耗时
[+] 退出
```

- 屏上文案**一律 ASCII**：libnx 的 console 用内置位图字体，没有汉字字形（中文会是方块）。
  日志文件里仍然是中文。
- `main()` 里的分支：`appletGetAppletType() != AppletType_Application` → 文字模式。
- 两个开关文件放在 `sdmc:/switch/nx-downloader/` 下，真机上不用重编就能来回验证：
  - `console.txt` 存在 → 强制文字模式（完整内存模式下也能看文字界面长什么样）；
  - `gui.txt` 存在 → 强制图形界面（Applet 模式下再试一次图形界面）。

### 1.3 这同时是**决定性实验**

- `[Y] SD 卡 I/O 自检` 把「是不是 SD 卡 I/O 把整机拖死」单独摘出来测：
  如果它在第 N 次写入时卡住，屏幕会**保留最后一帧**，用户直接就能报出 `write N/40`；
- 如果整个文字模式在 Applet 模式下用得挺好 → 问题在 borealis/GL 那条栈上，文字模式就是 Applet 模式的正式方案；
- 如果文字模式也卡死 → 问题在 Applet 环境本身（与图形无关），那时按 SD 卡争用方向继续查。

### 1.4 v2.5.0 里那些「少碰 SD 卡」的改动仍然保留

见 §2：日志改成「打开一次 + 逐行追加」（每行 6 次 FS 操作 → 1 次）、
`url.txt` / `settings.txt` 的读取提前到 UI 之前（构造函数彻底不碰 SD 卡）、
去掉多余的 `fsdevMountSdmc()`、内容没变不写 `settings.txt`。

---

## 2. v2.5.0 修过什么

### 2.1 把启动阶段的 SD 卡 I/O 大幅减少

Applet 模式下 SD 卡与父 applet / 系统**共用**，而原来我们对它相当粗暴：

| 原行为 | 每次 FS 调用数 | 启动阶段总量 |
| --- | --- | --- |
| **每写一行日志**都「删文件 → 建文件 → 打开 → 写 → flush → 关」 | 6 次 | 二十多行 → **上百次** |
| 构造函数里读 `settings.txt`、读 `url.txt`、回写 `settings.txt` | 数次 | 又一轮 |
| 额外挂一个 `fsdevMountSdmc()` | 1 次 + 一个额外会话 | — |

改成：日志「打开一次、逐行追加」（每行 1 次 FS 操作）；`url.txt` / `settings.txt` 的读取搬到 `main.cpp`（UI 之前）；
去掉 `fsdevMountSdmc()`；`settings.txt` 内容未变不写。

### 2.2 判读粒度做到按控件

`buildStatusArea()` 逐控件打面包屑，并记录 **applet 焦点状态**（前 6 次轮询）：

```
[UI] 轮询第 1 次：主循环正常，appletType=2(Applet（相册启动）)，焦点=InFocus(1)，屏幕=掌机
```

Applet 失焦（`OutOfFocus` / `Background`）时系统会接管 SD 卡与显示，
**此时做 SD 读写最容易互相干扰** —— 这是「整机死机」这类问题的关键判据。

---

## 3. v2.4.0 修过什么

### 3.1 图片「已加载却全白」：`brls::Image` 的两处硬伤

**① 它只吃「文件路径」，内部是 `fopen`。**

```c
this->texture = nvgCreateImage(vg, this->imagePath.c_str(), 0);
// nanovg.c：fopen 失败就返回 0，**不报错**
img = stbi_load(filename, &w, &h, &n, 4);
if (img == NULL) return 0;
```

本程序访问 SD 卡一直走**裸 `FsFileSystem`**，`sdmc:` 从未挂到 devoptab 上 →
`fopen("sdmc:/…")` 必然失败 → 纹理 ID 是 `0`。这就是「计数在涨、屏幕全白」。

**② 它的 `layout()` 会缓存「首次布局时的高度」。** `Application::pushView()` 里有 `view->invalidate(true)`
（同步强制布局整棵树）——那一刻图片还折叠着，**0 被永久缓存**，再用 0×0 的纹理尺寸算出 `0/0 = NaN`
灌进 `setHeight` / `nvgImagePattern`（`(int)NaN` 是 UB）。

**修法**：自绘 `RemoteImageView` —— `fsx::readBinaryFile`（二进制整读，超限直接失败不截断）→
`nvgCreateImageMem` 解码 → `nvgImageSize` 验尺寸；失败给出明确原因。尺寸只用整数 + 有限性检查，不可能产生 NaN。
**显存预算**：单文件 ≤12MB、单张 ≤4M 像素、总量 完整内存 16M / Applet 6M 像素。

### 3.2 构造函数不要碰 GPU / 不要有「只在某种模式下才建」的东西

图标贴图改到主循环第一轮加载；Applet 专属提示移出构造函数（两种模式视图树一致）；
构造函数逐步面包屑；`logx` 写盘失败时在界面上显示「⚠ 日志写入失败」。

---

## 4. v2.3.0 修过什么

### 4.1 `写入 SD 卡失败（0x00307202）`

```
0x307202  6201  Error: OpenMode_AllowAppend is required for implicit extension
                of file size by WriteFile().
```

`FsOpenMode_Append` 是 **`OpenMode_AllowAppend`（允许写入时扩大文件长度）**，不是「只能追加」。
libnx 自己的 `fsdev` 就是 `FsOpenMode_Write | FsOpenMode_Append`。
修复：带上该位；能从 `Content-Length` 得知总长度（≤256MB）时先预分配。

### 4.2 `img:` —— 启动自动加载图片

```
{ updata: … , download: … , img: https://a.com/1.jpg, https://a.com/2.jpg }
```

每项可以是 **http(s) 链接**（自动下载到 `img/`），也可以是**已放在 SD 卡上的文件名**
（按「原文 → 项目文件夹 → SD 根目录」找）。缓存名 `<序号>-<原名>`，改顺序/换图不会读到旧缓存。

### 4.3 「可以选中的横线」与「底部看不全」

- 那条线是**取消按钮**：可聚焦只看 `getDefaultFocus()` 是否非空，而 `Button` 永远返回自身；
  且它在 `List` 里没人给高度（0 高 = 一条线）。→ 子类化，只在任务进行中可聚焦，显式给行高。
- 看不全是因为 **borealis 的滚动只跟随焦点** → 底部放一个可聚焦的锚点按钮。

---

## 5. v2.2.0 摘要

网络早已成功（`curl_easy_perform = 0`、HTTP 200），卡死点在 UI 收尾：
`RepeatingTask → Dialog::close(cb) → menu_animation 回调 → Application::pushView`，即**在动画回调里改视图栈**。
v2.2.0 把这条路径整段删掉（不用 Dialog、不发通知、收尾只改文本），worker 终态最后置位，
`fsx` 全部公开接口加锁，修掉轮询任务「每帧都跑」。

> **两条设计铁律**：
> 1. 工作线程运行期间/收尾时**绝不改动视图栈**（唯一例外：按键触发的 `pushView`）；
> 2. 页面最底部必须留一个**可聚焦**的控件，否则永远滚不到底。

---

## 6. 目录结构

```
nx-downloader/
├── Makefile                      devkitPro/libnx 标准模板（已接好 borealis 与 switch-curl）
├── icon.jpg                      主图标
├── BUILD.md                      本文件
├── BUILD-v2.2.0.md / v2.1.1.md   历史版本说明（存档）
├── .github/workflows/build-nro.yml    云端编译，产出 nx-downloader.nro
├── romfs/                        图标 / borealis 文案 / material 图标字体
└── source/
    ├── main.cpp                  启动顺序：romfs → SD 卡 → 日志 → 网络 → curl → 项目文件夹
    │                              → 读 url.txt/settings.txt → **界面分支**（文字 / 图形）
    ├── console_mode.{hpp,cpp}    ★ 文字（控制台）界面：Applet 模式用，不碰 GL/borealis
    ├── app_config.{hpp,cpp}      项目文件夹 / url.txt 的生成与宽松解析（含 img:）
    ├── fsx.{hpp,cpp}             libnx FsFileSystem 封装（串行化、Append 位、预分配、
    │                              追加写 openForAppend、二进制整读 readBinaryFile、fileSize）
    ├── logx.{hpp,cpp}            SD 卡日志（打开一次逐行追加、[UI] 标记、过长轮转、写失败可上报）
    ├── netx.{hpp,cpp}            nifm + socket 初始化；applet 焦点/模式描述
    ├── downloader.{hpp,cpp}      libcurl 下载/取文本；pthread 2MiB 栈；终态最后置位
    ├── main_view.{hpp,cpp}       图形界面 + RemoteImageView + 诊断信息页
    ├── file_picker.{hpp,cpp}     选任意 .txt
    └── path_picker.{hpp,cpp}     选保存目录
```

---

## 7. SD 卡上的文件

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro              程序本体
├── url.txt                        首次启动自动生成（updata / download / img）
├── settings.txt                   图形界面里改过的链接与保存目录（内容没变不写）
├── log.txt                        运行日志（每次启动重建；真机排错就看它）
├── update_result.txt              上次检查更新拿到的完整返回内容
├── img/                           图片缓存（1-xxx.jpg、2-yyy.jpg …）
├── cacert.pem                     可选：放了它就严格校验 HTTPS 证书
├── console.txt                    ★ 可选：存在则强制文字界面
└── gui.txt                        ★ 可选：存在则强制图形界面
```

---

## 8. 安装与操作

1. 把 `nx-downloader.nro` 放到 `SD:/switch/nx-downloader/`。
2. **图形界面**：按住 <kbd>R</kbd> 从游戏图标启动（完整内存模式）。操作：
   **A** 输入链接（≤100 字符）· **X** 读 `url.txt` 对应值 · **txt 图标** 选 `.txt` ·
   **dir 图标** 选保存目录（默认 `sdmc:/`）· **十字键/左摇杆** 移动 · **诊断信息与提示** 看状态 · **+** 退出。
3. **文字界面**：从相册启动即自动进入。操作：
   **A** 检查更新 · **B** 下载 · **X** 重读 `url.txt` · **Y** SD 卡 I/O 自检 · **+** 退出。

---

## 9. 从源码编译

### 9.1 云端编译（推荐）

推送代码即触发 `.github/workflows/build-nro.yml`：先试 devkitPro 官方脚本装工具链，
被 Cloudflare 拦时退回官方镜像 `devkitpro/devkita64`；摘要里打印 libnx / switch-curl 版本；
`make -k` + 把所有 `error:` 行写进 Step Summary（一次 CI 看到全部编译错误）。
产物在运行页 Artifacts 里（**公开仓库下载 artifact 也必须登录 GitHub**）。

### 9.2 本地编译

```bash
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$PATH
git clone --depth=1 --recursive -b master https://github.com/XITRIX/borealis.git borealis
make sync-resources
make -j$(nproc)
```

关键点：borealis 必须 `-b master`；链接参数用 `curl-config --libs`；保留 RTTI/异常；
`.DEFAULT_GOAL := all`；别在 CI 里升级工具链（镜像自带 libnx 4.12.0 已满足固件 21.x 的 TLS ABI 要求）。

---

## 10. 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| **Applet 模式（相册启动）一启动就死** | v2.6.0 起这条路径**不再加载图形界面**，直接进文字模式（§1）。若文字模式也卡死，请按屏幕最后一行（例如 `write 23/40`）判断是不是 SD 卡 I/O；把 `log.txt` 发回。**图形界面请按住 R 从游戏图标启动。** |
| 想强制用某种界面 | 在项目文件夹里放 `console.txt`（强制文字）或 `gui.txt`（强制图形），不用重编。 |
| 图片「N/N 已加载」但画不出来 | 已修（§3.1）：自绘 + 内存解码。诊断页会写出每一格的失败原因。 |
| 图片加载失败 | 诊断页/日志给出原因：`文件太大`（>12MB）、`分辨率太高`（>4M 像素）、`不是能解码的图片`、`读不到文件`、`显存预算已用完`。 |
| **写入 SD 卡失败 (0x00307202)** | 已修（§4.1）：FS 6201 = 需要 `OpenMode_AllowAppend`。 |
| 日志很短、像「死在某一步」 | 先看界面有没有「⚠ 日志写入失败」（§3.2）；v2.5.0 起日志改为追加写，风险大幅降低。 |
| 启动提示「网络不可用」 | `0x00000F59` = `LibnxError_AlreadyInitialized`，按**成功**处理（§11）。 |
| 界面卡死 | 看 `log.txt` 最后一行 `[UI]` 面包屑；心跳停止＝渲染循环不动了。 |
| 底部内容看不全 | 已修（§4.3）：底部加了可聚焦锚点。 |
| 输入框只能输 32 个字符 | 已修（v2.1.0）。 |
| 按 A 输入就死机（图形界面） | Applet 模式下调系统键盘有风险；文字模式没有键盘输入。 |
| 中文显示成方块（文字模式） | 预期行为：libnx console 没有汉字字形，文字模式刻意只用 ASCII。 |
| HTTPS 报证书错误 | 设备没有 CA 链时自动降级为「不校验证书」并在提示里注明。 |
| HTTP 4xx/5xx 报「下载失败」 | 刻意的：避免把服务器错误页面当成文件存下来。 |

### 10.1 怎么读日志

`log.txt` 每次启动重建；`[UI]` 前缀 = UI 线程，无前缀 = 下载工作线程。图形界面的启动流程：

```
启动横幅 / romfsInit / fsx::init / 网络初始化 / curl / ensureProjectLayout
读 url.txt / 读 settings.txt / 界面选择：appletType=… → 文字 或 图形
[UI] 构造：解析设置 → 标题 → 两行 → 状态区 → 图片位 → 底部 → 按钮状态 → 轮询任务
     状态区：状态 Label / 进度条 / 取消按钮 / 详情 Label / 完成
     主界面已创建 → 准备 pushView → pushView 已返回 → 轮询第 1..6 次 → 按钮图标已补上
```

判读：停在 `构造：xxx`／`状态区：xxx` → 就是那一步；有 `pushView 已返回` 但没有 `轮询第 1 次`
→ 死在首次渲染；末尾出现 `[!] 日志写入 SD 卡失败` → 日志本身不完整。

---

## 11. libnx / FS 错误码

- `0x00000F59` = module 345（`Module_Libnx`）+ 描述 7（`LibnxError_AlreadyInitialized`）
  → **不是失败**：`soc:` 只在 `bsdInitialize` 成功后才登记。
- `0x00307202` = FS 错误 6201（`OpenMode_AllowAppend is required ...`）→ 见 §4.1。
- `0x0000D46E` = 查询联网状态失败（Applet 模式下 nifm 的查询受限，不影响下载）。
- 其它：查 switchbrew 的 Error codes 页面，或 `libnx/nx/include/switch/result.h`。

---

## 12. 版本历史

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 单行下载器：一个链接输入框 + 保存目录 + 下载按钮 |
| 2.0.0 | 双行结构；启动自动建项目文件夹与 `url.txt`；`X` 快捷读取；两行各有 txt 图标；左摇杆可导航 |
| 2.1.x | 修输入框上限被 CI 补丁覆盖成 32；补 `nifmInitialize`；Applet 模式禁用系统键盘；新增 `log.txt`；SD 卡 I/O 串行化；`AlreadyInitialized` 按成功处理 |
| 2.2.0 | 移除收尾路径上的全部动画/视图栈操作 → 进度与结果改为页内显示；worker 终态最后置位；`fsx` 接口全部加锁；修轮询任务每帧都跑 |
| 2.3.0 | 修「写入 SD 卡失败 0x00307202」；新增 `img:` 启动自动加载图片；取消按钮「仅任务中可聚焦」；新增底部锚点与诊断信息页 |
| 2.4.0 | 修图片「已加载却全白」（弃用 `brls::Image` → 自绘 + 内存解码 + 错误码 + 显存预算）；构造函数不碰 GPU；构造函数逐步面包屑；日志写失败可在界面看到 |
| 2.5.0 | 启动阶段 SD 卡冲击降到最低：日志「打开一次 + 逐行追加」（每行 6 次 FS 操作 → 1 次）；`url.txt`/`settings.txt` 读取提前到 UI 之前（构造函数不碰 SD）；去掉多余的 `fsdevMountSdmc()`；内容未变不写 `settings.txt`。定位粒度做到按控件 + 记录 applet 焦点状态 |
| **2.6.0** | **Applet 模式改用文字（控制台）界面**：不再加载 borealis/GL/字体，改为 检查更新 / 下载 / 重读 `url.txt` / **SD 卡 I/O 自检**（这同时是「到底卡在哪」的决定性实验）；`console.txt` / `gui.txt` 可强制切换界面。证据表明**冻结与图形渲染无关**（GL/字体/窗口都成功初始化，冻结点是纯 CPU 的视图构造，且紧跟一次 SD 写盘） |
