<img align="left" width="100" height="100" src="doc/firestorm_256.png" alt="Logo of Firestorm viewer"/>

**[Firestorm](https://www.firestormviewer.org) is a free client for 3D virtual worlds such as Second Life and various OpenSim worlds where users can create, connect and chat with others from around the world.**

# Firestorm by:zmer 自定义版

> [!TIP]
> **🙋 我是萌新，请加我好友！** 欢迎在 Second Life 里一起玩、交流和测试这些功能。

> [!IMPORTANT]
> 这是基于 Firestorm Viewer 制作的个人自定义分支，并非 Firestorm 官方发行版。原项目及相关商标归其各自所有者。

## 本分支新增功能

### 物品栏本地中文标签

- 可以为文件夹和普通物品添加本地中文标签。
- 中文标签显示在英文原名上方，并优先保证中文完整可见。
- 标签按照物品 UUID 关联，不修改 Second Life 服务器保存的真实名称。
- 标签只保存在本地客户端，右键菜单已加入“编辑本地标签…”入口。
- 支持可自定义快捷键；默认是 `X`，留空即可禁用。

### 发送消息翻译

- 附近聊天和私聊/IM 均可在发送前自动翻译。
- 支持普通翻译 API 和兼容 OpenAI `/v1/chat/completions` 的 AI 上下文翻译。
- 可设置目标语言、发送格式、API 地址、模型、上下文数量和自定义提示词。
- 翻译失败时会安全回退，不会吞掉原始消息。

### 脚本弹窗与 HUD 菜单翻译

- 支持翻译 `llDialog` 脚本弹窗、项圈、RLV 装备及 HUD 动态菜单。
- 支持普通翻译和 AI 翻译，并使用本地持久化缓存减少重复请求。
- 中文按钮只负责显示；点击后回传给脚本的仍是原始英文按钮值。
- 支持人工校正单个菜单项，人工结果优先于自动翻译并可持久保存。

### 便签翻译

- 游戏内便签窗口增加“机翻”和“AI 翻译”两个按钮。
- 翻译内容以只读方式显示，不会覆盖或修改原始便签。
- 可以随时切换翻译方式，或再次点击按钮返回原文。

### 中文界面与登录输入修复

- 汉化新增的本地标签、翻译设置和便签翻译界面。
- 调整翻译设置布局，避免 Azure API 地址输入框遮挡右侧设置。
- 登录账号框和密码框禁用中文 IME 组合输入，避免保存的密码状态被误改。
- 不影响附近聊天、私聊等其他输入框正常输入中文。

### Windows 构建

- 当前自定义频道显示为 `Firestorm-by:zmer`。
- Windows 文件名使用兼容形式 `by-zmer`，因为 Windows 文件名不允许使用英文冒号。
- 当前发布目标为 64 位 AVX2 开放版本，不依赖私有 KDU/FMOD 包。

This repository is a customized fork of the Firestorm viewer source code.

## Open Source

Firestorm is a third party viewer derived from the official [Second Life](https://github.com/secondlife/viewer) client. The client codebase has been open source since 2007 and is available under the LGPL license.

## Download

Pre-built versions of the viewer releases for Windows, Mac and Linux can be downloaded from the [official website](https://www.firestormviewer.org/choose-your-platform/).

## Build Instructions

Build instructions for each operating system can be found using the links below and in the official [wiki](https://wiki.firestormviewer.org).

- [Windows](doc/building_windows.md)
- [Mac](doc/building_macos.md)
- [Linux](doc/building_linux.md)

> [!NOTE]
> We do not provide support for compiling the viewer or issues resulting from using a self-compiled viewer. However, there is a self-compilers group within Second Life that can be joined to ask questions related to compiling the viewer: [Firestorm Self Compilers](https://tinyurl.com/firestorm-self-compilers)

## Contribute

Help make Firestorm better! You can get involved with improvements by filing bugs and suggesting enhancements via [JIRA](https://jira.firestormviewer.org) or [creating pull requests](CONTRIBUTING.md).

## Community respect

This section is guided by the [TPV Policy](https://secondlife.com/corporate/third-party-viewers) and the [Second Life Code of Conduct](https://github.com/secondlife/viewer?tab=coc-ov-file).

Firestorm code is made available during ongoing development, with the **master** branch representing the current nightly build. Developers and self-compilers are encouraged to work on their own forks and contribute back via pull requests, as detailed in the [contributing guide](CONTRIBUTING.md).

If you intend to use our code for your own viewer beyond personal use, please only use code from official release branches (for example, `Firestorm_7.1.13`), rather than from pre-release/preview or nightly builds.
