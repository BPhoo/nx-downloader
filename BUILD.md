# NX Downloader v2.7.0 — 交付说明

产物：`nx-downloader.nro`
部署：SD 卡 `sdmc:/switch/nx-downloader/nx-downloader.nro`（与 `url.txt` 同一目录）

---

## 本次修的三件事（都来自真机日志）

### ① 图片加载失败：`SSL connect error`

日志里的关键一行：

```
curl_easy_perform(第1次, 校验证书=1) = 35 (SSL connect error)
```

只有 **1 次**尝试 —— 也就是说**降级重试根本没触发**。原因在我的判定函数里：

```cpp
// 旧（v2.6.0）
bool isCertError(CURLcode code)
{
    return code == CURLE_PEER_FAILED_VERIFICATION ||   // 60
           code == CURLE_SSL_CACERT_BADFILE ||         // 77
           code == CURLE_SSL_CERTPROBLEM;              // 58
}
```

`CURLE_SSL_CONNECT_ERROR`（**35**）不在里面，而 Steam 的 CDN（Fastly）在 Switch 的
mbedTLS 上正是报 35。同一个日志里国内网盘报的是 60，所以那次降级生效了、能下下来，
图片那几张却一次就放弃 —— 差别就在错误码。

**现在的做法：四档降级，SSL 类错误一律重试**

| 档位 | 配置 | 日志里显示为 |
| --- | --- | --- |
| ① | 校验 HTTPS 证书 | `第1/4次, 校验证书` |
| ② | 关掉证书校验 | `第2/4次, 不校验证书` |
| ③ | 不校验 + HTTP/1.1 + **只用 TLS1.2** | `第3/4次, 不校验+HTTP1.1+仅TLS1.2` |
| ④ | 不校验 + HTTP/1.1 + **只用 TLS1.0** | `第4/4次, 不校验+HTTP1.1+仅TLS1.0` |

**为什么要把 TLS 版本「钉死」而不是只设最低版本**（这是 v2.7.1 的关键修正）：
`CURLOPT_SSLVERSION = CURL_SSLVERSION_TLSv1_2` 在 libcurl 里的含义是
「**最低** 1.2」，**仍然允许协商到 1.3**。而 switch-curl 走 mbedTLS 2.x，
对 TLS1.3 的支持不完整 —— 握手一旦往 1.3 走就失败，libcurl 只报一句 35。
所以第 ③④ 档用 `min | max`（`CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2`）
把版本上限也锁住。

握手/连接失败时日志会多打两行，用于定性：

```
| curl 原文：...（mbedTLS 的原始错误）
+ OS errno=0，对端 IP=…，SSL verify=…
```

`OS errno=0` 说明是协议层失败（不是被掐断）；`104/110/113` 说明连接被重置或超时；
`对端 IP` 有值说明 DNS 和 TCP 都通，问题出在 TLS 之后。

重试只针对 SSL 类错误（`isSslRetryable`：35/58/59/60/77/80/82/83/90/91/53/54），
每次降档前会删掉半成品文件；用户取消、写盘失败、或错误不属于 SSL 类时立刻停止。

### ② 文字模式任按何键都没反应（只有 HOME 有效）

两个独立原因，都是我自己写的：

**a) 主循环用了 `appletMainLoop()`。**
libnx 的源码里它长这样：

```c
bool appletMainLoop(void) {
    u32 msg = 0;
    if (R_FAILED(appletGetMessage(&msg))) return true;   // ← eventWait(..., 0) 无限等待
    return appletProcessMessage(msg);
}
```

`eventWait` 的 `timeout=0` 表示**无限等待**，而只有**系统消息**（HOME、焦点变化、
退出请求）能唤醒它，**按键不会**。放在 `while (appletMainLoop())` 里 = 整个程序
卡在那儿等消息 —— 表现就是「任何按键都没反应，而 HOME 能退（它会产生消息）」。
现在主循环改成纯 `padUpdate` 轮询。

**b) 只调了 `padInitializeDefault()`，缺了前两步。** 按键状态全在 hid 的共享内存里，
必须按顺序做：

```c
hidInitialize();                                  // ① 建 hid 服务 + 输入共享内存
padConfigureInput(1, HidNpadStyleSet_NpadStandard); // ② 告诉系统「我支持这些手柄」
padInitializeDefault(&g_pad);                     // ③ 读 No1 + Handheld（掌机模式）
```

少了 ②，系统给的 `style_set` 恒为 0，而 `padUpdate()` 里有
`if (style_set == 0) continue;` —— 所有手柄都被跳过，按键恒为 0，但程序看起来一切正常。

菜单里新增一行实时诊断，一眼就能判定到哪一步：

```
input: style=0x3 active=0x1 handheld buttons=0x8000000000000000
```

- `style=0x0` → 缺 ②（系统没把本程序识别成支持手柄）
- `active=0x0` → 手柄没连上或被系统收走
- `buttons=0x…` → 有值就说明按键**已经读到了**

### ③ 点「诊断信息与提示」直接报错退出

旧版是新建一个 `DetailsView`（List 子树）再 `pushView`，里面一次性塞 5 个 Label、
其中一个放 `update_result.txt` 的前 6000 字节。真机一点就退出。

我不继续猜是哪一层的问题，**把整条路去掉**：

- 诊断正文改成用**主页面已有的 Label 就地展开/收起**
  （`expand`/`collapse` 是图片位一直在用、已验证的机制，不涉及视图栈、不新建视图树）
- 正文压到几百字节并拆成两段，彻底没有超长文本
- 展开 / 收起 / 文本构建每一步都写 `[UI]` 面包屑，万一还有问题，log.txt 最后一行就指明位置
- 完整信息永远在文件里：`log.txt`（全过程）+ `update_result.txt`（返回内容）

---

## url.txt 格式

`sdmc:/switch/nx-downloader/url.txt`（程序启动时不存在会自动生成模板）

```
{
    updata:   https://example.com/version.txt ,
    download: https://example.com/app.nro ,
    img:      https://a.com/1.jpg, https://a.com/2.jpg
}
```

- `updata` / `update` 都认；`:` 或 `=` 都认；`// # ;` 开头的行是注释
- 链接可以省略 `https://`（会自动补上）
- `img` 每项**可以是链接**（自动下载并缓存到 `img/`），**也可以直接写 SD 卡上已有的文件名**
  （按「原文 → 项目文件夹 → SD 根目录」依次查找）。最多 6 张
- 图片缓存名带序号（`1-xxx.jpg`），改了顺序或换图不会读到旧缓存；想重下就删掉 `img/` 里对应文件

---

## 界面模式

| 启动方式 | 界面 |
| --- | --- |
| **按住 R → 游戏图标**（完整内存模式） | 图形界面，全部功能 |
| **从相册启动**（Applet 模式） | 文字界面（`Applet` 模式下图形界面会整机死机） |
| 放 `console.txt` 到项目目录 | 强制文字界面（完整内存模式下也能看） |
| 放 `gui.txt` 到项目目录 | 强制图形界面 |

文字模式按键：`A` 检查更新 · `B` 下载 · `X` 重读 url.txt · `Y` SD 卡 I/O 自检 · `+` 退出
（HOME 由系统处理，不需要程序响应）

图形界面：`A` 输入链接 · `X` 读 url.txt · txt 图标选文件 · 目录图标选保存目录 ·
十字键/左摇杆移动 · 底部「诊断信息与提示（A 展开/收起）」

---

## 运行期文件

```
sdmc:/switch/nx-downloader/
├── nx-downloader.nro      程序本体
├── url.txt                配置（updata / download / img）
├── settings.txt           界面改过的链接与保存目录（自动写回）
├── update_result.txt      上次「检查更新」的返回内容
├── img/                   图片缓存
├── log.txt                启动重建，全过程诊断（排错第一手资料）
├── console.txt            可选：强制文字界面
└── gui.txt                可选：强制图形界面
```

---

## 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| 图片加载不出来 | 底部按钮展开诊断区，看每一张的原因。v2.7.1 已做到**四档**降级（含把 TLS 版本钉死在 1.2 / 1.0）。仍失败就看 `log.txt` 里的 `curl 原文` 与 `OS errno`：**只有某个域名失败、其他链接正常**（例如 Steam 的 CDN 不通而网盘通）→ 多半是该服务器在当前网络下不可达或被 Switch 的 TLS 库排斥。**换一个图片地址，或先把图片拷到 SD 卡、用 `img: 文件名` 引用** |
| 文字模式按键无反应 | 看菜单里 `input: style=… active=… buttons=…`：`style=0x0` 说明系统没注册手柄，`active=0x0` 说明手柄没连上 |
| 启动就提示网络失败 | 错误码会直接显示。`0x00000F59` = `LibnxError_AlreadyInitialized`，实际是**已经初始化好了**（按成功处理），不是故障 |
| 下载报 `0x00307202` | 已修（v2.3.0）：`FsOpenMode_Append` 未加导致「写入时扩大文件」被拒绝 |
| 按 A 输入就死机 | Applet 模式下系统键盘有此风险，v2.1.0 起已在该模式禁用键盘，改用 X / 文件图标 |
| 中文显示成方块 | 程序挂载系统简体中文字体作为 fallback；主机系统字体缺失则无解 |
| Windows Defender 报毒 | `.nro` 不是 Windows 可执行文件，正常不会报 |

---

## 版本

| 版本 | 说明 |
| --- | --- |
| 2.0.0 | 双行结构（上行检查更新 / 下行下载）、启动建目录与 url.txt、X 快捷读取 |
| 2.1.x | 补齐 `nifmInitialize`、修输入框上限被 CI 补丁覆盖成 32、Applet 模式禁用键盘、新增 log.txt |
| 2.2.0 | 重写任务收尾路径（不再在动画回调里改视图栈） |
| 2.3.0 | 修 `写入SD卡失败 0x00307202`、新增 `img:` 图片、修底部滚不动 |
| 2.4.0 | 弃用 `brls::Image` 改自绘（修「已加载却全白」）、Applet 模式处置 |
| 2.5.0 | 构造函数彻底不碰 SD 卡、日志每行 FS 操作 6 次→1 次 |
| 2.6.0 | Applet 模式改文字界面 + SD 卡 I/O 自检 |
| **2.7.0** | SSL 降级重试（修图片失败）· 文字模式按键修复（去掉阻塞的 `appletMainLoop` + 补 hid 初始化）· 诊断信息改就地展开（修点击报错退出） |
| **2.7.1** | 降级扩到**四档**并把 TLS 版本**钉死**（`min｜max`，不再只设最低 1.2 —— 否则仍会协商到 mbedTLS 支持不全的 TLS1.3）；失败时打出 `curl 原文` + `OS errno` + `对端 IP` + `SSL verify` 用于定性；重试的超时缩短为 8 秒 |
| **2.7.2** | ① **一张失败不再中断整批**（旧逻辑第 1 张失败后 2/3/4 张压根不试，等于把能显示的也丢了）；② 底层诊断**直接显示在界面上**（curl 原文 / OS errno / 对端 IP），截图即可，不用拔卡取 `log.txt`；③ 诊断区每次展开都重新生成，永远是最新的 |
| **2.7.3** | ★ 修 v2.7.2 引入的**死循环**：失败的那一格没清 `pending`，`startNextPendingImage()` 下一轮又挑中它 → 同一张图无限重下（真机把 `log.txt` 刷到写满上限）。现在收尾时立刻清 `pending`，并加了「批量步数上限」保险丝；另外失败 2 张后放弃剩余，避免每次启动都要跑满 4 档 × 4 张 |

### v2.7.3 的一条重要结论：Steam 的 CDN 在 Switch 上取不到

真机日志（v2.7.1/2.7.2）给出的定性依据：

```
curl_easy_perform(第1~4/4次, …) = 35 (SSL connect error)
  | curl 原文：(curl 未给出文本)
  + OS errno=0，对端 IP=199.232.115.52，SSL verify=0
```

- `对端 IP` 有值 → **DNS 和 TCP 都通**，连上了 Fastly 的服务器
- `OS errno=0` → 不是被掐断/超时，是**协议层失败**
- 四档全失败（含把 TLS 钉死到 1.2 和 1.0）→ 不是版本协商问题

同一份日志里 `pan.qzyun.net` 第 ② 档就成功，说明网络和 libcurl 都正常。
⇒ **Switch 的 TLS 库（mbedTLS 2.x）与这个 CDN 握手不兼容**，再调 curl 参数没用。

**可行的两条出路**：
1. **换图片地址** —— 用对 Switch 友好的图源（你的网盘直链实测可通）。
2. **用本地文件** —— 把图片拷到 SD 卡，在 `url.txt` 里写文件名：
   ```
   img: 1.jpg, 2.jpg
   ```
   程序会按「项目文件夹 → SD 根目录」查找，**完全不走网络**，这条是稳的。
