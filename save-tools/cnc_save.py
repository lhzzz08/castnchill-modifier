#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cast n Chill 存档编辑器（F20/F18）

存档格式（M3 逆向结论，DESIGN.md 附录 B）：
- 纯文本，每行一条事件：`time,code,args...`（无尾随换行符）
- 首行 `<固定标识>,0,6` 为创建标识 + 版本条目，不需要动
- 常用条目码：1 Travel / 2 Catch / 3 Sell / 4 Buy / 6 Checkpoint /
  7 Flag / 8 Discovered / 9 Customise / 10 TimeOfDay / 11 CashTip
- 金钱 = 重放 CashTip(11) 等条目的净和；离线改钱 = 追加 `t,11,<delta>`
  （delta 为负 = 加钱，与 TipRusty 语义一致）
- 无校验和（首行标识跨存档恒定，实测可直接增改行）

用法：
  cnc_save.py show <journal>
  cnc_save.py cash  <journal> set|add <amount>
  cnc_save.py unlock <journal> fish|cosmetics|all [--ids game-ids.json]
  cnc_save.py restore <journal> <backup>
"""
import json
import os
import shutil
import sys
import time
from pathlib import Path

IDS_DEFAULT = Path.home() / ".castnchill-modifier/game-ids.json"

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


def load_ids(path):
    p = Path(path) if path else IDS_DEFAULT
    if not p.exists():
        sys.exit(f"[!] ID 清单不存在：{p}\n    先启动带 mod 的游戏执行一次 ids.dump")
    return json.loads(p.read_text())


def read_journal(path):
    text = Path(path).read_text()
    return text.rstrip("\n").split("\n") if text.strip() else []


def backup(path):
    dst = Path(path).parent / f"{Path(path).name}.cncbak-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, dst)
    return dst


def write_journal(path, lines):
    Path(path).write_text("\n".join(lines) + "\n")


def cmd_show(args):
    lines = read_journal(args.journal)
    cash, fish, cosmetics, sells, catches = 0, set(), set(), 0, 0
    print(f"存档：{args.journal}（{len(lines)} 行）")
    print(f"首行标识：{lines[0].split(',')[0][:20]}...")
    for ln in lines[1:]:
        f = ln.split(",")
        try:
            code = int(f[1])
        except (IndexError, ValueError):
            continue
        if code == 11 and len(f) >= 3:
            cash += int(f[2])
        elif code == 8 and len(f) >= 3:
            fish.add(f[2])
        elif code == 9 and len(f) >= 6:
            cosmetics.add(f[5])
        elif code == 3:
            sells += 1
        elif code == 2:
            catches += 1
    print(f"现金（CashTip 净和）：{cash}")
    print(f"渔获 {catches} 次 / 售出 {sells} 次 / 图鉴 {len(fish)} 种")
    print(f"外观条目 {len(cosmetics)} 条")


def cmd_cash(args):
    lines = read_journal(args.journal)
    cur = sum(int(l.split(",")[2]) for l in lines[1:]
              if l.split(",")[1:2] == ["11"])
    target = cur if args.op == "set" else cur + args.amount
    if args.op == "set":
        target = args.amount
    target = max(0, min(target, 1_000_000_000))
    bk = backup(args.journal)
    lines.append(f"{int(time.time())},11,{-(target - cur)}")
    write_journal(args.journal, lines)
    print(f"[✓] 现金 {cur} → {target}（追加 CashTip 条目；备份 {bk.name}）")


def cmd_unlock(args):
    ids = load_ids(getattr(args, "ids", None))
    lines = read_journal(args.journal)
    t = int(time.time())
    new = []
    if args.what in ("fish", "all"):
        known = {l.split(",")[2] for l in lines[1:] if l.split(",")[1:2] == ["8"]}
        add = [f for f in ids.get("FishData", []) if f not in known]
        new += [f"{t},8,{f}" for f in add]
        print(f"[✓] 图鉴解锁 {len(add)} 种鱼（原有 {len(known)}）")
    if args.what in ("cosmetics", "all"):
        total = 0
        for cls, slot in CUSTOMISE_SLOT.items():
            for page in (0, 1):  # 双钓手
                for item in ids.get(cls, []):
                    new.append(f"{t},9,{slot},{page},{item}")
                    total += 1
        print(f"[✓] 追加 {total} 条外观装备条目（皮肤/服装/帽/饵/竿 × 双钓手）")
    if not new:
        print("[i] 无需追加")
        return
    bk = backup(args.journal)
    lines += new
    write_journal(args.journal, lines)
    print(f"[✓] 已写入 {len(new)} 条（备份 {bk.name}）")


def cmd_catch(args):
    """F23 指定鱼获取：离线追加 Catch 条目（进图鉴 + 载入后自动卖出变现）"""
    ids = load_ids(getattr(args, "ids", None))
    known = ids.get("FishData", [])
    if known and args.fish not in known:
        sys.exit(f"[!] 未知鱼种 {args.fish}，可选：{known[:10]} ...（共 {len(known)}）")
    lines = read_journal(args.journal)
    max_id = 0
    for ln in lines[1:]:
        f = ln.split(",")
        if len(f) >= 3 and f[1] == "2":
            try:
                max_id = max(max_id, int(f[2]))
            except ValueError:
                pass
    size = args.size if args.size else 100
    bk = backup(args.journal)
    t = int(time.time())
    new = [f"{t},2,{max_id + 1},{args.fish},{size},1,0,0,"]
    if args.fish not in {l.split(',')[2] for l in lines[1:] if l.split(',')[1:2] == ['8']}:
        new.append(f"{t},8,{args.fish}")
    lines += new
    write_journal(args.journal, lines)
    print(f"[✓] 已钓到 {args.fish}（尺寸 {size}，catchId {max_id+1}）→ 下次进游戏自动入账；备份 {bk.name}")


def cmd_restore(args):
    shutil.copy2(args.backup, args.journal)
    print(f"[✓] 已从 {args.backup} 还原")


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Cast n Chill 存档编辑器")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("show"); p.add_argument("journal"); p.set_defaults(fn=cmd_show)

    p = sub.add_parser("cash"); p.add_argument("journal")
    p.add_argument("op", choices=["set", "add"]); p.add_argument("amount", type=int)
    p.set_defaults(fn=cmd_cash)

    p = sub.add_parser("unlock"); p.add_argument("journal")
    p.add_argument("what", choices=["fish", "cosmetics", "all"])
    p.add_argument("--ids", default=None); p.set_defaults(fn=cmd_unlock)

    p = sub.add_parser("catch"); p.add_argument("journal")
    p.add_argument("fish"); p.add_argument("size", nargs="?", type=int, default=100)
    p.add_argument("--ids", default=None); p.set_defaults(fn=cmd_catch)

    p = sub.add_parser("restore"); p.add_argument("journal"); p.add_argument("backup")
    p.set_defaults(fn=cmd_restore)

    args = ap.parse_args()
    j = Path(args.journal).expanduser()
    if not j.exists():
        sys.exit(f"[!] 存档不存在：{j}")
    if args.cmd != "restore" and args.cmd != "show":
        # 游戏运行中改档会互相覆盖，提醒但不阻拦（游戏是追加式写入）
        try:
            out = os.popen("pgrep -f 'MacOS/Cast n Chill'").read().strip()
            if out:
                print("[!] 游戏正在运行——建议先退出游戏再离线改档，否则改动会被游戏覆盖")
        except Exception:
            pass
    args.journal = str(j)
    args.fn(args)


if __name__ == "__main__":
    main()
