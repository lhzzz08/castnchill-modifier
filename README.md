# Cast n Chill 修改器（macOS Apple Silicon）

《Cast n Chill》（Steam 版）的开源游戏修改器，面向 **macOS Apple Silicon（M 系列芯片）**。功能对齐 Windows 平台主流修改器并有所超出，全部真机验证。

> ⚠️ 仅供本人合法购买的 Steam 副本、单机玩法使用。不修改游戏文件、不绕过 Steam 授权。

## 功能一览

| 分区 | 功能 |
|---|---|
| 现金 | 大数字显示 · +1万/+10万 · 精确设定 |
| 卖鱼 | 一键卖出全部渔获（任何位置，自动落盘） |
| 钓鱼增强 | 秒赢对决（战斗 0.05s）· 即时上钩（咬钩率 100%）· 不断之线 · 无限体力（划船/走路/破冰）· 超级投掷 |
| 速度 | 游戏加速 1–16× · 船速 1–10×（含放置闲逛/码头走路） |
| 存档编辑器 | 离线修改金钱 / 解锁全鱼图鉴 / 全外观（独立工具） |
| 快捷键 | Cmd+Q 退出 · Cmd+W 关窗 · Cmd+M 最小化（位置记忆） |

## 环境要求

- macOS 12+，**Apple Silicon**（M1/M2/M3/M4）
- Xcode Command Line Tools（`xcode-select --install`，提供 clang/swiftc）
- Steam 客户端已安装并登录，拥有《Cast n Chill》

## 从源码构建

```bash
git clone <本仓库地址>
cd castnchill-modifier
make        # 编译注入模块 libcncmod.dylib + 面板 CNCPanel.app
make dmg    # （可选）打包成 CastNChillTrainer.dmg 安装镜像
```

## 安装

**方式 A（推荐）**：`make dmg` → 挂载 dmg → 把 `CNCPanel.app` 拖进「应用程序」→ 双击 dmg 里的「安装组件.command」

**方式 B（仓库直用）**：直接运行仓库里的 `launcher/cnc`

## 日常使用（三步）

1. 启动 Steam（保持登录）
2. 双击 `~/.castnchill-modifier/cnc`（或 `launcher/cnc`）——自动校验版本、备份存档、注入启动游戏
3. 打开 CNCPanel，等绿点「已连接游戏」即可操作

> 面板在游戏标题画面会显示「启动中」属正常（轮询不干扰加载），**进入存档后自动变绿**。

## 存档编辑器（离线，先退游戏）

```bash
cnc_save.py show   <存档>                        # 查看现状
cnc_save.py cash   <存档> set 100000             # 改钱
cnc_save.py unlock <存档> all                    # 全鱼图鉴+全外观
cnc_save.py catch  <存档> arctic-char 150        # 指定鱼入包
cnc_save.py restore <存档> <备份>                 # 回滚
```

## 故障排查

| 症状 | 处置 |
|---|---|
| 强退后重启卡在启动画面 | 已自动规避（启动器清理窗口恢复状态）；仍卡则删 `~/Library/Logs/DiagnosticReports/Cast*` |
| 游戏秒退无弹窗 | 重启 Steam 客户端（状态机脏了） |
| 面板红点 | 游戏须经 `cnc` 启动器启动才会注入；标题画面显示「启动中」属正常 |

## 技术架构

DYLD_INSERT_LIBRARIES 注入（游戏 ad-hoc 签名无 Hardened Runtime，无需 sudo/重签名）→ IL2CPP 运行时按名解析（零硬编码偏移，扛游戏更新）→ GCD 主队列编组全部托管调用 → Unix socket JSON 协议驱动面板。详见 `DESIGN.md`。

## 免责声明

本项目仅供学习研究。使用本修改器产生的任何游戏内后果（成就、云存档等）由使用者自行承担。请勿用于任何多人或商业场景。
