#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cast n Chill 存档编辑器（F20/F18）

存档格式（M3 逆向结论）：
- 纯文本，每行一条事件：`time,code,args...`（文件末尾有无换行以原文件为准，写回时保持）
- 首行 `<固定标识>,0,6` 为创建标识 + 版本条目，不需要动
- 常用条目码：1 Travel / 2 Catch / 3 Sell / 4 Buy / 6 Checkpoint /
  7 Flag / 8 Discovered / 9 Customise / 10 TimeOfDay / 11 CashTip
- 现金 = 重放 卖鱼/买东西/CashTip 等全部条目的结果，离线无法精确算出；
  CashTip 条目 `t,11,x` 的语义与游戏内 TipRusty(x) 一致：现金 -= x
  （所以 x 为负 = 加钱）。离线改钱 = 追加一条 CashTip。
- 无校验和（首行标识跨存档恒定，实测可直接增改行）

用法：
  cnc_save.py show    <journal>
  cnc_save.py cash    <journal> add <amount>
  cnc_save.py cash    <journal> set <amount> --current <当前金额>
  cnc_save.py unlock  <journal> fish|cosmetics|all [--ids game-ids.json]
  cnc_save.py catch   <journal> <fish-id> [size] [--ids game-ids.json]
  cnc_save.py restore <journal> <backup>
所有写操作默认拒绝在游戏运行时执行（会被游戏覆盖），确需强制加 --force。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

IDS_DEFAULT = Path.home() / ".castnchill-modifier/game-ids.json"
CASH_STEP_MAX = 1_000_000_000  # D4：单次金钱改动 ≤10 亿

ENTRY_NAMES = {
    0: "Version", 1: "Travel", 2: "Catch", 3: "Sell", 4: "Buy", 5: "Upgrade",
    6: "Checkpoint", 7: "Flag", 8: "Discovered", 9: "Customise",
    10: "TimeOfDay", 11: "CashTip", 12: "LostFish", 13: "Flotsam", 14: "Restock",
}

# Customise 槽位（实测自真实存档：0=skin 1=outfit 2=hat 4=bobber 5=rod；3=lure 推定）
CUSTOMISE_SLOT = {
    "AnglerSkinData": 0, "AnglerOutfitData": 1, "AnglerHatData": 2,
    "LureData": 3, "RodData": 5,
}


def fields(line):
    """拆一行，返回 (code, 字段列表)；格式不对返回 (None, 字段列表)"""
    f = line.split(",")
    try:
        return int(f[1]), f
    except (IndexError, ValueError):
        return None, f


def load_ids(path):
    p = Path(path) if path else IDS_DEFAULT
    if not p.exists():
        sys.exit(f"[!] ID 清单不存在：{p}\n    先启动带 mod 的游戏执行一次 ids.dump")
    return json.loads(p.read_text(encoding="utf-8"))


class Journal:
    """存档文本 + 原文件是否以换行结尾（写回时保持原样）"""

    def __init__(self, path):
        self.path = Path(path)
        text = self.path.read_text(encoding="utf-8")
        self.trailing_nl = text.endswith("\n")
        body = text[:-1] if self.trailing_nl else text
        self.lines = body.split("\n") if body.strip() else []

    def save(self):
        """先写同目录临时文件再原子替换：中途出错不会留下半截存档"""
        text = "\n".join(self.lines) + ("\n" if self.trailing_nl else "")
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=f".{self.path.name}.")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(text)
            shutil.copymode(self.path, tmp)
            os.replace(tmp, self.path)
        except BaseException:
            os.unlink(tmp)
            raise


def backup(path):
    """备份名唯一：同一秒内多次写入不会互相覆盖（覆盖 = 丢掉更早的备份）"""
    base = Path(path).parent / f"{Path(path).name}.cncbak-{time.strftime('%Y%m%d-%H%M%S')}"
    dst, n = base, 1
    while dst.exists():
        dst = base.with_name(f"{base.name}-{n}")
        n += 1
    shutil.copy2(path, dst)
    return dst


def game_running():
    r = subprocess.run(["pgrep", "-f", "Cast n Chill.app/Contents/MacOS/Cast n Chill"],
                       capture_output=True, text=True)
    return bool(r.stdout.strip())


def discovered(lines):
    out = set()
    for ln in lines[1:]:
        code, f = fields(ln)
        if code == 8 and len(f) >= 3:
            out.add(f[2])
    return out


def cmd_show(args):
    j = Journal(args.journal)
    tip_sum, cosmetics, sells, catches = 0, set(), 0, 0
    print(f"存档：{args.journal}（{len(j.lines)} 行）")
    if j.lines:
        print(f"首行标识：{j.lines[0].split(',')[0][:20]}...")
    for ln in j.lines[1:]:
        code, f = fields(ln)
        if code == 11 and len(f) >= 3:
            try:
                tip_sum += int(f[2])
            except ValueError:
                pass
        elif code == 9 and len(f) >= 5:
            cosmetics.add(tuple(f[2:]))
        elif code == 3:
            sells += 1
        elif code == 2:
            catches += 1
    # CashTip(x) 的效果是 现金 -= x，所以对现金的净影响是 -sum
    print(f"CashTip 对现金的净影响：{-tip_sum:+d}（不含卖鱼/买东西收支，真实现金看游戏内或面板）")
    print(f"渔获 {catches} 次 / 售出 {sells} 次 / 图鉴 {len(discovered(j.lines))} 种")
    print(f"外观条目 {len(cosmetics)} 条（去重后）")


def cmd_cash(args):
    if args.op == "set":
        if args.current is None:
            sys.exit("[!] 离线无法算出当前现金（还取决于卖鱼/买东西记录）。\n"
                     "    请用 --current 给出当前金额（游戏里或面板上看到的数），例如：\n"
                     f"    cnc_save.py cash <存档> set {args.amount} --current 12345")
        target = max(0, min(args.amount, CASH_STEP_MAX))
        delta = target - args.current
        desc = f"{args.current} → {target}"
    else:
        delta = args.amount
        desc = f"{delta:+d}"
    delta = max(-CASH_STEP_MAX, min(delta, CASH_STEP_MAX))
    if delta == 0:
        print("[i] 金额无变化，未写入")
        return
    j = Journal(args.journal)
    bk = backup(args.journal)
    j.lines.append(f"{int(time.time())},11,{-delta}")
    j.save()
    print(f"[✓] 现金 {desc}（追加 CashTip 条目；备份 {bk.name}）")


def cmd_unlock(args):
    ids = load_ids(args.ids)
    j = Journal(args.journal)
    t = int(time.time())
    new = []
    if args.what in ("fish", "all"):
        known = discovered(j.lines)
        add = [f for f in ids.get("FishData", []) if f not in known]
        new += [f"{t},8,{f}" for f in add]
        print(f"[✓] 图鉴解锁 {len(add)} 种鱼（原有 {len(known)}）")
    if args.what in ("cosmetics", "all"):
        # 已有的 (槽位, 钓手, 外观) 不重复追加；记下每个 (槽位, 钓手) 最后一条 = 当前穿戴
        have, worn = set(), {}
        for ln in j.lines[1:]:
            code, f = fields(ln)
            if code == 9 and len(f) >= 5:
                have.add((f[2], f[3], f[4]))
                worn[(f[2], f[3])] = f
        total, touched = 0, set()
        for cls, slot in CUSTOMISE_SLOT.items():
            for page in (0, 1):  # 双钓手
                for item in ids.get(cls, []):
                    if (str(slot), str(page), item) in have:
                        continue
                    new.append(f"{t},9,{slot},{page},{item}")
                    touched.add((str(slot), str(page)))
                    total += 1
        # 追加的最后一条会变成当前穿戴：把原来穿的再追加一次，外观保持不变
        for key in sorted(touched):
            if key in worn:
                new.append(",".join([str(t)] + worn[key][1:]))
        print(f"[✓] 追加 {total} 条外观条目（皮肤/服装/帽/饵/竿 × 双钓手，已有的跳过，当前穿戴保持不变）")
    if not new:
        print("[i] 无需追加")
        return
    bk = backup(args.journal)
    j.lines += new
    j.save()
    print(f"[✓] 已写入 {len(new)} 条（备份 {bk.name}）")


def cmd_catch(args):
    """F23 指定鱼获取：离线追加 Catch 条目（进图鉴 + 载入后自动卖出变现）"""
    ids = load_ids(args.ids)
    known = ids.get("FishData", [])
    if known and args.fish not in known:
        sys.exit(f"[!] 未知鱼种 {args.fish}，可选：{known[:10]} ...（共 {len(known)}）")
    if args.size <= 0:
        sys.exit("[!] 尺寸必须是正整数")
    j = Journal(args.journal)
    max_id = 0
    for ln in j.lines[1:]:
        code, f = fields(ln)
        if code == 2 and len(f) >= 3:
            try:
                max_id = max(max_id, int(f[2]))
            except ValueError:
                pass
    bk = backup(args.journal)
    t = int(time.time())
    new = [f"{t},2,{max_id + 1},{args.fish},{args.size},1,0,0,"]
    if args.fish not in discovered(j.lines):
        new.append(f"{t},8,{args.fish}")
    j.lines += new
    j.save()
    print(f"[✓] 已钓到 {args.fish}（尺寸 {args.size}，catchId {max_id + 1}）→ 下次进游戏自动入账；备份 {bk.name}")


def cmd_restore(args):
    src = Path(args.backup).expanduser()
    if not src.is_file():
        sys.exit(f"[!] 备份文件不存在：{src}")
    if src.resolve() == Path(args.journal).resolve():
        sys.exit("[!] 备份文件就是存档本身")
    bk = backup(args.journal)  # 还原前先备份当前存档，还原错了还能回来
    shutil.copy2(src, args.journal)
    print(f"[✓] 已从 {src} 还原（还原前的存档已备份为 {bk.name}）")


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Cast n Chill 存档编辑器")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--force", action="store_true", help="游戏运行时也强制写入（不推荐）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("show"); p.add_argument("journal"); p.set_defaults(fn=cmd_show)

    p = sub.add_parser("cash", parents=[common]); p.add_argument("journal")
    p.add_argument("op", choices=["set", "add"]); p.add_argument("amount", type=int)
    p.add_argument("--current", type=int, default=None, help="set 时必填：当前金额")
    p.set_defaults(fn=cmd_cash)

    p = sub.add_parser("unlock", parents=[common]); p.add_argument("journal")
    p.add_argument("what", choices=["fish", "cosmetics", "all"])
    p.add_argument("--ids", default=None); p.set_defaults(fn=cmd_unlock)

    p = sub.add_parser("catch", parents=[common]); p.add_argument("journal")
    p.add_argument("fish"); p.add_argument("size", nargs="?", type=int, default=100)
    p.add_argument("--ids", default=None); p.set_defaults(fn=cmd_catch)

    p = sub.add_parser("restore", parents=[common]); p.add_argument("journal"); p.add_argument("backup")
    p.set_defaults(fn=cmd_restore)

    args = ap.parse_args()
    j = Path(args.journal).expanduser()
    if not j.is_file():
        sys.exit(f"[!] 存档不存在：{j}")
    if args.cmd != "show" and game_running():
        # 游戏追加式写存档：运行中改档会被覆盖或写坏
        if not args.force:
            sys.exit("[!] 游戏正在运行，请先退出游戏再改档（确需强制可加 --force）")
        print("[!] 游戏正在运行，按 --force 强制写入，改动可能被游戏覆盖")
    args.journal = str(j)
    args.fn(args)


if __name__ == "__main__":
    main()
