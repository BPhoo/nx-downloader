# NX Downloader —— 构建与部署说明

Nintendo Switch 自制程序（`.nro`）：上行检查更新、下行下载文件，全部基于
**devkitPro/libnx + borealis + libcurl** 实现。

---

## 1. 功能与界面

```
┌──────────────────────────────────────────────────────────────────────┐
│  NX Downloader                                                       │
│  上行检查更新 · 下行下载文件                                          │
├──────────────────────────────────────────────────────────────────────┤
│  更新链接   [ https://example.com/version.txt      ]  [txt]  [ 访问 ] │
│  下载链接   [ https://example.com/app.nro          ]  [txt] [dir] [访问] │
├──────────────────────────────────────────────────────────────────────┤
│  下载保存到：sdmc:/  （点第二行的文件夹图标可更换）                    │
└──────────────────────────────────────────────────────────────────────┘
```

| 控件 | 说明 |
| --- | --- |
| 第一行 输入框 | 检查更新用的链接。按 **A** 打开系统键盘手动输入（**最长 100 字符，无下限**） |
| 第一行 [txt] | 打开 txt 选择器，选中文件后自动填入该文件里的 `updata` |
| 第一行 [访问] | 访问该链接，把**服务器返回内容**显示在结果页（可滚动，B 返回） |
| 第二行 输入框 | 下载用的链接，输入方式同上 |
| 第二行 [txt] | 打开 txt 选择器，选中后自动填入该文件里的 `download` |
| 第二行 [dir] | 选择下载保存目录（默认 **SD 卡根目录 `sdmc:/`**） |
| 第二行 [访问] | 访问该链接并下载文件，带进度条 / 完成提示 / 错误提示 |

### 按键

| 按键 | 作用 |
| --- | --- |
| `A` | 打开系统键盘输入链接；在列表里则是「进入 / 选择」 |
| `X` | **快捷读取** `sdmc:/switch/nx-downloader/url.txt` 里对应的值（第一行读 `updata`，第二行读 `download`） |
| `十字键` / **左摇杆** | 移动焦点（左右在同一行内切换，上下在行之间切换） |
| `B` | 返回上一屏 |
| `+` | 退出程序 |

### 下载反馈
* **进度**：模态对话框里显示百分比 + `已下载 / 总大小`
* **完成提示**：通知横幅 + 弹窗显示最终文件路径
* **错误提示**：弹窗显示具体原因（HTTP 4xx/5xx、证书、空间不足、写盘失败等）
* **取消**：对话框上的「取消」按钮；取消/失败会**删掉半成品文件**，不留损坏文件

---

## 2. 项目结构

```
nx-downloader/
├── Makefile                     devkitPro/libnx 标准模板 + borealis + curl-config
├── icon.jpg                     256×256 JPEG，.nro 的图标
├── .github/workflows/build-nro.yml   GitHub Actions 云编译
├── romfs/                       会被打进 .nro，运行时通过 romfs:/ 访问
│   ├── i18n/en-US/brls.json     底部按键提示等文案
│   ├── icon/folder.png          「选保存目录」按钮的图标（128×128）
│   ├── icon/txt.png             「选 txt 文件」按钮的图标（128×128）
│   └── material/                Material 图标字体
└── source/
    ├── main.cpp                 初始化顺序 / 中文字体 fallback / 主循环
    ├── app_config.hpp/.cpp      项目文件夹 + url.txt 的创建与宽松解析
    ├── downloader.hpp/.cpp      libcurl 工作线程：落盘下载 + 文本拉取（两种模式）
    ├── fsx.hpp/.cpp             libnx FsFileSystem 封装（读写、列目录、路径工具）
    ├── main_view.hpp/.cpp       主界面（双行结构、自绘输入框、结果页）
    ├── file_picker.hpp/.cpp     选择 .txt 文件的浏览视图
    ├── path_picker.hpp/.cpp     选择保存目录的浏览视图
    └── progress_dialog.hpp/.cpp 进度对话框
```

---

## 3. SD 卡上的文件

程序启动时如果发现**项目文件夹不存在就创建**，并在里面生成 `url.txt` 模板：

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro      程序本体
├── url.txt                ← 启动时自动生成（格式见下）
├── settings.txt           界面里改过的链接与保存目录（自动写回）
└── cacert.pem             可选：放了它就严格校验 HTTPS 证书
```

`url.txt` 的内容：

```text
// NX Downloader 配置文件（直接用文本编辑器改就行，改完重启程序生效）
// 第一项：检查更新用的链接，对应界面第一行的「访问」
// 第二项：下载文件用的链接，对应界面第二行的「访问」
// 链接可以省略 https:// ，程序会自动补上
{
    updata:   https://example.com/version.txt ,
    download: https://example.com/app.nro
}
```

解析做得很宽松，下面这些写法都认：

* 键名大小写不敏感；`updata` 与 `update` 等价
* 分隔符 `:` 或 `=` 都行，也可以不写
* 值可以加单/双引号
* `//`、`#`、`;` 开头的整行按注释忽略
* 值里省略协议时自动补 `https://`（所以写 `example.com/a.nro` 也可以）
* 换行、逗号分隔都行；`{` `}` 只是装饰，可以不加

---

## 4. 本地构建（需要 devkitPro）

> ⚠️ 先做网络检查：`pkg.devkitpro.org` 挂在 Cloudflare 后面，部分地区会被**整站 403**，
> 表现是 `dkp-pacman` 死活装不上包。若被拦，请直接看第 5 节走云编译。

```bash
# 在 devkitPro 的 MSYS2 Shell 里执行（不是普通 Git Bash）
dkp-pacman -S --needed switch-dev switch-curl

# 工程根目录下
git clone --depth=1 --recursive -b master https://github.com/XITRIX/borealis.git borealis
make sync-resources          # 把 borealis 的 i18n / material 资源同步进 romfs/
make -j$(nproc)

# 产物
ls -lh nx-downloader.nro
```

拷到 SD 卡：`SD:/switch/nx-downloader/nx-downloader.nro`

在 Switch 上从 **hbmenu** 启动，或**按住 R 键从任意游戏图标**启动（完整内存模式，
Applet 模式下系统键盘可能打不开）。

---

## 5. 装不了 devkitPro？用 GitHub Actions 云编译

本机零安装：把工程推到 GitHub，让 runner 编译，再从 Actions 页面下载 Artifacts。

`.github/workflows/build-nro.yml` 已经处理好这些坑：

| 坑 | 处理方式 |
| --- | --- |
| GitHub runner 访问 `apt.devkitpro.org` 同样被 Cloudflare 拦 | 探活失败时自动退到官方镜像 `devkitpro/devkita64`，编译在容器里跑 |
| `XITRIX/borealis` 默认分支是 `moonlight_wiliwili`（结构不同） | 显式 `git clone -b master` |
| `make` 1 秒结束、没有产物 | Makefile 里 `sync-resources` 排在 `all` 前面，GNU make 会把它当默认目标 → 显式 `.DEFAULT_GOAL := all` |
| borealis(master) 用了新版 libnx 已删除的 `swkbdConfigSetStringLenMaxExt` | CI 里 sed 换成 `swkbdConfigSetStringLenMax` |
| borealis(master) 左摇杆不能导航（源码里只有一句 TODO） | CI 里在 `application.cpp` 的 TODO 处注入左摇杆 → `Application::navigate()`（死区 0.5、180ms 重复、弹窗期间不生效） |

触发方式：push 代码，或到 Actions 页面点 **Run workflow**。编译完在运行的 **Artifacts**
区下载 `nx-downloader-nro.zip`，解压出 `nx-downloader.nro`。

---

## 6. 已知限制与排错

| 现象 | 原因 / 处理 |
| --- | --- |
| 中文显示成方块 | 程序已挂载系统简体中文字体作为 fallback；若主机系统字体缺失则无解 |
| 从「相册」启动时 `A` 打不开键盘 | Applet 模式限制。改用 `X` / 文件图标读 `url.txt`，或按住 R 从游戏图标启动 |
| HTTPS 报证书错误 | 设备上没有 CA 链时，程序会**自动降级为不校验证书**并在完成提示里注明；要严格校验就把 `cacert.pem` 放到项目文件夹 |
| HTTP 404 也提示「下载失败」 | 这是刻意的：避免把服务器的错误页面当成文件存下来 |
| 界面卡在进度框不动 | 网络太慢或服务器无响应，点「取消」；超时/低速阈值是 15s 连接、60s 低速 |
| Windows Defender 报毒 | `.nro` 不是 Windows 可执行文件，正常情况下不会报；打包 zip 分发若被拦，加白名单即可 |
| 点「访问」闪退 | 已修的两个历史崩溃：① 工作线程栈溢出（改用 `pthread` 显式 2 MiB 栈）；② `SocketInitConfig` 字段差异（用 SFINAE 兼容） |

---

## 7. 版本

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 单行下载器：一个链接输入框 + 保存目录 + 下载按钮 |
| **2.0.0** | 双行结构：上行检查更新（显示返回内容）、下行下载（可选目录、进度、完成/错误提示）；启动自动创建项目文件夹与 `url.txt`；`X` 快捷读取；两行各有 txt 选择图标；左摇杆可导航 |
