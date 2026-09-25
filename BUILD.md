# NX Downloader（v2.5.0）

Nintendo Switch 自制程序（`.nro`）：上行检查更新、下行下载文件，并在启动时自动加载 `url.txt` 里 `img:` 指定的图片。

```
更新链接  [ 输入框 ]  [txt图标]              [ 访问 ]
下载链接  [ 输入框 ]  [txt图标] [dir图标]    [ 访问 ]
状态：就绪
[ 进度条 ]
[ 取消当前任务 ]        ← 只有任务进行中才出现（否则完全收起）
详情 / 返回内容：……
图片：2/2 已加载
[ 图片 1 ]  [ 图片 2 ]  ← 固定 300px 高、等比缩放，不可聚焦
下载保存到：sdmc:/（第二行的文件夹图标可更换）
[ 诊断信息与提示 ]      ← 底部锚点：可聚焦，保证能滚到最底
```

---

## 1. v2.5.0（本轮）修了什么

**背景**：真机日志（Applet 模式）停在这一行——

```
[021] [UI] 构造：两行
[022] [UI] 构造：状态区        ← 之后什么都没有
```

也就是说崩溃点被夹在 `buildStatusArea()` 这一个函数里（面包屑是上版新加的，一次就把范围从「主界面某处」缩到一个函数）。
把 `Label` / `Button` / `ProgressDisplay` 的构造函数逐个读完，**这个函数里没有任何 GPU 调用、没有任何文字测量、没有 SD 访问**，
全是纯 CPU 的视图树搭建 —— 唯独它前面**紧挨着一次 SD 卡写盘**（写 `[022]` 那行日志）。

### 1.1 把启动阶段的 SD 卡 I/O 大幅减少（主要修改）

`Applet 模式（从相册启动）`下 SD 卡是与父 applet / 系统**共用**的。原来我们的启动流程对 SD 卡相当粗暴：

| 原来的行为 | 每次操作的 FS 调用数 | 启动阶段总量 |
| --- | --- | --- |
| **每写一行日志**都「删文件 → 建文件 → 打开 → 写 → flush → 关」（整份重写） | 6 次 | 二十多行 → **上百次** |
| 构造函数里读 `settings.txt`、读 `url.txt`、写回 `settings.txt` | 数次 | 又一轮 |
| 额外挂一个 `fsdevMountSdmc()`（多一个 SD 卡会话） | 1 次 + 会话 | — |

改成：

1. **日志改为「启动时打开一次、之后逐行追加」**（`fsx::openForAppend` + `fsx::writeFileChunk`）：
   每行从 **6 次 FS 操作降到 1 次**，而且只写新增的那几十字节，不再反复删建 2KB 的文件。
2. **构造函数里彻底不碰 SD 卡**：`url.txt` / `settings.txt` 的读取搬到 `main.cpp`（UI 初始化之前），
   解析好的结果传进 `MainView`。这样构造函数只剩「纯 CPU 的视图树搭建」+ 逐行日志。
3. **去掉 `fsdevMountSdmc()`**：本程序自己走裸 `FsFileSystem`，图片也改成内存解码，根本不需要 devoptab，
   少一个 SD 卡会话就少一分互相干扰的机会。
4. **不再写无意义的 `settings.txt`**：启动时把上次的值填回输入框会触发回写，内容其实没变 →
   加 `applyingSaved` 抑制 + 内容比对，内容相同就不写盘。

> 这些改动本身都是「该做的事」（少写盘、少开会话、不写重复内容），同时也直接针对观察到的现场：
> **崩溃点紧跟在一次 SD 写盘之后**。

### 1.2 顺便把「崩溃点」的判读粒度做到按控件

`buildStatusArea()` 现在逐个控件打面包屑：

```
[UI] 构造：状态区
[UI] 状态区：状态 Label
[UI] 状态区：进度条
[UI] 状态区：取消按钮
[UI] 状态区：详情 Label
[UI] 状态区：完成
```

并且新增 **applet 焦点状态**记录（前 6 次轮询各记一次）：

```
[UI] 轮询第 1 次：主循环正常，appletType=2(Applet（相册启动）)，焦点=InFocus(1)，屏幕=掌机
```

为什么记它：Applet 失焦（`OutOfFocus` / `Background`）时，系统会接管 SD 卡与显示，
**此时做 SD 读写最容易互相干扰**。如果下一份日志显示卡死前焦点状态发生了变化，问题的方向就确定了。

### 1.3 仍然要说明的边界

这一版**没有**做到「确定修好 Applet 模式死机」——它做的是两件事：
①把最可疑的环境因素（启动瞬间的 SD 卡 I/O 洪峰）压到最低；
②把定位粒度从「主界面某处」缩到「单个控件 / 单次调用」。
下一份日志的最后一行就是结论。

---

## 2. v2.4.0 修过什么

### 2.1 图片「已加载却全白」：`brls::Image` 的两处硬伤

**① 它只吃「文件路径」，内部是 `fopen`。**

```c
// borealis/library/lib/image.cpp
this->texture = nvgCreateImage(vg, this->imagePath.c_str(), 0);
// nanovg.c：fopen 失败就返回 0，**不报错**
img = stbi_load(filename, &w, &h, &n, 4);
if (img == NULL) return 0;
```

本程序 SDK 卡访问一直走**裸 `FsFileSystem`**，`sdmc:` 从未挂到 devoptab 上 →
`fopen("sdmc:/…")` 必然失败 → 纹理 ID `0`。这就是「计数在涨、屏幕全白」。

**② 它的 `layout()` 会缓存「首次布局时的高度」。** `Application::pushView()` 里有一句
`view->invalidate(true)`（同步强制布局整棵树）——那一刻图片还折叠着，**0 被永久缓存**，
再用 0×0 的纹理尺寸算出 `0/0 = NaN` 灌进 `setHeight` / `nvgImagePattern`（`(int)NaN` 是 UB）。

**修法**：弃用 `brls::Image`，自绘 `RemoteImageView`：`fsx::readBinaryFile`（二进制整读，超限直接失败不截断）
→ `nvgCreateImageMem` 解码 → `nvgImageSize` 验尺寸；失败给出明确原因
（读不到 / 文件太大 / 解码失败 / 分辨率太高 / 显存预算用完）。尺寸只用整数、带有限性检查，结构上不可能产生 NaN。

**显存预算**（Applet 模式内存小得多）：单文件 ≤12MB、单张 ≤4M 像素、总量 完整内存 16M / **Applet 6M 像素**。

### 2.2 构造函数不要碰 GPU / 不要有「只在某种模式下才建」的东西

- `Button::setImage()` 会立刻创建 GL 纹理 → 图标改到主循环第一轮才加载（`InputRow::loadIcons()`）。
- Applet 专属提示原先在构造函数里多建一个 Label（两种模式视图树不同、崩了无法比较）→ 改成只存、界面起来后再显示。
- 构造函数逐步面包屑 + 轮询计数 + `pushView` 前后标记。
- `logx` 写盘失败时**在界面上显示「⚠ 日志写入失败」**（避免「SD 写失败 → 日志静静停在上次成功那行 → 看起来像死在那一步」）。

---

## 3. v2.3.0 修过什么

### 3.1 `写入 SD 卡失败（0x00307202）`

```
0x307202  6201  Error: OpenMode_AllowAppend is required for implicit extension
                of file size by WriteFile().
```

`FsOpenMode_Append` 是 **`OpenMode_AllowAppend`（允许写入时扩大文件长度）**，不是「只能追加」。
libnx 自己的 `fsdev` 就是 `FsOpenMode_Write | FsOpenMode_Append`。
修复：① 打开时带上该位；② 能从 `Content-Length` 得知总长度（≤256MB）时先 `fsFsCreateFile(path, total, 0)` 预分配。

### 3.2 `img:` —— 启动自动加载图片

```
{ updata: … , download: … , img: https://a.com/1.jpg, https://a.com/2.jpg }
```

每项可以是 **http(s) 链接**（自动下载并缓存到 `img/`），也可以是**已放在 SD 卡上的文件名**
（按「原文 → 项目文件夹 → SD 根目录」找）。本地已有缓存的直接加载不联网；缓存名 `<序号>-<原名>`（改顺序/换图不会读到旧缓存）。
JPEG / PNG 都支持。

### 3.3 「可以选中的横线」与「底部看不全」

- 那条线是**取消按钮**：borealis 判定可聚焦只看 `getDefaultFocus()` 是否非空，而 `Button` 永远返回自身；
  且它在 `List` 里没人给高度（0 高 = 一条线）。→ 子类化，**只有任务进行中才可聚焦**，显式给行高，空闲时完全收起。
- 看不全是因为 **borealis 的滚动只跟随焦点** → 底部加一个**可聚焦的锚点按钮**「诊断信息与提示」，长文案挪进诊断页。

---

## 4. v2.2.0 摘要

真机日志显示网络早已成功（`curl_easy_perform = 0`、HTTP 200），卡死点在 UI 收尾：
`RepeatingTask → Dialog::close(cb) → menu_animation 回调 → Application::pushView`，即**在动画回调里嵌套改视图栈**。
v2.2.0 把这条路径整段删掉（不用 Dialog、不发通知、收尾只改文本），worker 终态改为「最后一步置位」，
`fsx` 全部公开接口加锁，修掉轮询任务「每帧都跑」。

> **两条设计铁律**：
> 1. 工作线程运行期间/收尾时**绝不改动视图栈**（唯一例外：按键触发的 `pushView`）；
> 2. 页面最底部必须留一个**可聚焦**的控件，否则永远滚不到底。

---

## 5. 目录结构

```
nx-downloader/
├── Makefile                      devkitPro/libnx 标准模板（已接好 borealis 与 switch-curl）
├── icon.jpg                      主图标
├── BUILD.md                      本文件
├── BUILD-v2.2.0.md / v2.1.1.md   历史版本说明（存档）
├── .github/workflows/build-nro.yml    在 GitHub 服务器云编译，产出 nx-downloader.nro
├── romfs/
│   ├── i18n/en-US/brls.json      borealis 文案
│   ├── icon/txt.png              「选 txt」图标（128×128）
│   ├── icon/folder.png           「选保存目录」图标（128×128）
│   └── material/MaterialIcons-Regular.ttf
└── source/
    ├── main.cpp                  启动顺序：romfs → SD 卡 → 日志 → 网络 → curl → 项目文件夹
    │                              → 读 url.txt/settings.txt → UI → 主循环
    ├── app_config.{hpp,cpp}      项目文件夹 / url.txt 的生成与宽松解析（含 img: 列表）
    ├── fsx.{hpp,cpp}             libnx FsFileSystem 封装（全部接口串行化、Append 位、预分配、
    │                              追加写 openForAppend、二进制整读 readBinaryFile、fileSize）
    ├── logx.{hpp,cpp}            SD 卡日志（打开一次逐行追加、[UI] 标记、过长轮转、写入失败可上报）
    ├── netx.{hpp,cpp}            nifm + socket 初始化；applet 焦点/模式描述
    ├── downloader.{hpp,cpp}      libcurl 下载/取文本；pthread 2MiB 栈；终态最后置位
    ├── main_view.{hpp,cpp}       主界面 + RemoteImageView（自绘图片）+ 诊断信息页
    ├── file_picker.{hpp,cpp}     选任意 .txt
    └── path_picker.{hpp,cpp}     选保存目录
```

> `source/progress_dialog.*` 自 v2.2.0 起废弃（进度改为页内显示）。
> 上传通道（GitHub 网页 `/upload`）只能新增/覆盖、不能删文件，所以流水线里有一句
> `rm -f source/progress_dialog.{cpp,hpp}` 保证它们不参与编译。

---

## 6. SD 卡上的文件

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro              程序本体
├── url.txt                        首次启动自动生成（updata / download / img）
├── settings.txt                   界面里改过的链接与保存目录（自动写回，内容没变不写）
├── log.txt                        运行日志（每次启动重建；真机排错就看它）
├── update_result.txt              上次检查更新拿到的完整返回内容
├── img/                           图片缓存（1-xxx.jpg、2-yyy.jpg …）
└── cacert.pem                     可选：放了它就严格校验 HTTPS 证书
```

`url.txt` 解析很宽松：`updata`/`update` 都认、`img`/`image`/`images` 都认、`:` 与 `=` 都认、可加引号、
`// # ;` 开头的整行按注释忽略、链接省略协议会自动补 `https://`。注意 `img:` 的值**不做补协议**（也可能是本地文件名）。

---

## 7. 安装与操作

1. 把 `nx-downloader.nro` 放到 `SD:/switch/nx-downloader/`。
2. **推荐按住 <kbd>R</kbd> 从游戏图标启动**（完整内存模式）。从「相册」启动属 Applet 模式：
   系统键盘被禁用（按 A 调键盘有死机风险），此时用 `X` 或文件图标读 `url.txt`。
3. 首次启动自动创建项目文件夹与 `url.txt`。
4. 操作：**A** 输入链接（≤100 字符）· **X** 读 `url.txt` 对应值 · **txt 图标** 选 `.txt` ·
   **dir 图标** 选保存目录（默认 `sdmc:/`）· **十字键/左摇杆** 移动焦点 ·
   **诊断信息与提示** 看图片状态/环境/日志/完整返回内容 · **+** 退出。

同名文件会被直接覆盖，界面不弹确认框。

---

## 8. 从源码编译

### 8.1 云端编译（推荐，本机无需装工具链）

推送代码即触发 `.github/workflows/build-nro.yml`：先试 devkitPro 官方脚本装工具链，
被 Cloudflare 拦时退回官方镜像 `devkitpro/devkita64`；摘要里打印 libnx / switch-curl 版本；
`make -k` + 把所有 `error:` 行写进 Step Summary（一次 CI 看到全部编译错误）。
产物在运行页 Artifacts 里（**公开仓库下载 artifact 也必须登录 GitHub**）。

### 8.2 本地编译

```bash
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$PATH
git clone --depth=1 --recursive -b master https://github.com/XITRIX/borealis.git borealis
make sync-resources
make -j$(nproc)
```

关键点：borealis 必须 `-b master`；链接参数用 `curl-config --libs`；保留 RTTI/异常；
`.DEFAULT_GOAL := all`（`sync-resources` 写在前面，不写这句 `make` 会 1 秒结束不出包）；
别在 CI 里升级工具链（镜像自带 libnx 4.12.0 已满足固件 21.x 的 TLS ABI 要求）。

---

## 9. 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| **Applet 模式（相册启动）一启动就死** | 见 §1：已把构造函数里的 SD 访问与 GPU 调用全部移出、把日志写盘从每行 6 次 FS 操作降到 1 次、去掉多余 SD 会话并在日志里记录 applet 焦点状态。请把新的 `log.txt` 发回——最后一行会直接指出死在哪个控件/哪次调用。**稳定可用路径：按住 R 从游戏图标启动。** |
| 图片「N/N 已加载」但画不出来 | 已修（v2.4.0）：不再用 `brls::Image`（`fopen` 依赖 devoptab + 0×0→NaN 的布局缓存），改自绘 + 内存解码。诊断页会写出每一格的具体原因。 |
| 图片加载失败 | 诊断页/日志会给出原因：`文件太大`（上限 12MB）、`分辨率太高`（上限 4M 像素）、`不是能解码的图片`、`读不到文件`、`显存预算已用完`（Applet 总预算 6M 像素）。 |
| **写入 SD 卡失败 (0x00307202)** | 已修（v2.3.0）：FS 6201 = 需要 `OpenMode_AllowAppend` 才能扩大文件。若仍出现，把 `log.txt` 发回。 |
| 日志很短、像「死在某一步」 | 先看界面上有没有「⚠ 日志写入失败」：SD 写失败时日志会停在上次成功那行（v2.4.0 起会显式提示）。v2.5.0 起日志改为追加写，这类风险大幅降低。 |
| 启动提示「网络不可用」 | `LibnxError_AlreadyInitialized`（`0x0F59`）按**成功**处理（它恰恰说明 socket 已可用）。其它错误码见 §10。 |
| 界面卡死 | v2.2.0 起收尾路径不再动视图栈。看 `log.txt` 最后一行 `[UI]` 面包屑；心跳停止＝渲染循环不动了。 |
| 底部内容看不全 | 已修（v2.3.0）：底部加了可聚焦的「诊断信息与提示」锚点。 |
| 输入框只能输 32 个字符 | 已修（v2.1.0）。 |
| 按 A 输入就死机 | Applet 模式下调系统键盘有风险：用 `X` / 文件图标，或按住 R 从游戏图标启动。 |
| 中文显示成方块 | 程序挂载系统中简字体作为 fallback；主机缺该字体则无解。 |
| HTTPS 报证书错误 | 设备没有 CA 链时自动降级为「不校验证书」并在提示里注明；要严格校验就把 `cacert.pem` 放到项目文件夹。 |
| HTTP 4xx/5xx 报「下载失败」 | 刻意的：避免把服务器错误页面当成文件存下来。 |

### 10. 怎么读日志（v2.5.0 起）

`log.txt` 每次启动重建，按顺序读就能还原整个过程：

```
[001] 启动横幅 + appletType
[003] romfsInit / fsx::init
[005] 网络初始化（nifm → socket，含错误码）
[011] curl_global_init
[012] ensureProjectLayout
      读 url.txt = …（字节数 / 解析结果 / img 项数）
      读 settings.txt = …
[013] Application::init = OK
      环境 / applet 状态（appletType / 焦点 / 屏幕）
[015] 中文共享字体
[017] 启动提示（Applet 模式等）
[018] 准备 pushView 主界面
[019] 构造：解析设置 → 标题 → 两行 → 状态区 → 图片位 → 底部 → 按钮状态 → 轮询任务
        状态区：状态 Label / 进度条 / 取消按钮 / 详情 Label / 完成
        主界面已创建（图片位 N 个）
[..]  pushView 已返回，进入主循环
[..]  轮询第 1..6 次：主循环正常，appletType=… 焦点=…
[..]  主循环第一轮：按钮图标已补上（GL 纹理创建正常）
[..]  图片加载开始：共 N 项，显存预算 …M 像素
```

**判读**：
- 停在 `构造：xxx` → 构造函数那一段（已按控件细分）；
- 有 `pushView 已返回` 但没有 `轮询第 1 次` → 死在**首次渲染**（字体图集 / GL 绘制）；
- 有 `轮询第 1..2 次`、没有第 3 次 → 死在**图片加载**那一步；
- 末尾若出现 `[!] 日志写入 SD 卡失败` → 日志本身不完整，别据此判断崩溃点。

---

## 11. libnx / FS 错误码

`Result` 编码为 `(module << 9) | description`：

- `0x00000F59` = module 345（`Module_Libnx`）+ 描述 7（`LibnxError_AlreadyInitialized`）
  → **不是失败**：`soc:` 只在 `bsdInitialize` 成功后才登记，能拿到它就说明 socket 早就好了。
- `0x00307202` = FS 错误 6201（`OpenMode_AllowAppend is required ...`）→ 见 §3.1。
- 其它：查 switchbrew 的 Error codes 页面，或 `libnx/nx/include/switch/result.h`。

---

## 12. 版本历史

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 单行下载器：一个链接输入框 + 保存目录 + 下载按钮 |
| 2.0.0 | 双行结构：上行检查更新、下行下载；启动自动建项目文件夹与 `url.txt`；`X` 快捷读取；两行各有 txt 图标；左摇杆可导航 |
| 2.1.0 | 修输入框上限被 CI 补丁覆盖成 32；补 `nifmInitialize`；Applet 模式禁用系统键盘；新增 `log.txt`；SD 卡 I/O 串行化；去掉每帧文字测量 |
| 2.1.1 | `AlreadyInitialized` 按成功处理；去掉未导出的 `bsdSocket`；CI 失败时也保证生成 Step Summary |
| 2.2.0 | 移除收尾路径上的全部动画/视图栈操作 → 进度与结果改为页内显示；worker 终态最后置位；`fsx` 接口全部加锁；修轮询任务每帧都跑；新增面包屑与心跳日志 |
| 2.3.0 | 修「写入 SD 卡失败 0x00307202」（`FsOpenMode_Append` + 预分配）；新增 `img:` 启动自动加载图片；取消按钮「仅任务中可聚焦」；新增底部锚点与诊断信息页 |
| 2.4.0 | 修图片「已加载却全白」（弃用 `brls::Image` → 自绘 `RemoteImageView` + 内存解码 + 错误码 + 显存预算）；构造函数不碰 GPU（图标改主循环加载）；启动提示移出构造函数；构造函数逐步面包屑 + 轮询计数；日志写失败可在界面看到 |
| **2.5.0** | **把启动阶段对 SD 卡的冲击压到最低**：日志改「打开一次 + 逐行追加」（每行 6 次 FS 操作 → 1 次）；`url.txt` / `settings.txt` 的读取提前到 UI 之前，构造函数彻底不碰 SD 卡；去掉多余的 `fsdevMountSdmc()`（少一个 SD 会话）；内容未变不写 `settings.txt`。**同时把崩溃定位粒度做到按控件**：`buildStatusArea()` 逐控件面包屑 + 记录 applet 焦点状态（判断是否发生在失焦之后） |
