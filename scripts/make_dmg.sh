#!/bin/bash
# 打包 CNCPanel.app + 全部组件为 dmg（D5：分发形态）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/CastNChillTrainer"
APP="$DIST/CNCPanel.app"

rm -rf "$ROOT/dist/CastNChillTrainer"
mkdir -p "$APP/Contents/Resources"
cp -R "$ROOT/panel/build/CNCPanel.app/." "$APP/"
cp "$ROOT/mod/build/libcncmod.dylib" "$APP/Contents/Resources/"
cp "$ROOT/launcher/cnc" "$APP/Contents/Resources/cnc"
cp "$ROOT/save-tools/cnc_save.py" "$APP/Contents/Resources/cnc_save.py"
cp "$ROOT/README.md" "$DIST/使用说明.md"

cat > "$DIST/安装组件.command" <<'EOF'
#!/bin/bash
# 把 CNCPanel.app 内组件安装到 ~/.castnchill-modifier/
set -e
SRC="$(cd "$(dirname "$0")" && pwd)/CNCPanel.app/Contents/Resources"
DST="$HOME/.castnchill-modifier"
mkdir -p "$DST"
cp "$SRC/libcncmod.dylib" "$SRC/cnc" "$SRC/cnc_save.py" "$DST/"
chmod +x "$DST/cnc" "$DST/cnc_save.py"
echo "✓ 组件已安装到 $DST"
echo "  日常启动：$DST/cnc   （自动备份存档 + 注入启动游戏）"
echo "  面板：应用程序里的 CNCPanel"
read -n 1 -s -r -p "按任意键关闭..."
EOF
chmod +x "$DIST/安装组件.command"

hdiutil create -volname "CastNChillTrainer" \
  -srcfolder "$DIST" -format UDZO -ov \
  "$ROOT/dist/CastNChillTrainer.dmg" -quiet
echo "✓ 打包完成：$ROOT/dist/CastNChillTrainer.dmg"
hdiutil imageinfo "$ROOT/dist/CastNChillTrainer.dmg" 2>/dev/null | grep -E "Format:|Size" | head -2 || true
