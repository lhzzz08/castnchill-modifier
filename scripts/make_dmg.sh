#!/bin/bash
# 打包 CNCPanel.app + 全部组件为 dmg（D5：分发形态）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/CastNChillTrainer"
APP="$DIST/CNCPanel.app"

for f in "$ROOT/panel/build/CNCPanel.app" "$ROOT/mod/build/libcncmod.dylib"; do
  [ -e "$f" ] || { echo "缺少构建产物：$f（先执行 make）" >&2; exit 1; }
done

rm -rf "$ROOT/dist/CastNChillTrainer"
mkdir -p "$APP/Contents/Resources"
cp -R "$ROOT/panel/build/CNCPanel.app/." "$APP/"
cp "$ROOT/mod/build/libcncmod.dylib" "$APP/Contents/Resources/"
cp "$ROOT/launcher/cnc" "$APP/Contents/Resources/cnc"
cp "$ROOT/save-tools/cnc_save.py" "$APP/Contents/Resources/cnc_save.py"
cp "$ROOT/README.md" "$DIST/使用说明.md"
cp "$ROOT/docs/使用手册.md" "$DIST/使用手册.md"
# 往 Resources 里加了文件，原来的签名封印失效，必须重签（ad-hoc）
codesign --force --deep --sign - "$APP"

cat > "$DIST/安装组件.command" <<'EOF'
#!/bin/bash
# 把 CNCPanel.app 内组件安装到 ~/.castnchill-modifier/
set -e
SRC="$(cd "$(dirname "$0")" && pwd)/CNCPanel.app/Contents/Resources"
DST="$HOME/.castnchill-modifier"
for f in libcncmod.dylib cnc cnc_save.py; do
  [ -f "$SRC/$f" ] || { echo "✗ 安装包不完整，缺少 $f"; read -n 1 -s -r -p "按任意键关闭..."; exit 1; }
done
mkdir -p "$DST"
cp "$SRC/libcncmod.dylib" "$SRC/cnc" "$SRC/cnc_save.py" "$DST/"
chmod +x "$DST/cnc" "$DST/cnc_save.py"
# 从 dmg 拷出的文件带隔离标记：不去掉的话 dylib 可能被拒绝注入、cnc 双击会被拦
xattr -d com.apple.quarantine "$DST/libcncmod.dylib" "$DST/cnc" "$DST/cnc_save.py" 2>/dev/null || true
if [ -d /Applications/CNCPanel.app ]; then
  xattr -dr com.apple.quarantine /Applications/CNCPanel.app 2>/dev/null || true
fi
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
