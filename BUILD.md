# NX Downloader（v2.3.0）

Nintendo Switch 自制程序（`.nro`）：上行检查更新、下行下载文件，并在启动时自动加载 `url.txt` 里 `img:` 指定的图片。

```
更新链接  [ 输入框 ]  [txt图标]              [ 访问 ]
下载链接  [ 输入框 ]  [txt图标] [dir图标]    [ 访问 ]
状态：就绪
[ 进度条 ]
[ 取消当前任务 ]        ← 只有任务进行中才出现（否则完全收起）
详情 / 返回内容：……
图片：2/2 已加载
[ 图片 1 ]  [ 图片 2 ]  ← 固定 300px 高、按 FIT 缩放，不可聚焦
下载保存到：sdmc:/（第二行的文件夹图标可更换）
[ 诊断信息与提示 ]      ← 底部锚点：可聚焦，保证能滚到最底
```

---

## 1. v2.3.0 修了什么

真机日志显示 v2.2.0 已经不卡死了，这次解决的是另外四个问题。

### 1.1 `写入 SD 卡失败（0x00307202）` —— 这是硬 bug，已定位到具体原因

查 switchbrew 的错误码表：

```
0x307202  6201  Error: OpenMode_AllowAppend is required for implicit extension
                of file size by WriteFile().
```

也就是说：**当 `WriteFile` 要「扩大文件长度」时，文件必须以 `FsOpenMode_Append` 打开**。
`Append` 这一位在 Nintendo 的 FS 里叫 **`OpenMode_AllowAppend`**（允许隐式扩大），
不是「只能追加」—— libnx 自己的 `fsdev`（devoptab 实现）就是这么开的：

```c
// libnx/nx/source/runtime/devices/fs_dev.c
case O_WRONLY: fsdev_flags |= FsOpenMode_Write | FsOpenMode_Append;   break;
case O_RDWR:   fsdev_flags |= (FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append); break;
```

而本程序之前只开了 `FsOpenMode_Write`。修复做了两层：

1. `fsx::createAndOpenFile()` / `fsx::writeWholeFile()` 一律改成
   **`FsOpenMode_Write | FsOpenMode_Append`**；
2. 下载时若能从 `Content-Length` 得知总长度（且 ≤ 256MB），
   **先把文件长度定好**再写 —— 这样所有写入都落在长度之内，`AllowAppend` 那条路根本不会走到，
   顺便也省掉 FAT32 上逐块扩簇的开销。

### 1.2 新增：`url.txt` 的 `img:` —— 启动自动加载网络图片

`url.txt` 里新增一项，逗号分隔（中英文逗号都认）：

```
{
    updata:   https://example.com/version.txt ,
    download: https://example.com/app.nro ,
    img:      https://example.com/a.jpg, https://example.com/b.jpg
}
```

- 每一项**可以写 http(s) 链接**（程序自动下载并缓存到 `img/` 目录），
  **也可以写已经放在 SD 卡上的文件名**（按「原文 → 项目文件夹 → SD 根目录」顺序找）。
- 启动后自动开始：**本地已有缓存的直接加载（不联网）**，缺的才逐张下载；先加载完界面再后台补图。
- 缓存文件名是 `<序号>-<原名>`，例如 `img/1-a.jpg`。带序号是为了：
  ① 不同图片重名也不会互相覆盖；② 改了 `url.txt` 里的顺序/内容后文件名随之变化，**不会读到旧缓存**。
- 要重新下载某张图：把 `img/` 里对应的文件删掉再启动即可。
- 最多加载 6 张（显存有限），多出来的会被忽略并写进日志。
- 图片位固定 300px 高、`FIT` 缩放居中、**不可聚焦**（不会在导航里被选中）；
  没加载成功的图片位会完全收起，不留空白。

> 依赖：borealis 内置的 nanovg 走 `stb_image`，**JPEG 与 PNG 都支持**（已确认 `STBI_NO_JPEG` 未生效）。

### 1.3 那条「可以选中的横线」

它是**取消按钮**，两个原因叠在一起：

- `borealis` 判定「视图能不能被聚焦」的唯一依据是 `getDefaultFocus()` 是否返回非空，
  而 `Button` 永远返回自身 → 任务结束后那个禁用的取消按钮**仍然可以被选中**；
- 它在 `List` 里没人给它高度（`Dialog` 里是 `Dialog::layout` 显式给的），
  高度为 0 就被画成了一条横线。

修法：取消按钮改成子类，**只有任务进行中才允许聚焦**，并且显式给了一个行高（`List.Item.height`）；
任务结束时用**非动画**方式完全收起（不留空行、更不会留下一条能选中的线）。

另外顺带明确：`Label` / `ProgressDisplay` / `Image` 这些视图在 borealis 里本来就**不可聚焦**
（它们没有重写 `getDefaultFocus()`），所以导航只会停在输入行和按钮上。

### 1.4 底部内容显示不全、滚不下去

根因是 borealis 的滚动**只跟随焦点**：`ScrollView` 的滚动位置由「当前聚焦视图」驱动，
页面最底下如果没有可聚焦的控件，再按「下」也不会滚动 —— 于是「环境：…」那行永远看不全。

修法两步：

1. 页面最底部放一个**可聚焦的锚点按钮**「诊断信息与提示」，按「下」到底时会带动整页滚动，
   底下的内容自然全部可见；
2. 把原来堆在主页面的长文案（操作提示、运行环境、日志路径、完整返回内容）挪进**诊断信息页**，
   主页面因此短了很多，也更清爽。诊断页由按钮触发 `pushView`（属于按键常规路径，符合设计铁律）。

---

## 2. v2.2.0 修过什么（摘要，详见 BUILD-v2.2.0.md）

真机日志显示网络早已成功（`curl_easy_perform = 0`、HTTP 200），卡死点在 UI 收尾。
根因是收尾路径走了 `RepeatingTask → Dialog::close(cb) → menu_animation 回调 → Application::pushView`，
即**在动画回调里嵌套改视图栈**（borealis 的 `show/hide` 每次都 `menu_animation_kill_by_tag` + push 新动画）。
v2.2.0 把这条路径整个删掉：不用 Dialog、不发通知、任务收尾只改 Label 文本；
同时把 worker 终态改成「最后一步置位」，`fsx` 全部公开接口统一加锁，
修掉轮询任务「每帧都跑」，并加了 UI 面包屑日志与轮询心跳。

> **两条必须守住的设计铁律**（写进代码注释里了）：
> 1. 工作线程运行期间/收尾时**绝不改动视图栈**（唯一例外：按键触发的 `pushView`）；
> 2. 页面最底部必须留一个**可聚焦**的控件，否则永远滚不到底。

---

## 3. 目录结构

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
    ├── main.cpp                  启动顺序：romfs → SD 卡 → 网络 → curl → 项目文件夹 → UI
    ├── app_config.{hpp,cpp}      项目文件夹 / url.txt 的生成与宽松解析（含 img: 列表）
    ├── fsx.{hpp,cpp}             libnx FsFileSystem 封装（全部接口串行化、Append 位、预分配）
    ├── logx.{hpp,cpp}            SD 卡日志（多线程安全、带 [UI] 标记、满了保留最近部分）
    ├── netx.{hpp,cpp}            nifm + socket 初始化
    ├── downloader.{hpp,cpp}      libcurl 下载/取文本；pthread 2MiB 栈；终态最后置位
    ├── main_view.{hpp,cpp}       主界面 + 图片位 + 诊断信息页
    ├── file_picker.{hpp,cpp}     选任意 .txt
    └── path_picker.{hpp,cpp}     选保存目录
```

> `source/progress_dialog.*` 自 v2.2.0 起废弃（进度改为页内显示）。
> 由于上传通道（GitHub 网页 `/upload`）只能新增/覆盖、**不能删文件**，
> 流水线里有一句 `rm -f source/progress_dialog.{cpp,hpp}` 保证它们不参与编译。

---

## 4. SD 卡上的文件

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro              程序本体
├── url.txt                        首次启动自动生成（updata / download / img）
├── settings.txt                   界面里改过的链接与保存目录（自动写回）
├── log.txt                        运行日志（每次启动重建；真机排错就看它）
├── update_result.txt              上次检查更新拿到的完整返回内容
├── img/                           网络图片缓存（1-xxx.jpg、2-yyy.jpg …）
└── cacert.pem                     可选：放了它就严格校验 HTTPS 证书
```

`url.txt` 的解析很宽松：`updata`/`update` 都认、`img`/`image`/`images` 都认、`:` 与 `=` 都认、
可加引号、`// # ;` 开头的整行按注释忽略、链接省略协议会自动补 `https://`。
注意 `img:` 的值**不做补协议**（因为它也可能是本地文件名）。

---

## 5. 安装与操作

1. 把 `nx-downloader.nro` 放到 `SD:/switch/nx-downloader/`。
2. 从**游戏图标**启动（按住 <kbd>R</kbd> 打开任意游戏进入 hbmenu）—— 完整内存模式。
   从「相册」启动也能用，但属于 Applet 模式：**程序会禁用系统键盘**（真机实测按 A 调键盘有死机风险），
   此时请用 `X` 或文件图标读 `url.txt`。
3. 首次启动自动创建项目文件夹与 `url.txt`。
4. 操作：
   - **A** 输入链接（最长 100 字符）· **X** 读取 `url.txt` 里对应的值
   - **txt 图标** 选任意 `.txt` 取里面的字段 · **dir 图标** 选下载保存目录（默认 `sdmc:/`）
   - **十字键 / 左摇杆** 移动焦点 · **诊断信息与提示** 看环境/日志/完整返回内容 · **+** 退出

**同名文件会被直接覆盖**（写入侧是「先删再建」），界面上不再弹确认框。

---

## 6. 从源码编译

### 6.1 云端编译（推荐，本机无需装工具链）

推送代码即触发 `.github/workflows/build-nro.yml`：

- 先试 devkitPro 官方脚本装工具链；若 `apt.devkitpro.org` 被 Cloudflare 拦，自动退回
  官方镜像 `devkitpro/devkita64`。
- 编译用 `make -k`，并把所有 `error:` 行写进 Step Summary —— 一次 CI 看到全部编译错误。
- 产物在运行页的 Artifacts 里（`nx-downloader-nro`）。**公开仓库下载 artifact 也必须登录 GitHub。**

### 6.2 本地编译

```bash
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$PATH

git clone --depth=1 --recursive -b master https://github.com/XITRIX/borealis.git borealis
make sync-resources
make -j$(nproc)
```

踩过的关键点：

- **borealis 必须 `-b master`**：默认分支 `moonlight_wiliwili` 结构不同，找不到 `library/borealis.mk`。
- **链接参数用 `curl-config --libs`**：`LIBS := $(CURL_LIBS) -lnx -lm`，传递依赖不会漏。
- **保留 RTTI / 异常**：不要加 `-fno-rtti` / `-fno-exceptions`。
- **`.DEFAULT_GOAL := all`**：`sync-resources` 写在 `all` 前面，不写这句 `make` 会 1 秒结束且不出包。
- **CI 里不要升级工具链**：镜像自带 libnx 4.12.0（满足固件 21.x 的 TLS ABI 要求），
  升级只会把 gcc 从 15 换到 16，平白引入变量。

---

## 7. 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| **写入 SD 卡失败 (0x00307202)** | 已修（v2.3.0）：FS 错误 6201 = 需要 `OpenMode_AllowAppend` 才能扩大文件。若仍出现，说明是别的写入点漏了 —— 把 `log.txt` 发回。 |
| 界面卡死 | v2.2.0 起收尾路径不再动视图栈。把 `log.txt` 发回：最后一行 `[UI]` 面包屑会指出卡在哪个调用（心跳停止＝渲染循环不动了）。 |
| 底部内容看不全 | 已修（v2.3.0）：底部加了可聚焦的「诊断信息与提示」锚点；borealis 的滚动只跟随焦点，底部无可聚焦项就滚不动。 |
| 图片没显示 | 看 `log.txt` 里 `图片 N` 相关的行；确认 `img:` 写对（逗号分隔）、`img/` 目录里有缓存文件或网络可用。最多 6 张。 |
| 启动提示「网络不可用」 | `LibnxError_AlreadyInitialized`（`0x0F59`）按**成功**处理（它恰恰说明 socket 已可用）。其它错误码见 7.1。 |
| 输入框只能输 32 个字符 | 已修：v2.0.0 的 CI 补丁把 `maxStringLength` 覆盖成了 32，现改成整行删除 + 断言。 |
| 按 A 输入就死机 | Applet 模式下调系统键盘有风险。请在 Applet 模式下用 `X` / 文件图标，或按住 R 从游戏图标启动。 |
| 中文显示成方块 | 程序会挂载系统中简字体作为 fallback（日志里记录 `plGetSharedFontByType` 的返回值）；主机缺该字体则无解。 |
| HTTPS 报证书错误 | 设备没有 CA 链时会自动降级为「不校验证书」并在提示里注明；要严格校验就把 `cacert.pem` 放到项目文件夹。 |
| HTTP 4xx/5xx 报「下载失败」 | 刻意的：避免把服务器错误页面当成文件存下来。 |
| 下载很慢 / 进度不动 | 点「取消当前任务」；超时阈值是 15s 连接、60s 低速。 |
| 日志里出现「前文过长已丢弃」 | 正常：日志满 24KB 后只保留最近部分（最后几行最有价值）。 |

### 7.1 libnx / FS 错误码

`Result` 编码为 `(module << 9) | description`：

- `0x00000F59` = module 345 (`Module_Libnx`) + 描述 7 (`LibnxError_AlreadyInitialized`)
  → **不是失败**：`soc:` 只在 `bsdInitialize` 成功后才登记，能拿到它就说明 socket 早就好了。
- `0x00307202` = FS 错误 6201（`OpenMode_AllowAppend is required ...`）→ 见 §1.1。
- 其它错误码：查 switchbrew 的 Error codes 页面，或 `libnx/nx/include/switch/result.h`。

---

## 8. 版本历史

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 单行下载器：一个链接输入框 + 保存目录 + 下载按钮 |
| 2.0.0 | 双行结构：上行检查更新、下行下载；启动自动建项目文件夹与 `url.txt`；`X` 快捷读取；两行各有 txt 图标；左摇杆可导航 |
| 2.1.0 | 修输入框上限被 CI 补丁覆盖成 32；补 `nifmInitialize`；Applet 模式禁用系统键盘；新增 `log.txt`；SD 卡 I/O 串行化；去掉每帧文字测量 |
| 2.1.1 | `AlreadyInitialized` 按成功处理（网络不再被误禁用）；去掉未导出的 `bsdSocket` 符号；CI 失败时也保证生成 Step Summary |
| 2.2.0 | 移除收尾路径上的全部动画/视图栈操作（对话框、通知、嵌套 pushView）→ 进度与结果改为页内显示；worker 终态最后置位；`fsx` 全部接口加锁；修轮询任务每帧都跑；新增面包屑与心跳日志 |
| **2.3.0** | **修「写入 SD 卡失败 0x00307202」**（`FsOpenMode_Write \| FsOpenMode_Append` + 按 Content-Length 预分配）；**新增 `img:` 启动自动加载网络图片**（本地缓存优先、最多 6 张）；取消按钮改为「仅任务中可聚焦」并在空闲时完全收起；新增底部锚点按钮与诊断信息页，修掉「底部内容看不全」 |
