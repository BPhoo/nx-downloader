# NX Downloader —— 构建说明（生成 .nro）

本文档说明如何把本工程编译成 Switch 可运行的 `nx-downloader.nro`。

---

## 0. 工程结构

```
nx-downloader/
├── Makefile                 构建入口（devkitPro 标准模板 + borealis + curl）
├── icon.jpg                 256x256 应用图标，会被打包进 .nro
├── source/
│   ├── main.cpp             入口：初始化各子系统、挂中文字体、跑 UI 主循环
│   ├── main_view.hpp/.cpp   主界面：输入框 / 目录按钮 / 下载按钮 + 完成提示
│   ├── path_picker.hpp/.cpp 目录选择器（浏览 SD 卡子目录）
│   ├── progress_dialog.hpp/.cpp 下载进度对话框（进度条 + 取消）
│   ├── downloader.hpp/.cpp  下载引擎（libcurl + 工作线程）
│   └── fsx.hpp/.cpp         SD 卡文件系统与路径工具
└── romfs/                   会被打进 .nro 的资源
    ├── icon/folder.png      「选择目录」按钮上的文件夹图标
    ├── i18n/en-US/brls.json borealis 界面文案（OK/Back/Exit）
    └── material/            图标字体（borealis 运行时加载）
```

---

## 1. 前置环境：devkitPro

Switch 自制程序必须用 **devkitPro** 的工具链（`aarch64-none-elf-gcc`），
不能直接用系统里的 MinGW / MSVC / 普通 gcc。

### 1.0 ⚠️ 先做一个网络前置检查（很多人卡在这里）

devkitPro 的安装器和 `dkp-pacman` 都要从 **`https://pkg.devkitpro.org`** 下载包。
这个站点挂在 Cloudflare 后面，**会按地区/ASN 直接返回 403**
（devkitPro 官方 issue [devkitPro/pacman#31](https://github.com/devkitPro/pacman/issues/31)
确认过，属于已知的上游地域封锁，不是本机网络故障）。

先测一下通不通：

```bash
curl -o /dev/null -w "%{http_code}\n" https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
```

- 返回 `200` → 正常，直接往下走。
- 返回 `403` → **先解决网络再装**，否则安装器会在「下载包」那一步失败：
  - 挂代理 / 开 VPN（中国大陆用户最常见的情况）；
  - 或换个出口网络（比如手机热点换另一家运营商）后再复测；
  - 装了代理后，让 pacman 走代理（见 1.3）；
  - **装不了或不想折腾 → 直接跳到 §10「云编译」，本机一行命令都不用装。**

### 1.1 安装

1. 从 <https://github.com/devkitPro/installer/releases> 下载 **Windows 安装器**并安装，
   默认装到 `C:\devkitPro`。
   安装器是联网安装的（本体只有 190 KB，包是安装时下载），记得勾选
   **"Switch Development"**（devkitA64 + libnx）。
2. 安装完成后，从开始菜单打开 **"devkitPro MSYS2 Shell"**
   （或直接运行 `C:\devkitPro\msys2.exe`）。
   **后续所有命令都在这个 shell 里执行**，否则 `DEVKITPRO` 变量不存在，`make` 会直接报错。

### 1.2 验证安装

```bash
echo "$DEVKITPRO"          # 应输出 /opt/devkitpro
which aarch64-none-elf-gcc # 应输出 /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc
which curl-config          # 应输出 /opt/devkitpro/portlibs/switch/bin/curl-config
```

### 1.3 让 pacman 走代理（仅当 1.0 返回 403 且你有代理时）

pacman 底层是 libcurl，直接认环境变量。假设本地 HTTP 代理在 `127.0.0.1:7890`：

```bash
export http_proxy=http://127.0.0.1:7890
export https_proxy=http://127.0.0.1:7890
```

SOCKS5 也可以：`export all_proxy=socks5h://127.0.0.1:1080`。
想长期生效就写进 `~/.bashrc`，或在 `/etc/pacman.conf` 里用 `XferCommand` 指定。

## 2. 安装依赖包

在 devkitPro MSYS2 Shell 里执行：

```bash
dkp-pacman -S --needed switch-dev switch-curl
```

- `switch-dev`：libnx 与 `libnx/switch_rules`（Makefile 依赖它）
- `switch-curl`：Switch 版 libcurl（底层是 mbedTLS，HTTPS 全靠它）

`switch-curl` 会一并装上 `mbedtls`、`zlib`、`libssh2` 等传递依赖。
本工程的 Makefile 用 `curl-config --libs` 自动展开这些依赖，不需要手工写 `-lmbedtls` 之类。

## 3. 准备 borealis

borealis 是界面库，本工程以源码形式引入（不预编译），源码放在工程根目录的 `borealis/`：

```bash
cd /path/to/nx-downloader
git clone --depth=1 --recursive https://github.com/XITRIX/borealis.git borealis
```

> `borealis/` 目录不进版本库也没关系，它是外部依赖。

## 4. 同步运行时资源

borealis 运行时要读翻译文件与图标字体。执行一次即可：

```bash
make sync-resources
```

它会把 `borealis/resources/{i18n,material}` 拷进本工程的 `romfs/`。
（`romfs/icon/folder.png` 是本工程自带的，不受影响。）

## 5. 编译

```bash
make -j$(nproc)
```

成功后在工程根目录得到：

```
nx-downloader.elf    中间文件（可忽略）
nx-downloader.nacp   应用元信息
nx-downloader.nro    ★ 这个就是要拷到 Switch 的文件
```

从头重来：`make clean && make -j$(nproc)`

### `.nro` 是怎么生成的？

Makefile 最终调用 devkitPro 的 `elf2nro`：

```
nx-downloader.elf --nacp=nx-downloader.nacp --icon=icon.jpg --romfsdir=romfs → nx-downloader.nro
```

也就是说 `.nro` = **可执行代码（elf）+ 应用信息（nacp）+ 图标（icon.jpg）+ 资源（romfs/）**。

## 6. 部署到 Switch

1. 用读卡器把 SD 卡插到电脑（或通过 FTP / nxmtp 传输）。
2. 在 SD 卡上建目录 `switch/nx-downloader/`，把 `nx-downloader.nro` 拷进去：

```
SD:/switch/nx-downloader/nx-downloader.nro
```

3. 把卡插回 Switch，进入 **相册（Album）** 或 **hbmenu**，运行 "NX Downloader"。

## 7. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| 安装器走到「下载包」报 `403` / `failed retrieving file` | `pkg.devkitpro.org` 把你的出口 IP 拦了。见 1.0 与 1.3（挂代理或换网络） |
| `Please set DEVKITPRO in your environment` | 没在 devkitPro MSYS2 Shell 里执行 `make` |
| `borealis/library/borealis.mk: No such file` | 没做第 3 步，或目录名不是 `borealis` |
| 图标 / 文件夹图片不显示 | 没做第 4 步 `make sync-resources` |
| 运行后界面中文是方块 | 主机为英文/日文时系统标准字体不含汉字。程序会自动挂载系统内置简体中文字体；若仍异常，可检查日志中的 `简体中文字体加载失败` 提示 |
| **按 A 打不开系统键盘** | Applet 模式（从相册启动）下系统软键盘可能调不起来。见下面第 8 节 |
| 下载报证书错误 | 设备没有 CA 证书链。程序会自动降级为「不校验证书」重试一次并给出提示；如需严格校验，把 `cacert.pem` 放到 `SD:/switch/nx-downloader/` |

## 8. Applet 模式 vs 完整内存模式

从**相册**启动时，程序运行在 **Applet 模式**（内存受限），系统软键盘（swkbd）有可能调起失败。
两种应对方式：

1. **降级输入**：把链接写进文本文件，界面上按 **X** 键读取。
   文件路径：`SD:/switch/nx-downloader/url.txt`（写一行链接即可）。
2. **换成完整内存模式**：在主界面按住 **R** 键不放，再从游戏图标启动程序，
   此时内存充足，系统键盘可正常使用。

程序自己的设置也存放在 `SD:/switch/nx-downloader/settings.txt`，格式：

```
url=https://example.com/file.zip
dir=sdmc:/downloads
```

## 9. 操作速查

| 按键 | 作用 |
| --- | --- |
| A | 在当前控件上确认：输入框 → 打开系统键盘；目录按钮 → 进入目录选择器；下载按钮 → 开始下载 |
| B | 返回上一屏（目录选择器内）／ 关闭可关闭的对话框 |
| X | 在链接输入框上：从 `url.txt` 读取链接 |
| + | 退出程序 |

---

## 10. 装不了 devkitPro 时：用 GitHub Actions 云编译（推荐）

如果你的网络被 `pkg.devkitpro.org` 403 拦住（见 §1.0），**本机不需要装任何工具链**。
把工程推到 GitHub，让 GitHub 的服务器帮你编译，再从 Actions 页面下载 `.nro`。

GitHub 的 runner 在美国/新加坡，不受 Cloudflare 地域封锁影响。
本工程已经自带 workflow：`.github/workflows/build-nro.yml`，打开即用。

### 10.1 步骤

1. 在 <https://github.com/new> 新建一个仓库（**Public 即可，免费额度不限**；
   私有仓库也行，每月 2000 分钟，一次编译只需几分钟）。
2. 把 `nx-downloader/` 里的内容传上去，两种方式任选：
   - **命令行**（本机 git 可用）：
     ```bash
     cd nx-downloader
     git init && git add . && git commit -m "init"
     git branch -M main
     git remote add origin https://github.com/<你的用户名>/<仓库名>.git
     git push -u origin main
     ```
   - **纯网页**：在仓库首页点 "uploading an existing file"，把文件夹拖进去，然后 Commit。
3. push 完成后，打开仓库的 **Actions** 标签页，会看到 `build-nro` 正在跑（约 3~8 分钟）。
4. 跑完后，点进那次运行，在页面底部的 **Artifacts** 里下载 `nx-downloader-nro`
   （里面是 `nx-downloader.nro` 和 `nx-downloader.elf`）。
5. 把 `.nro` 拷到 `SD:/switch/nx-downloader/`（见 §6）。

改了代码后重新 `git push`，Actions 会自动再编一次。

### 10.2 为什么这样可行

workflow 在 `ubuntu-latest` 上执行：

```bash
wget https://apt.devkitpro.org/install-devkitpro-pacman   # devkitPro 官方的 Debian 安装脚本
sudo ./install-devkitpro-pacman
sudo dkp-pacman -S --needed switch-dev switch-curl
git clone --depth=1 --recursive https://github.com/XITRIX/borealis.git borealis
make sync-resources && make -j$(nproc)
```

等价于 §1~§5 的全部步骤，只是跑在别人的 Linux 服务器上。
产物通过 `actions/upload-artifact` 带回给你。

### 10.3 其他备选（都不如云编译省事）

| 方案 | 说明 |
| --- | --- |
| **Cloudflare WARP** | 免费改出口 IP，`winget install --id Cloudflare.Warp`。改完 IP 后本机就能正常装 devkitPro，但它在国内不保证可用，且需要管理员权限、会动系统网络配置 |
| **代理 / VPN** | 挂上后按 §1.3 给 pacman 配 `https_proxy`，本机正常装 |
| **手工拼装工具链** | ARM 官方 Windows 原生 `aarch64-none-elf` 工具链可以下载（约 177 MB），但还要自己编译 libnx 与 switch-tools、自己准备 `make`、自己解决 Switch 版 libcurl，坑很多，**不建议** |
| **国内镜像站** | 已实测：清华/中科大/阿里/腾讯/上交/南大/BFSU 都没有 devkitpro 镜像，别浪费时间 |
