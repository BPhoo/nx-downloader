# NX Downloader（v2.4.0）

Nintendo Switch 自制程序（`.nro`）：上行检查更新、下行下载文件，并在启动时自动加载 `url.txt` 里 `img:` 指定的图片。

```
更新链接  [ 输入框 ]  [txt图标]              [ 访问 ]
下载链接  [ 输入框 ]  [txt图标] [dir图标]    [ 访问 ]
状态：就绪
[ 进度条 ]
[ 取消当前任务 ]        ← 只有任务进行中才出现（否则完全收起）
详情 / 返回内容：……
图片：2/2 已加载
[ 图片 1 ]  [ 图片 2 ]  ← 固定 300px 高、等比缩放着色，不可聚焦
下载保存到：sdmc:/（第二行的文件夹图标可更换）
[ 诊断信息与提示 ]      ← 底部锚点：可聚焦，保证能滚到最底
```

> 图标按钮的贴图在**第一帧之后**才补上（肉眼无差别），原因见 §1.2。

---

## 1. v2.4.0 修了什么

### 1.1 图片「已加载」却什么都不显示 —— 根因是 `brls::Image` 的两处硬伤

模拟器截图显示进度条 100%、`图片：4/4 已加载`，但图片区一片空白。查下来是 borealis 的
`brls::Image` 有两个问题，两个都踩到了：

**① 它只能吃「文件路径」，内部走 `nvgCreateImage(path)` → `stb_image` 的 `fopen`。**

```c
// borealis/library/lib/image.cpp
this->texture = nvgCreateImage(vg, this->imagePath.c_str(), 0);
// nanovg.c：fopen 失败就返回 0 —— **不报错、不抛异常**
img = stbi_load(filename, &w, &h, &n, 4);
if (img == NULL) return 0;
```

而本程序的 SD 卡访问一直走**裸 `FsFileSystem`**（更可控、也不会因为 hbmenu 已挂载过 sdmc 而失败），
`sdmc:` 从来没挂到 devoptab 上 → `fopen("sdmc:/…")` 一律失败 → 纹理 ID `0`。
于是「加载计数」明明在涨，画出来却什么都没有。**这就是那条「4/4 已加载但全白」的真相。**

**② 它的 `layout()` 会缓存「首次布局时的高度」，并用 0×0 纹理尺寸算出 `0/0 = NaN`。**

```c
void Image::layout(...)
{
    if (this->origViewWidth == 0 || this->origViewHeight == 0)
    {   // ★ Application::pushView() 里有一句 view->invalidate(true)，
        //   会在第一帧之前强制布局一次 —— 那会儿图片还是折叠状态，高度 0 被永久缓存
        this->origViewWidth  = this->getWidth();
        this->origViewHeight = this->getHeight();     // = 0
    }
    nvgImageSize(vg, this->texture, &this->imageWidth, &this->imageHeight);  // 0×0
    float imageAspectRatio = (float)this->imageWidth / (float)this->imageHeight;  // 0/0 = NaN
    ...   // NaN 一路灌进 setHeight 与 nvgImagePattern
```

NaN 参与布局与 paint 计算是**未定义行为**（`(int)NaN` 在 ARM 上得到 `INT_MIN`），
在真机上完全可能表现为随机崩溃/白屏。

**修法：不用 `brls::Image`，自己写一个 `RemoteImageView`**（`source/main_view.cpp`）：

- 先用 `fsx::readBinaryFile()` 把字节读出来（裸 `FsFileSystem`，**不依赖 devoptab**），
  再交给 `nvgCreateImageMem()` 解码 —— 成功失败都有明确返回值；
- 解完立刻用 `nvgImageSize()` 问真实尺寸，`0×0` 就判定失败并删掉纹理；
- 缩放比例全部在 `draw()` 里现算，**尺寸只用整数**，并对 `ratio / drawW / drawH`
  做有限性检查（非正数直接不画）—— 结构上不可能产生 NaN；
- 加了一条保险：`fsx::init()` 里顺手 `fsdevMountSdmc()`，把 `sdmc:` 也挂到 devoptab 上，
  这样将来任何走 stdio 的第三方库都不会再无声失败。

顺带加了**显存预算**，避免图片把内存吃光（Applet 模式内存小得多）：

| 限制 | 值 |
| --- | --- |
| 单张文件大小 | 12 MB（超过直接跳过，**绝不截断**，半张图比报错更难查） |
| 单张像素 | 4M 像素（≈16MB 显存） |
| 总像素预算 | 完整内存模式 16M / **Applet 模式 6M** |

失败原因会写进日志与诊断页（`文件太大 12.4 MB` / `分辨率太高` / `不是能解码的图片` / `读不到文件`）。

### 1.2 真机 Applet 模式（从相册启动）一启动就死机

日志停在 `[017] [UI] 启动提示：当前是 Applet 模式…`，**构造函数自己的日志一行都没写出来** ——
也就是说崩溃发生在 `new MainView(...)` 这个区间里（此前构造函数里没有任何日志，所以只能靠猜，代价是一轮一轮试）。

本轮做了三件事，先把「能确定的事」全部改掉，并把不确定的变成**可定位的**：

**① 把构造函数里唯一碰 GPU 的东西挪出去。**

构造函数里唯一会碰图形硬件的是 `Button::setImage()`（它内部 `nvgCreateImage` →
`glGenTextures` / `glTexImage2D`，**立刻创建 GL 纹理**）。现在两个图标按钮改为
**第一帧画完之后**再补贴图（`InputRow::loadIcons()`，肉眼无差别），
构造函数从此**完全不碰 GPU**，只剩纯 CPU 的视图树搭建。

**② 把「只在 Applet 模式下才会创建」的那段从构造函数里拿掉。**

原来「Applet 模式」这条提示会额外建一个 `Label` —— 于是两种启动模式构造出来的视图树**不一样**，
真机一崩就没法判断是不是这个 Label 引起的。现在提示只在构造函数里**存起来**，
等界面跑起来后写进状态区/详情区（`showStartupNotice()`）。
**现在两种模式下构造函数的动作完全一致**，崩溃点因此具备可比性。

**③ 加了「指哪打哪」的面包屑日志。**

```
[UI] 构造：读设置 → 构造：标题 → 构造：两行 → 构造：状态区 → 构造：图片位
     → 构造：底部 → 构造：按钮状态 → 构造：恢复上次的链接 → 构造：轮询任务
     → 主界面已创建（图片位 N 个）
[UI] 准备 pushView 主界面 → pushView 已返回，进入主循环
[UI] 轮询第 1 次（主循环正常）…… 第 5 次
[UI] 首帧之后：按钮图标已补上（GL 纹理创建正常）
[UI] 图片加载开始：共 N 项，显存预算 16M 像素（模式=…）
```

下一份日志的**最后一行**就会直接指出死在哪一步：

| 日志停在 | 说明 |
| --- | --- |
| `构造：xxx` 之前/之中 | 构造函数那一段的问题，且已经缩小到一个具体动作 |
| `pushView 已返回` 之后、没有 `轮询第 1 次` | 死在**首次渲染**（字体图集/GL 绘制） |
| 有 `轮询第 1..2 次`、没有 `轮询第 3 次` | 主循环活着，问题在图片加载那一步 |

### 1.3 日志本身也可能是「假的短」

`logx` 每次写日志都是「整份重写文件」。如果某个瞬间 SD 卡写入失败，
**日志会静静地停在上一次成功的那一行** —— 看起来就像「程序死在这一步」，其实是后面的行没写下来。
（这是排查时最容易上当的地方。）

现在 `logx::line()` 会检查写入结果：一旦失败就标记出来，**在界面上显示「⚠ 日志写入失败」**，
并在下次写入成功时把「写入已恢复（之前有内容丢失）」补进日志。诊断页也会带上这条状态。

---

## 2. v2.3.0 修过什么

### 2.1 `写入 SD 卡失败（0x00307202）` —— 硬 bug

switchbrew 的错误码表：

```
0x307202  6201  Error: OpenMode_AllowAppend is required for implicit extension
                of file size by WriteFile().
```

`FsOpenMode_Append` 在 Nintendo FS 里是 **`OpenMode_AllowAppend`（允许写入时扩大文件长度）**，
不是「只能追加」。libnx 自己的 `fsdev` 就是这么开的：

```c
case O_WRONLY: fsdev_flags |= FsOpenMode_Write | FsOpenMode_Append; break;
```

修复两层：① `createAndOpenFile` / `writeWholeFile` 一律 `FsOpenMode_Write | FsOpenMode_Append`；
② 能从 `Content-Length` 得知总长度（≤256MB）时**先把文件长度定好**，写入全部落在长度之内，
顺便省掉 FAT32 逐块扩簇的开销。

### 2.2 新增 `url.txt` 的 `img:` —— 启动自动加载图片

```
{
    updata:   https://example.com/version.txt ,
    download: https://example.com/app.nro ,
    img:      https://example.com/a.jpg, https://example.com/b.jpg
}
```

- 每一项可以是 **http(s) 链接**（自动下载并缓存到 `img/`），也可以是**已放在 SD 卡上的文件名**
  （按「原文 → 项目文件夹 → SD 根目录」找）。
- 本地已有缓存的**直接加载、不联网**；缺的才逐张下载（顺序执行、可取消、失败即停）。
- 缓存名是 `<序号>-<原名>`（如 `img/1-a.jpg`）：不同图片重名不冲突，改了顺序/内容也不会读到旧缓存。
  想重下就删掉 `img/` 里对应文件。
- JPEG / PNG 都支持（已确认 borealis 的 `STBI_NO_JPEG` 未生效）。

### 2.3 那条「可以选中的横线」

它是**取消按钮**：borealis 判定「能否聚焦」只看 `getDefaultFocus()` 是否非空，而 `Button` 永远返回自身
→ 禁用的按钮仍可被选中；同时它在 `List` 里没人给高度（`Dialog` 里是 `Dialog::layout` 给的），
0 高度就被画成一条线。改成子类：**只有任务进行中才可聚焦**，并显式给行高；空闲时用非动画方式完全收起。

### 2.4 底部内容看不全

borealis 的滚动**只跟随焦点**，页面底部没有可聚焦控件就永远滚不动。修法：底部加一个
**可聚焦的锚点按钮**「诊断信息与提示」，并把长文案（环境、日志路径、完整返回内容）挪进诊断页。

---

## 3. v2.2.0 修过什么（摘要）

真机日志显示网络早已成功（`curl_easy_perform = 0`、HTTP 200），卡死点在 UI 收尾：
`RepeatingTask → Dialog::close(cb) → menu_animation 回调 → Application::pushView`，
即**在动画回调里嵌套改视图栈**。v2.2.0 把这条路径整段删掉（不用 Dialog、不发通知、收尾只改文本），
worker 终态改为「最后一步置位」，`fsx` 全部公开接口加锁，修掉轮询任务「每帧都跑」。

> **两条设计铁律**（写进代码注释了）：
> 1. 工作线程运行期间/收尾时**绝不改动视图栈**（唯一例外：按键触发的 `pushView`）；
> 2. 页面最底部必须留一个**可聚焦**的控件，否则永远滚不到底。

---

## 4. 目录结构

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
    ├── main.cpp                  启动顺序：romfs → SD 卡 → 网络 → curl → 项目文件夹 → UI → 主循环
    ├── app_config.{hpp,cpp}      项目文件夹 / url.txt 的生成与宽松解析（含 img: 列表）
    ├── fsx.{hpp,cpp}             libnx FsFileSystem 封装（全部接口串行化、Append 位、预分配、
    │                              二进制整读 readBinaryFile、fileSize、顺带挂 sdmc devoptab）
    ├── logx.{hpp,cpp}            SD 卡日志（多线程安全、[UI] 标记、满了保留最近、写入失败可上报）
    ├── netx.{hpp,cpp}            nifm + socket 初始化
    ├── downloader.{hpp,cpp}      libcurl 下载/取文本；pthread 2MiB 栈；终态最后置位
    ├── main_view.{hpp,cpp}       主界面 + RemoteImageView（自绘图片）+ 诊断信息页
    ├── file_picker.{hpp,cpp}     选任意 .txt
    └── path_picker.{hpp,cpp}     选保存目录
```

> `source/progress_dialog.*` 自 v2.2.0 起废弃（进度改为页内显示）。
> 上传通道（GitHub 网页 `/upload`）只能新增/覆盖、**不能删文件**，
> 所以流水线里有一句 `rm -f source/progress_dialog.{cpp,hpp}` 保证它们不参与编译。

---

## 5. SD 卡上的文件

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro              程序本体
├── url.txt                        首次启动自动生成（updata / download / img）
├── settings.txt                   界面里改过的链接与保存目录（自动写回）
├── log.txt                        运行日志（每次启动重建；真机排错就看它）
├── update_result.txt              上次检查更新拿到的完整返回内容
├── img/                           图片缓存（1-xxx.jpg、2-yyy.jpg …）
└── cacert.pem                     可选：放了它就严格校验 HTTPS 证书
```

`url.txt` 的解析很宽松：`updata`/`update` 都认、`img`/`image`/`images` 都认、`:` 与 `=` 都认、
可加引号、`// # ;` 开头的整行按注释忽略、链接省略协议会自动补 `https://`。
注意 `img:` 的值**不做补协议**（它也可能是本地文件名）。

---

## 6. 安装与操作

1. 把 `nx-downloader.nro` 放到 `SD:/switch/nx-downloader/`。
2. **推荐按住 <kbd>R</kbd> 从游戏图标启动**（完整内存模式）。从「相册」启动属于 Applet 模式：
   程序会禁用系统键盘（真机实测按 A 调键盘有死机风险），此时用 `X` 或文件图标读 `url.txt`。
3. 首次启动自动创建项目文件夹与 `url.txt`。
4. 操作：
   - **A** 输入链接（最长 100 字符）· **X** 读取 `url.txt` 里对应的值
   - **txt 图标** 选任意 `.txt` 取字段 · **dir 图标** 选下载保存目录（默认 `sdmc:/`）
   - **十字键 / 左摇杆** 移动焦点 · **诊断信息与提示** 看图片状态/环境/日志/完整返回内容 · **+** 退出

**同名文件会被直接覆盖**（写入侧是「先删再建」），界面上不弹确认框。

---

## 7. 从源码编译

### 7.1 云端编译（推荐，本机无需装工具链）

推送代码即触发 `.github/workflows/build-nro.yml`：

- 先试 devkitPro 官方脚本装工具链；`apt.devkitpro.org` 被 Cloudflare 拦时自动退回官方镜像
  `devkitpro/devkita64`；并在摘要里打印 libnx / switch-curl 版本。
- 编译用 `make -k`，把所有 `error:` 行写进 Step Summary（一次 CI 看到全部编译错误）。
  注意 Actions 的 run 块是 `bash -e`：`make` 一失败脚本就中断，所以摘要段落里加了 `|| echo` 兜底。
- 产物在运行页的 Artifacts 里（`nx-downloader-nro`）。**公开仓库下载 artifact 也必须登录 GitHub。**

### 7.2 本地编译

```bash
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$PATH

git clone --depth=1 --recursive -b master https://github.com/XITRIX/borealis.git borealis
make sync-resources
make -j$(nproc)
```

踩过的关键点：

- **borealis 必须 `-b master`**：默认分支 `moonlight_wiliwili` 结构不同，找不到 `library/borealis.mk`。
- **链接参数用 `curl-config --libs`**：`LIBS := $(CURL_LIBS) -lnx -lm`。
- **保留 RTTI / 异常**：不要加 `-fno-rtti` / `-fno-exceptions`。
- **`.DEFAULT_GOAL := all`**：`sync-resources` 写在 `all` 前面，不写这句 `make` 会 1 秒结束且不出包。
- **CI 里不要升级工具链**：镜像自带 libnx 4.12.0（满足固件 21.x 的 TLS ABI 要求）。

---

## 8. 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| **图片「N/N 已加载」但画不出来** | 已修（v2.4.0）：不再用 `brls::Image`（它走 `fopen`，需要 `sdmc:` devoptab，且会踩 0×0→NaN），改成自绘视图 + 内存解码。若仍不显示，诊断页会写出每一格的具体原因。 |
| **真机 Applet 模式一启动就死** | v2.4.0 已把构造函数里唯一碰 GPU 的动作（图标贴图）挪到首帧之后，并把 Applet 专属的提示移出构造函数。请再跑一次并把 `log.txt` 发回：最后一行面包屑直接指出死在哪一步（见 §1.2 的表）。 |
| 界面卡死 | v2.2.0 起收尾路径不再动视图栈。看 `log.txt` 最后一行的 `[UI]` 面包屑；心跳停止＝渲染循环不动了。 |
| 日志很短、像是「死在某一步」 | 先看界面有没有「⚠ 日志写入失败」：SD 卡写入失败时日志会停在上一次成功的那一行（v2.4.0 起会显式提示）。 |
| 底部内容看不全 | 已修（v2.3.0）：底部加了可聚焦的「诊断信息与提示」锚点（borealis 的滚动只跟随焦点）。 |
| 图片加载失败 | 诊断页/日志会写出原因：`文件太大`（上限 12MB）、`分辨率太高`（上限 4M 像素）、`不是能解码的图片`、`读不到文件`、`显存预算已用完`（Applet 模式总预算 6M 像素）。 |
| **写入 SD 卡失败 (0x00307202)** | 已修（v2.3.0）：FS 6201 = 需要 `OpenMode_AllowAppend` 才能扩大文件。若仍出现，把 `log.txt` 发回。 |
| 启动提示「网络不可用」 | `LibnxError_AlreadyInitialized`（`0x0F59`）按**成功**处理（恰恰说明 socket 已可用）。其它错误码见 §9。 |
| 输入框只能输 32 个字符 | 已修（v2.1.0）：CI 补丁把 `maxStringLength` 覆盖成了 32，现改成整行删除 + 断言。 |
| 按 A 输入就死机 | Applet 模式下调系统键盘有风险：用 `X` / 文件图标，或按住 R 从游戏图标启动。 |
| 中文显示成方块 | 程序会挂载系统中简字体作为 fallback（日志记录 `plGetSharedFontByType` 的返回值）；主机缺该字体则无解。 |
| HTTPS 报证书错误 | 设备没有 CA 链时自动降级为「不校验证书」并在提示里注明；要严格校验就把 `cacert.pem` 放到项目文件夹。 |
| HTTP 4xx/5xx 报「下载失败」 | 刻意的：避免把服务器错误页面当成文件存下来。 |
| 下载很慢 / 进度不动 | 点「取消当前任务」；超时阈值是 15s 连接、60s 低速。 |
| 日志里出现「前文过长已丢弃」 | 正常：日志满 24KB 后只保留最近部分（最后几行最有价值）。 |

### 8.1 拿日志定位（v2.4.0 起）

`log.txt` 每次启动重建，按顺序读就能还原整个过程：

- `[UI]` 前缀 = UI 线程，无前缀 = 下载工作线程；
- `构造：…` 一行一步 → 崩溃点缩小到单个动作；
- `轮询第 N 次（主循环正常）` → 证明主循环在跑；
- `下载图片`/`图片 N 已加载`/`图片 N 加载失败` → 图片这条路的每一步；
- 末尾若出现 `[!] 日志写入 SD 卡失败` → 说明日志本身不完整。

---

## 9. libnx / FS 错误码

`Result` 编码为 `(module << 9) | description`：

- `0x00000F59` = module 345（`Module_Libnx`）+ 描述 7（`LibnxError_AlreadyInitialized`）
  → **不是失败**：`soc:` 只在 `bsdInitialize` 成功后才登记，能拿到它就说明 socket 早就好了。
- `0x00307202` = FS 错误 6201（`OpenMode_AllowAppend is required ...`）→ 见 §2.1。
- 其它：查 switchbrew 的 Error codes 页面，或 `libnx/nx/include/switch/result.h`。

---

## 10. 版本历史

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 单行下载器：一个链接输入框 + 保存目录 + 下载按钮 |
| 2.0.0 | 双行结构：上行检查更新、下行下载；启动自动建项目文件夹与 `url.txt`；`X` 快捷读取；两行各有 txt 图标；左摇杆可导航 |
| 2.1.0 | 修输入框上限被 CI 补丁覆盖成 32；补 `nifmInitialize`；Applet 模式禁用系统键盘；新增 `log.txt`；SD 卡 I/O 串行化；去掉每帧文字测量 |
| 2.1.1 | `AlreadyInitialized` 按成功处理；去掉未导出的 `bsdSocket`；CI 失败时也保证生成 Step Summary |
| 2.2.0 | 移除收尾路径上的全部动画/视图栈操作 → 进度与结果改为页内显示；worker 终态最后置位；`fsx` 接口全部加锁；修轮询任务每帧都跑；新增面包屑与心跳日志 |
| 2.3.0 | 修「写入 SD 卡失败 0x00307202」（`FsOpenMode_Append` + 预分配）；新增 `img:` 启动自动加载图片；取消按钮「仅任务中可聚焦」；新增底部锚点与诊断信息页 |
| **2.4.0** | **修图片「已加载却全白」**：弃用 `brls::Image`（`fopen` 依赖 devoptab + 0×0→NaN 的布局缓存），改成自绘 `RemoteImageView`（内存解码、明确错误码、整数尺寸、显存预算）；`fsx::init` 顺带 `fsdevMountSdmc()`。**针对真机 Applet 模式一启动就死**：构造函数里不再碰 GPU（图标改在首帧后加载）、Applet 专属提示移出构造函数（两种模式构造完全一致）、构造函数逐步面包屑 + 轮询计数 + `pushView` 前后标记。**日志写入失败可上报**（不再出现「日志假性变短」）。 |
