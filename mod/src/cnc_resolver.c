#include "cnc_resolver.h"
#include "cnc_log.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

cnc_il2cpp_t g_il2;

static void *find_gameassembly(void) {
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *path = _dyld_get_image_name(i);
        if (!path) continue;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (strcmp(base, "GameAssembly.dylib") == 0)
            return dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    }
    return NULL;
}

#define BIND(field, name)                                                   \
    do {                                                                    \
        g_il2.field = (__typeof__(g_il2.field))dlsym(g_il2.ga, name);       \
        if (!g_il2.field) {                                                 \
            cnc_log("resolver: 缺少导出符号 %s", name);                       \
            return false;                                                   \
        }                                                                   \
    } while (0)

#define BIND_OPT(field, name)                                               \
    do {                                                                    \
        g_il2.field = (__typeof__(g_il2.field))dlsym(g_il2.ga, name);       \
        if (!g_il2.field) cnc_log("resolver: 可选符号缺失 %s", name);         \
    } while (0)

void **cnc_find_instances(void *klass, size_t *n) {
    *n = 0;
    if (!g_il2.class_get_type || !g_il2.type_get_object) return NULL;
    void *type_obj = g_il2.type_get_object(g_il2.class_get_type(klass));
    void *res = cnc_find_class("UnityEngine", "Resources");
    if (!type_obj || !res) return NULL;
    void *m = g_il2.class_get_method_from_name(res, "FindObjectsOfTypeAll", 1);
    if (!m) return NULL;
    void *exc = NULL;
    void *params[1] = {type_obj};
    void *arr = g_il2.runtime_invoke(m, NULL, params, &exc);
    if (exc || !arr) {
        cnc_log("resolver: FindObjectsOfTypeAll 异常 %p", exc);
        return NULL;
    }
    size_t len = g_il2.array_length(arr);
    /* Il2CppArray: [klass 8][monitor 8][max_length 8][vector 数据 @0x20] */
    void **out = malloc((len ? len : 1) * sizeof(void *));
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = *(void **)((char *)arr + 0x20 + i * sizeof(void *));
    *n = len;
    return out;
}

bool cnc_il2cpp_bind(void) {
    if (g_il2.ga) return true;
    g_il2.ga = find_gameassembly();
    if (!g_il2.ga) return false;

    BIND(domain_get, "il2cpp_domain_get");
    BIND(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    BIND(thread_attach, "il2cpp_thread_attach");
    BIND(assembly_get_image, "il2cpp_assembly_get_image");
    BIND(class_from_name, "il2cpp_class_from_name");
    BIND(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    BIND(runtime_invoke, "il2cpp_runtime_invoke");
    BIND(string_chars, "il2cpp_string_chars");
    BIND(string_length, "il2cpp_string_length");
    BIND(object_new, "il2cpp_object_new");
    BIND(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    BIND(field_get_offset, "il2cpp_field_get_offset");
    BIND(string_new, "il2cpp_string_new");
    BIND_OPT(class_get_type, "il2cpp_class_get_type");
    BIND_OPT(type_get_object, "il2cpp_type_get_object");
    BIND_OPT(array_length, "il2cpp_array_length");
    BIND_OPT(field_get_type, "il2cpp_field_get_type");
    BIND_OPT(class_from_il2cpp_type, "il2cpp_class_from_il2cpp_type");
    return true;
}

bool cnc_il2cpp_wait_ready(int timeout_ms) {
    for (int waited = 0; waited <= timeout_ms; waited += 50) {
        if (cnc_il2cpp_bind() && g_il2.domain_get && g_il2.domain_get()) {
            g_il2.domain = g_il2.domain_get();
            size_t n = 0;
            if (g_il2.domain_get_assemblies)
                g_il2.domain_get_assemblies(g_il2.domain, &n);
            g_il2.assembly_count = n;
            return true;
        }
        usleep(50 * 1000);
    }
    return false;
}

void *cnc_il2cpp_attach(void) {
    return g_il2.thread_attach ? g_il2.thread_attach(g_il2.domain) : NULL;
}

void *cnc_find_class(const char *ns, const char *name) {
    if (!g_il2.domain) return NULL;
    size_t n = 0;
    void **asms = g_il2.domain_get_assemblies(g_il2.domain, &n);
    if (!asms) return NULL;
    for (size_t i = 0; i < n; i++) {
        void *image = g_il2.assembly_get_image(asms[i]);
        if (!image) continue;
        void *klass = g_il2.class_from_name(image, ns, name);
        if (klass) return klass;
    }
    return NULL;
}

char *cnc_string_to_utf8(void *il2str) {
    if (!il2str) return NULL;
    int len = g_il2.string_length(il2str);
    const uint16_t *chars = g_il2.string_chars(il2str);
    if (len < 0 || !chars) return NULL;

    /* UTF-16 -> UTF-8（游戏版本号等纯 ASCII 场景为主，兼容简单代理对） */
    char *out = malloc((size_t)len * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (int i = 0; i < len; i++) {
        uint32_t cp = chars[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len &&
            chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}
