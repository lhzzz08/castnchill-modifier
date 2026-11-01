#ifndef CNC_RESOLVER_H
#define CNC_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IL2CPP 运行时按名解析层。
 * 一切按 命名空间/类/方法名 在运行时解析，仓库禁止出现硬编码偏移 ——
 * 这是修改器扛游戏更新的根本设计。
 */

typedef struct {
    void *ga;               /* GameAssembly.dylib 的 RTLD_NOLOAD 句柄 */
    void *domain;           /* il2cpp 域 */
    size_t assembly_count;  /* 程序集数量 */

    /* il2cpp 导出 API（部分） */
    void *(*domain_get)(void);
    void **(*domain_get_assemblies)(void *, size_t *);
    void *(*thread_attach)(void *);
    void *(*assembly_get_image)(void *);
    void *(*class_from_name)(void *image, const char *ns, const char *name);
    void *(*class_get_method_from_name)(void *klass, const char *name, int argc);
    void *(*runtime_invoke)(void *method, void *obj, void **params, void **exc);
    const uint16_t *(*string_chars)(void *str);
    int (*string_length)(void *str);
    void *(*object_new)(void *klass);
    void *(*class_get_field_from_name)(void *klass, const char *name);
    int (*field_get_offset)(void *field);
    void *(*string_new)(const char *str);
    void *(*class_get_type)(void *klass);
    void *(*type_get_object)(void *type);
    size_t (*array_length)(void *array);
    void *(*field_get_type)(void *field);
    void *(*class_from_il2cpp_type)(void *type);
} cnc_il2cpp_t;

/* 以下全部函数只能在 Unity 主线程调用（见 cnc_mt）。 */

/* 万能实例枚举：Resources.FindObjectsOfTypeAll(Type) 拿某类全部已加载实例。
 * 返回 malloc 的指针数组（调用方 free），*n 为数量，失败返回 NULL。 */
void **cnc_find_instances(void *klass, size_t *n);

extern cnc_il2cpp_t g_il2;

/* 找到 GameAssembly.dylib（遍历 dyld 镜像 + RTLD_NOLOAD），解析全部 API 指针。
 * 返回 false 表示 GameAssembly 尚未加载或缺少必需符号（g_il2 保持全空）。 */
bool cnc_il2cpp_bind(void);

/* 把当前线程 attach 到 il2cpp（主线程已由 Unity attach，重复调用无害）。 */
void *cnc_il2cpp_attach(void);

/* 按 命名空间+类名 在所有程序集镜像里找类（命中结果缓存），找不到返回 NULL。 */
void *cnc_find_class(const char *ns, const char *name);

/* 取 UTF-16 的 Il2CppString 内容转为 UTF-8（malloc，调用方 free）。 */
char *cnc_string_to_utf8(void *il2str);

#endif
