# Omarchy Screenshot

[English](README.md) | 简体中文

为 Hyprland / Omarchy 编写的 Qt 6 截图与标注工具，支持冻结画面和跨屏选区。

## 功能

- 快速选中窗口、显示器或任意区域，支持跨屏截图和选区调整。
- 用图形、箭头、画笔、文字和编号标注截图，用马赛克遮挡隐私，支持撤销与重做。
- 滚动截取长页面，自动拼接长图，支持继续截图和标注。
- 一键复制或保存截图，也可双击选区快速复制。
- 用 PP-OCRv6 Tiny 识别选区中的文字，一键复制结果。
- 将截图和标注 Pin 到桌面最上层，支持多张贴图、跨显示器拖动，点击右上角 × 关闭。
- 自定义标注颜色和深浅主题，自动记住偏好。
- 支持 42 种语言／地区模式，适配多显示器、不同缩放和旋转屏幕。

## 操作

| 操作 | 结果 |
| --- | --- |
| 悬停后单击 | 选中窗口或显示器 |
| 未标注时按 H / 点击滚动截图图标 | 开始滚动截图；多个窗口时先选择目标 |
| 滚动中单击截图区域空白处 | 停止并打开长图；仍可点击“继续截图”延长 |
| 长图中按 H / 点击继续截图图标 | 继续向下截图，保留已有标注 |
| 长图中按 = / - | 放大 / 缩小 |
| 放大后的长图中，用 V 工具左键拖动内部空白区域 | 平移查看长图内容；边框和八个调整点仍用于调整选区 |
| 长图中滚轮 / Ctrl+滚轮 / 中键拖动 | 平移 / 缩放 / 拖动画面 |
| 拖动 | 自由选区；选中后可移动或调整选区 |
| 在普通截图或长图的选区空白处双击 | 将当前选区和已有标注复制到剪贴板并关闭界面；长图按原始分辨率导出，不受预览缩放或滚动位置影响 |
| 未标注时按 O / 点击 OCR 按钮 | 识别选区文字，复制结果并显示在面板中 |
| V / T / G / B / W | 选区 / 文字 / 矩形马赛克 / 编号标记 / 直线 |
| R / E / A / D | 激活上次选择的矩形 / 圆形 / 箭头（含直线） / 画笔（含荧光笔）样式；点击主按钮展开该组图标栏 |
| Shift+B | 聚光灯 |
| Shift+R / Shift+D / Shift+E | 圆角矩形 / 实心矩形 / 实心椭圆 |
| Shift+A / Shift+W | 弯曲箭头 / 双向弯曲箭头 |
| Q / 工具栏左侧颜色按钮 | 选择预设色，或拖动调色盘选色；自动保存 |
| 调色盘顶部太阳 / 月亮图标 | 切换浅色 / 深色工具栏；自动保存 |
| V 模式下方向键 / Shift+方向键 | 对应边框扩展 / 收缩 1px；长按连续调整 |
| 文字工具内单击 | 输入文字；Shift+Enter 换行，Enter 或点到别处确认 |
| 文字工具内按 Alt | 确认已输入文字，返回上一个工具 |
| Z | 撤销上一步标注 |
| X | 重做已撤销的标注 |
| P / 图钉按钮 | 将选区和标注贴到桌面，退出截图界面；长图完整保留，缩小到屏幕内显示 |
| 拖动贴图 / 点击贴图右上角 × | 跨显示器移动 / 关闭当前贴图；双击贴图不执行操作 |
| C | 复制截图并退出 |
| S | 保存截图到图片目录（设置 `OMARCHY_SCREENSHOT_DIR` 时保存到该目录）并退出 |
| Esc / 右键 | 退出 |

窗口截图包含选区内当时可见的像素，包括其他窗口的遮挡部分。

普通截图和滚动截图都排除鼠标光标，包括取帧时切换形状的软件光标。取帧前会暂时禁止合成器绘制光标，并等待约 550 ms 让隐藏状态生效；完成或失败后恢复原状态，不改动配置文件。

贴图只在当前会话保留，不自动保存或复制。再次截图时会临时隐藏已有贴图，截图结束后恢复。有贴图时进程保持运行；未启用常驻模式时，关闭最后一张贴图后退出。

## OCR

选中区域后按 `O` 或点击 OCR 按钮，程序会在后台识别选区文字。识别读取原始截图像素，不包含标注；完成后自动复制到剪贴板，并在面板中显示，可再次选择或复制。按 Esc 关闭面板，再按一次退出截图。

OCR 使用 PP-OCRv6 Tiny 和内置的 RapidOCRLite C++ 引擎。模型支持简体中文、繁体中文、英文和拉丁字母语言。日文、韩文、西里尔文、阿拉伯文、希伯来文、泰文等需要其他识别模型。

马赛克和聚光灯标注不会阻止 OCR 读取文字，因为 OCR 读取的是原始截图像素。如果文字敏感，请先识别再标注。

暂不支持长截图 OCR。OCR 只对普通选区可用。

## 界面语言

支持 42 种语言／地区模式，默认跟随系统界面语言；键盘布局不影响界面语言。可单独指定：

```sh
omarchy-screenshot --language zh_CN
OMARCHY_SCREENSHOT_LANGUAGE=ja omarchy-screenshot
```

优先级：`--language` > `OMARCHY_SCREENSHOT_LANGUAGE` > 系统语言（`LANGUAGE`、`LC_ALL`、`LC_MESSAGES`、`LANG`）。启动时生效，支持 `pt-BR`、`zh-Hant` 等标签，无匹配翻译时回退到英语；快捷键不变。完整语言列表见 [translations/](translations/)，字体建议安装 `noto-fonts` 和 `noto-fonts-cjk`。

翻译为 Qt Linguist `.ts` 文件，构建时内嵌到程序。修改界面文字后，运行 `cmake --build build --target update_translations` 并补齐翻译；保留 `%1` 占位符、程序名和快捷键。

## 构建与运行

需要 Hyprland Wayland 会话，支持 `x86_64` 和 `aarch64`。在 Omarchy 上从 AUR 安装：

```sh
omarchy pkg aur add omarchy-screenshot
```

在 `~/.config/hypr/bindings.lua` 中设置截图快捷键：`Ctrl+Alt+A` 立即截图，`Ctrl+Alt+Shift+A` 延时 5 秒截图：

```lua
hl.unbind("CTRL + ALT + A")
o.bind("CTRL + ALT + A", "Omarchy Screenshot", "omarchy-screenshot")
hl.unbind("CTRL + ALT + SHIFT + A")
o.bind("CTRL + ALT + SHIFT + A", "Screenshot after 5 seconds", "omarchy-screenshot --delay 5")
```

延时快捷键会在当前屏幕中央显示电影开拍风格的圆环倒计时，从 5 倒数到 1；等待期间可以切换窗口或展开菜单，提示不会抢占键盘或鼠标。倒计时结束后，提示先完全淡出并关闭，再抓取画面并显示选区界面，截图不会包含倒计时。也可以运行 `omarchy-screenshot --delay 5`，`--delay` 支持 0–60 秒，默认 0 表示立即截图。

从源码构建需要 `cmake`、`gcc`、`pkgconf`、`qt6-base`、`qt6-declarative`、`qt6-wayland`、`qt6-tools`、`layer-shell-qt`、`wayland`、`wayland-protocols`、`grim`、`wl-clipboard` 和 `hyprland`。默认测试还需要 `python`，可用 `-DBUILD_TESTING=OFF` 关闭。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/omarchy-screenshot
```

配置阶段会获取 RapidOCRLite，并把 PP-OCRv6 Tiny 模型下载到构建目录。离线构建时，可提前解压 RapidOCRLite 源码，并指定模型目录：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_RAPIDOCRLITE=/path/to/RapidOCRLite \
  -DOMARCHY_SCREENSHOT_OCR_MODEL_DIR=/path/to/models
```

本地安装（同时配置常驻模式所需的 service 路径）：

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local -DSYSTEMD_USER_UNIT_DIR=$HOME/.local/share/systemd/user
cmake --build build -j
cmake --install build --component omarchy-screenshot
```

### Omarchy 栏插件

仓库也提供 Quickshell 栏按钮，插件 ID 为 `io.github.manateelazycat.screenshot`。需要先安装原生截图程序；标准插件安装只安装栏按钮，不会编译或安装 Qt 程序。5 秒倒计时要求原生程序版本 0.1.12 或更新版本。

```sh
omarchy pkg aur add omarchy-screenshot
omarchy plugin add https://github.com/manateelazycat/omarchy-screenshot --enable
```

左键点击相机按钮立即截图，右键点击显示 5 秒圆环倒计时。插件从 `PATH` 启动 `omarchy-screenshot`；使用本地构建时，可在栏配置中的对应条目设置 `"command": "/home/yourname/.local/bin/omarchy-screenshot"`。安装栏按钮不会修改 Hyprland 快捷键，快捷键按上方示例手动配置。

卸载栏按钮：`omarchy plugin remove io.github.manateelazycat.screenshot`。需要同时卸载 AUR 程序时，运行 `omarchy pkg drop omarchy-screenshot`。从源码安装到 `~/.local` 时，先用 `systemctl --user disable --now omarchy-screenshot.socket` 和 `systemctl --user stop omarchy-screenshot.service` 停止可选的常驻进程，再删除 `~/.local/bin/omarchy-screenshot` 及安装的 `omarchy-screenshot.service`、`omarchy-screenshot.socket` 用户服务文件；从 `~/.config/hypr/bindings.lua` 移除自己添加的快捷键。

### 测试

`ctest --test-dir build --output-on-failure` 无需桌面会话，可检查翻译、滚动拼接、光标隐藏与恢复、常驻进程、OCR 及不同缩放下的图像采样。在 Hyprland 中运行 `./build/cursor-live-test` 可验证鼠标跨屏移动、切换三种光标时的普通截图和滚动取帧；测试期间各屏会短暂显示纯色画面。以下自检可用 `./build/omarchy-screenshot` 加对应参数运行：

| 参数 | 检查内容 |
| --- | --- |
| `--self-test` | 跨屏选区、撤销／重做和编号；需至少两块显示器 |
| `--ui-self-test --language he` | 工具栏、主题、标注、导出和翻译排版；使用临时设置 |
| `--scroll-stitch-test` | 滚动拼接 |
| `--scroll-ui-self-test` | 长图界面 |
| `--pin-ui-self-test` | 贴图导出、真实跨屏拖动、多张贴图、截图排除贴图和关闭按钮；需至少两块显示器 |

### 常驻模式

默认关闭。开启后复用 Qt 和 GPU 初始化，减少每次截图的启动开销；每次取帧仍需等待光标隐藏生效。空闲 10 分钟后自动退出，运行时约占 100 MB 内存和 320 MB 显存。

```sh
systemctl --user enable --now omarchy-screenshot.socket   # 开启
systemctl --user disable --now omarchy-screenshot.socket  # 关闭并停止后台进程
```

无 systemd 时可将 `omarchy-screenshot --daemon` 加入会话自启动。快捷键无需修改；仅无参数启动转交后台进程，`--language`、`--delay` 和自测独立运行。

运行 `systemctl --user edit omarchy-screenshot.service` 可修改空闲超时（秒，`0` 表示不退出），保留实际安装路径：

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/omarchy-screenshot --daemon --idle-timeout 1800
```

Omarchy 的覆盖层淡入可能额外延迟最多 400 ms。要立即显示，在 `~/.config/hypr/hyprland.lua` 中加入：

```lua
hl.layer_rule({ match = { namespace = "^omarchy-screenshot$" }, no_anim = true, animation = "none" })
```

### AUR 发布

[PKGBUILD](PKGBUILD) 定义软件包；推送 `v主版本.次版本.修订号` 标签后，[GitHub Actions](.github/workflows/publish-aur.yml) 自动更新版本、校验和并提交到 AUR。首次发布需配置 `AUR_USERNAME`、`AUR_EMAIL`、`AUR_SSH_PRIVATE_KEY` Secrets，并将公钥添加到 AUR 账户。

## 许可证

Copyright (C) 2026 Andy Stewart。源码使用 [GPL-3.0-only](LICENSE)；随附的[虚拟指针协议](protocols/wlr-virtual-pointer-unstable-v1.xml)使用 MIT 许可证；RapidOCRLite 及其 OCR 组件使用 Apache-2.0 和 BSD-3-Clause 许可证。
