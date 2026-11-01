# CNCPanel.app —— SwiftUI 面板（无 Xcode 工程，swiftc 直编）
SWIFT   := swiftc
FLAGS   := -O -parse-as-library
APP     := build/CNCPanel.app
BIN     := $(APP)/Contents/MacOS/CNCPanel

all: $(APP)

$(BIN): src/CNCPanel.swift build-stamp
	@mkdir -p $(APP)/Contents/MacOS
	$(SWIFT) $(FLAGS) -o $@ src/CNCPanel.swift

$(APP): $(BIN) assets/AppIcon.icns
	@mkdir -p $(APP)/Contents/Resources
	@cp assets/AppIcon.icns $(APP)/Contents/Resources/AppIcon.icns
	@printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>\n\t<key>CFBundleExecutable</key><string>CNCPanel</string>\n\t<key>CFBundleIdentifier</key><string>local.castnchill.panel</string>\n\t<key>CFBundleName</key><string>CNCPanel</string>\n\t<key>CFBundleDisplayName</key><string>Cast n Chill 修改器</string>\n\t<key>CFBundlePackageType</key><string>APPL</string>\n\t<key>CFBundleShortVersionString</key><string>1.0.0</string>\n\t<key>LSMinimumSystemVersion</key><string>12.0</string>\n\t<key>NSHighResolutionCapable</key><true/>\n\t<key>CFBundleIconFile</key><string>AppIcon</string>\n</dict></plist>\n' > $(APP)/Contents/Info.plist
	@touch $(APP)
	@echo "构建完成: $(APP)"

assets/AppIcon.icns: make_icon.swift
	@mkdir -p assets
	@rm -rf assets/AppIcon.iconset
	@swift make_icon.swift assets/AppIcon.iconset >/dev/null
	@iconutil -c icns assets/AppIcon.iconset -o assets/AppIcon.icns
	@echo "图标已重新生成"

build-stamp:

clean:
	rm -rf build

.PHONY: all clean
