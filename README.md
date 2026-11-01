# Cast n Chill 修改器（macOS Apple Silicon）

《Cast n Chill》（Steam 版）的开源游戏修改器，面向 **macOS Apple Silicon（M 系列芯片）**。功能对齐 Windows 平台主流修改器并有所超出，全部真机验证。

> ⚠️ 仅供本人合法购买的 Steam 副本、单机玩法使用。不修改游戏文件、不绕过 Steam 授权。

## 功能一览

| 分区 | 功能 |
|---|---|
| 现金 | 大数字显示 · +1万/+10万 · 精确设定 |
| 卖鱼 | 一键卖出全部渔获（任何位置，自动落盘） |
| 钓鱼增强 | 不断之线 · 超级投掷 |
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

> 本项目只做 ad-hoc 签名，没有 Apple 公证。从网上下载的 dmg 首次打开「安装组件.command」时，请右键 → 打开；安装脚本会自动去掉组件和 `/Applications/CNCPanel.app` 的隔离标记。

**方式 B（仓库直用）**：直接运行仓库里的 `launcher/cnc`

## 日常使用（三步）

1. 启动 Steam（保持登录）
2. 双击 `~/.castnchill-modifier/cnc`（或 `launcher/cnc`）——自动校验版本、备份存档、注入启动游戏
3. 打开 CNCPanel，等绿点「已连接游戏」即可操作

> 面板在游戏标题画面会显示「启动中」属正常（轮询不干扰加载），**进入存档后自动变绿**。

## 存档编辑器（离线，先退游戏）

```bash
cnc_save.py show   <存档>                              # 查看现状
cnc_save.py cash   <存档> add 50000                    # 加钱（负数为扣钱）
cnc_save.py cash   <存档> set 100000 --current 12345   # 设为指定金额（需给出当前金额）
cnc_save.py unlock <存档> all                          # 全鱼图鉴+全外观（已有的跳过，穿戴不变）
cnc_save.py catch  <存档> arctic-char 150              # 指定鱼入包
cnc_save.py restore <存档> <备份>                       # 回滚（回滚前自动备份当前存档）
```

游戏运行中编辑器会拒绝写入（游戏会覆盖改动），确需强制加 `--force`。

## 命令行直控

面板之外，任何终端都能通过 Unix socket 发命令：`printf 'cash.add 50000\n' | nc -U /tmp/cnc-mod-$(id -u).sock`

| 命令 | 说明 |
|---|---|
| `ping` / `state.get` | 探活 / 总状态（含各开关、倍率） |
| `cash.get` · `cash.add <n>` · `cash.set <n>` | 金钱（单次改动 ≤10 亿） |
| `speed.get` · `speed.set <x>` | 游戏加速 0.25–16× |
| `sell.all` · `sell.mult <x>` · `sell.instant on\|off` | 一键卖鱼 / 售价倍率 1–100× / 即时码头销售 |
| `line.safe` `cast.super` + `on\|off` | 钓鱼增强开关（不断之线 / 超级投掷） |
| `fish.trophy on\|off` | 必出奖杯尺寸（面板未露出） |
| `boat.fast <x>` · `ice.fast <x>` · `reel.fast <x>` | 船速 1–10× / 冰面 1–10× / 收线 1–20× |
| `time.get` · `time.set <分钟>` | 游戏内时间（0–1440） |
| `storage.max` · `save.now` · `ids.dump` | 船存拉满 / 立即存盘 / 导出资产 ID |

游戏更新后没解析到的功能会返回 `unsupported`，其余功能照常可用。

## 故障排查

| 症状 | 处置 |
|---|---|
| 强退后重启卡在启动画面 | 已自动规避（启动器清理窗口恢复状态）；仍卡则删 `~/Library/Logs/DiagnosticReports/Cast*` |
| 游戏秒退无弹窗 | 重启 Steam 客户端（状态机脏了） |
| 面板红点 | 游戏须经 `cnc` 启动器启动才会注入；标题画面显示「启动中」属正常 |

## 技术架构

DYLD_INSERT_LIBRARIES 注入（游戏 ad-hoc 签名无 Hardened Runtime，无需 sudo/重签名）→ IL2CPP 运行时按名解析（零硬编码偏移，扛游戏更新）→ GCD 主队列编组全部托管调用 → Unix socket JSON 协议驱动面板。

| 目录 | 内容 |
|---|---|
| `mod/` | 注入模块 `libcncmod.dylib`（C）：`cnc_resolver` 按名解析 IL2CPP、`cnc_mt` 主线程调度、`cnc_ipc` socket 服务、`cnc_game` 各功能 |
| `panel/` | SwiftUI 面板 `CNCPanel.app`（swiftc 直编，无 Xcode 工程） |
| `launcher/cnc` | 启动器：版本校验、存档备份、注入启动 |
| `save-tools/cnc_save.py` | 离线存档编辑器 |
| `scripts/make_dmg.sh` | 打包 dmg |

## 免责声明

本项目仅供学习研究。使用本修改器产生的任何游戏内后果（成就、云存档等）由使用者自行承担。请勿用于任何多人或商业场景。
