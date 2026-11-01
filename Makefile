# Cast n Chill 修改器 —— 顶层构建
# make        编译注入模块 + 面板
# make dmg    编译并打包 CastNChillTrainer.dmg
# make clean  清理全部构建产物

.PHONY: all mod panel dmg clean

all: mod panel

mod:
	$(MAKE) -C mod

panel:
	$(MAKE) -C panel

dmg: all
	bash scripts/make_dmg.sh

clean:
	-$(MAKE) -C mod clean
	-$(MAKE) -C panel clean
	rm -rf dist
