#ifndef OH2_MACHO_LOADER_H
#define OH2_MACHO_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OH2_MAX_SEGMENTS 8
#define OH2_MAX_SECTIONS 64
#define OH2_MAX_DYLIBS   64

typedef struct {
    char     segname[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
    void    *mapped_addr;
} MachOSegment;

typedef struct {
    char     sectname[17];
    char     segname[17];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    void    *mapped_addr;
} MachOSection;

typedef struct {
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    uint32_t export_off;
    uint32_t export_size;
} MachODyldInfo;

typedef struct {
    char          path[512];
    int           fd;
    size_t        file_size;
    void         *base_addr;
    uint64_t      preferred_base;
    intptr_t      slide;
    size_t        mapped_span;

    uint32_t      ncmds;
    uint32_t      sizeofcmds;
    uint32_t      header_flags;

    bool          has_encryption_info;
    uint32_t      cryptoff;
    uint32_t      cryptsize;
    uint32_t      cryptid;

    uint32_t      tpidrro_el0_reads;

    uint32_t      num_segments;
    MachOSegment  segments[OH2_MAX_SEGMENTS];

    uint32_t      num_sections;
    MachOSection  sections[OH2_MAX_SECTIONS];

    uint32_t      num_dylibs;
    char          dylibs[OH2_MAX_DYLIBS][256];
    bool          dylib_weak[OH2_MAX_DYLIBS];

    MachODyldInfo dyld_info;
    const uint8_t *linkedit_file_base;

    uint32_t      symoff;
    uint32_t      nsyms;
    uint32_t      stroff;
    uint32_t      strsize;
    uint32_t      indirectsymoff;
    uint32_t      nindirectsyms;

    void        **mod_init_funcs;
    uint32_t      mod_init_count;
    uint32_t      mod_init_executed;
    uint32_t      current_mod_init_idx;
    void         *current_mod_init_fn;

    uint64_t      entry_offset;
    void         *entry_point;

    /* Fixup & registration telemetry */
    uint32_t      rebase_count;
    uint32_t      bind_count;
    uint32_t      bind_addend16_count;
    uint32_t      lazy_bind_count;
    uint32_t      weak_bind_count;
    uint32_t      missing_symbol_count;

    uint32_t      eh_frame_fde_count;
    uint32_t      synth_fde_count;
    uint32_t      lsda_count;

    uint32_t      objc_class_count;
    uint32_t      objc_cat_count;
    uint32_t      objc_proto_count;
    uint32_t      objc_selref_count;
    uint32_t      cfstring_count;
} MachOImage;

/* Public M1 Loader API */
int   macho_load(const char *path, MachOImage *out_img);
void *macho_lookup_export(const MachOImage *img, const char *sym_name);
int   macho_run_initializers(MachOImage *img, int argc, char **argv, char **envp, char **apple);
int   macho_call_main(MachOImage *img, int argc, char **argv, char **envp, char **apple);
void  macho_unload(MachOImage *img);
const MachOSection *macho_find_section(const MachOImage *img, const char *segname, const char *sectname);
const MachOImage   *macho_get_active_image(void);

/* LC_DYLD_INFO_ONLY Fixup Engine (dyld_fixups.c) */
int dyld_apply_all_fixups(MachOImage *img);

/* Unwind Bridge (unwind_bridge.cpp) */
int oh2_unwind_register(MachOImage *img);

/* Symbol & ObjC Runtime Resolver (symbol_resolver.cpp) */
void  oh2_symbol_resolver_init(MachOImage *img);
int   oh2_objc_register_image(MachOImage *img);
void *darwin_resolve_symbol(const char *dylib_path, const char *sym_name);
const char *darwin_cfstring_utf8(const void *cfstr_obj, char *scratch, size_t scratch_size);
void *darwin_objc_lookup_imp(void *self, const char *sel);
void *darwin_objc_lookup_super2_imp(void *sup, const char *sel);

/* M2 Extension Hooks (zero overhead / no-op unless explicitly configured by M2 runtime) */
void        oh2_symbol_register_override(const char *sym_name, void *addr);
void        oh2_objc_register_instance_method(const char *cls_name, const char *sel_name, void *imp);
void        oh2_objc_register_class_method(const char *cls_name, const char *sel_name, void *imp);
void       *oh2_objc_get_class(const char *cls_name);
void       *oh2_objc_alloc_instance(void *cls, size_t extra_bytes);
void       *oh2_objc_create_string(const char *utf8);
void       *oh2_objc_create_string_len(const char *utf8, size_t len);
const char *oh2_objc_intern_selector(const char *sel_name);
void       *oh2_objc_lookup_imp(void *self, const char *sel);
void       *oh2_objc_lookup_super2_imp(void *sup, const char *sel);
int         oh2_objc_is_heap_object(const void *obj);
int32_t     oh2_objc_get_refcount(const void *obj);
int32_t     oh2_objc_add_refcount(void *obj, int32_t delta);
void        oh2_objc_set_bundle_root(const char *path);
const char *oh2_objc_get_bundle_root(void);

/* Toll-free CF <-> Foundation container helpers */
void       *oh2_objc_array_create(const void **items, size_t count);
void        oh2_objc_array_add(void *arr, void *item);
size_t      oh2_objc_array_count(const void *arr);
void       *oh2_objc_array_get(const void *arr, size_t idx);
void       *oh2_objc_dict_create(void);
void        oh2_objc_dict_set(void *dict, const char *key, void *val);
void       *oh2_objc_dict_get(const void *dict, const char *key);
size_t      oh2_objc_dict_count(const void *dict);
void       *oh2_objc_data_create(const void *bytes, size_t len);
const void *oh2_objc_data_bytes(const void *data);
size_t      oh2_objc_data_length(const void *data);

typedef int  (*oh2_open_handler_t)(const char *path, int d_flags, char *darwin_sp);
void         oh2_variadic_set_open_handler(oh2_open_handler_t fn);

typedef void (*oh2_ue4_log_hook_t)(const char *msg);
void         oh2_variadic_set_log_hook(oh2_ue4_log_hook_t fn);

/* Logging helper mirrored to logcat + stdout */
void oh2_log_info(const char *fmt, ...);
void oh2_log_error(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* OH2_MACHO_LOADER_H */
