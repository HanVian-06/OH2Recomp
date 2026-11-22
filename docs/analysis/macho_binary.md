# Mach-O Binary Analysis (`Payload/OH2.app/OH2`)

**Reproducible Verification Script**: `python scripts/analysis/inspect_macho.py` (supports `--json`)  
**Target Binary (Read-Only)**: `Payload/OH2.app/OH2` (`53,729,072` bytes)  
**Engine**: Unreal Engine 4.23 (`++UE4+Release-4.23 May 22 2020 +`), Bundle ID `com.cornfox.oceanhorn2`, Version `100.76955.200521` (`0.78`)

---

## 1. FairPlay Encryption Gate (`LC_ENCRYPTION_INFO_64`)
- **Result**: **PASS (`cryptid == 0`)** — no FairPlay encryption; Human Checkpoint 1 is clear.
- **Load Command**: `LC_ENCRYPTION_INFO_64` (`0x2c`, index `12`, file offset `3000`, `cryptoff = 0x58e000` (`5,824,512`), `cryptsize = 0x1000` (`4,096`), `cryptid = 0`).
- **Entry Point Content Verification (`LC_MAIN`)**:
  - `entryoff = 0x58e3bc` (`5,825,468`, inside `[0x58e000, 0x58f000)`).
  - First instructions decoded via `capstone` (`CS_ARCH_ARM64`):
    1. `sub sp, sp, #0x1a0`
    2. `stp x28, x27, [sp, #0x140]`
    3. `stp x26, x25, [sp, #0x150]`
  - Confirms valid plaintext ARM64 code at entry.

---

## 2. Mach-O Header, Segments & Sections
- **Header (`mach_header_64`)**:
  - `magic`: `0xfeedfacf` (`MH_MAGIC_64`, single thin ARM64 binary — no FAT/universal header, no embedded `.dylib`/`.framework` in `OH2.app`)
  - `cputype`: `0x0100000c` (`CPU_TYPE_ARM64`), `cpusubtype`: `0x00000000` (`CPU_SUBTYPE_ARM64_ALL`)
  - `filetype`: `2` (`MH_EXECUTE`)
  - `ncmds`: `47`, `sizeofcmds`: `5576` bytes
  - `flags`: `0x00218085` (`MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_WEAK_DEFINES | MH_BINDS_TO_WEAK | MH_PIE`)
  - `LC_BUILD_VERSION`: `platform=2` (iOS), `minos=12.0.0`, `sdk=13.4.0`
  - `LC_UUID`: `200ca4cd2f6f3fc9bd77521f44097a68`
  - `LC_LOAD_DYLINKER`: `/usr/lib/dyld` | `LC_RPATH`: `@executable_path/Frameworks`
- **Segments (`4` `LC_SEGMENT_64`, `30` sections total)**:
  - `__PAGEZERO`: `vmaddr=0x0`, `vmsize=0x100000000`, `fileoff=0`, `filesize=0`, `nsects=0`
  - `__TEXT`: `vmaddr=0x100000000`, `vmsize=0x2b9c000` (`45,727,744` B), `filesize=45,727,744`, `maxprot/initprot=5/5` (RX), `12` sections (`__text`, `__stubs`, `__stub_helper`, `__gcc_except_tab`, `__const`, `__objc_methname`, `__objc_classname`, `__objc_methtype`, `__cstring`, `__ustring`, `__unwind_info`, `__eh_frame`)
  - `__DATA`: `vmaddr=0x102b9c000`, `vmsize=0x7e8000` (`8,290,304` B), `fileoff=45,727,744`, `filesize=6,209,536`, `maxprot/initprot=3/3` (RW), `18` sections (`__got`, `__la_symbol_ptr`, `__mod_init_func`, `__const`, `__cfstring`, `__objc_*`, `__data`, `__bss`, `__common`)
  - `__LINKEDIT`: `vmaddr=0x103384000`, `vmsize=0x1b8000` (`1,802,240` B), `fileoff=51,937,280`, `filesize=1,791,792`, `maxprot/initprot=1/1` (R)

---

## 3. Fixups (`LC_DYLD_INFO_ONLY`) & Initializers
`OH2` uses **`LC_DYLD_INFO_ONLY`** (`0x80000022`) — **zero** `LC_DYLD_CHAINED_FIXUPS` or `LC_DYLD_EXPORTS_TRIE`:
- **Rebase pointers**: `520,119` (`rebase_size = 149,192` B)
- **Bind sites**: `4,040` (`272` unique `(ordinal, symbol)` pairs, `bind_size = 14,576` B)
- **Weak bind sites**: `672` (`660` unique symbols, mostly `physx::*` and `_ZTV*`, `weak_bind_size = 54,632` B)
- **Lazy bind sites**: `539` (`539` unique `(ordinal, symbol)` pairs, `lazy_bind_size = 12,936` B)
- **Export trie symbols**: `661` (`export_size = 478,272` B)
- **Initializers (`__DATA,__mod_init_func`)**: `466` function pointers (`3,728` B; first 5: `0x1000dc270`, `0x1000dc2a4`, `0x1000de708`, `0x1000e0808`, `0x1000e19d0`). No `__init_offsets`.

---

## 4. Linked Dylibs (`30`) & Undefined Symbols (`805`)
- `LC_SYMTAB`: `1,467` symbols (`1` local, `661` extdef, **`805` undefined**); `LC_DYSYMTAB`: `2,052` indirect symbols.
- `30` `LC_LOAD*_DYLIB` commands (`27` strong `LC_LOAD_DYLIB`, `3` weak `LC_LOAD_WEAK_DYLIB`: `CloudKit`, `MultipeerConnectivity`, `Metal`), **`27` with >= 1 undefined symbol** (`WebKit`, `CoreAudio`, `MediaToolbox` have `0` direct undefined symbols):

| Ord | Type | Dylib | Undef Count |
|---:|---|---|---:|
| 1 | Strong | `/System/Library/Frameworks/OpenGLES.framework/OpenGLES` | 99 |
| 2 | Strong | `/System/Library/Frameworks/QuartzCore.framework/QuartzCore` | 6 |
| 3 | Strong | `/System/Library/Frameworks/UIKit.framework/UIKit` | 52 |
| 4 | Strong | `/System/Library/Frameworks/Foundation.framework/Foundation` | 42 |
| 5 | Strong | `/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox` | 16 |
| 6 | Strong | `/System/Library/Frameworks/AVFoundation.framework/AVFoundation` | 27 |
| 7 | Strong | `/System/Library/Frameworks/GameKit.framework/GameKit` | 11 |
| 8 | Strong | `/System/Library/Frameworks/StoreKit.framework/StoreKit` | 7 |
| 9 | Strong | `/System/Library/Frameworks/CoreVideo.framework/CoreVideo` | 15 |
| 10 | Strong | `/System/Library/Frameworks/CoreMedia.framework/CoreMedia` | 12 |
| 11 | Strong | `/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics` | 12 |
| 12 | Strong | `/System/Library/Frameworks/GameController.framework/GameController` | 3 |
| 13 | Strong | `/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration` | 2 |
| 14 | Strong | `/System/Library/Frameworks/DeviceCheck.framework/DeviceCheck` | 1 |
| 15 | Strong | `/System/Library/Frameworks/UserNotifications.framework/UserNotifications` | 5 |
| 16 | Strong | `/System/Library/Frameworks/CoreMotion.framework/CoreMotion` | 1 |
| 17 | Strong | `/System/Library/Frameworks/WebKit.framework/WebKit` | 0 |
| 18 | Strong | `/System/Library/Frameworks/CoreAudio.framework/CoreAudio` | 0 |
| 19 | Strong | `/System/Library/Frameworks/Security.framework/Security` | 12 |
| 20 | Strong | `/System/Library/Frameworks/MediaToolbox.framework/MediaToolbox` | 0 |
| 21 | Strong | `/System/Library/Frameworks/CoreImage.framework/CoreImage` | 8 |
| 22 | Strong | `/System/Library/Frameworks/ImageIO.framework/ImageIO` | 1 |
| 23 | Weak | `/System/Library/Frameworks/CloudKit.framework/CloudKit` | 8 |
| 24 | Weak | `/System/Library/Frameworks/MultipeerConnectivity.framework/MultipeerConnectivity` | 2 |
| 25 | Weak | `/System/Library/Frameworks/Metal.framework/Metal` | 28 |
| 26 | Strong | `/usr/lib/libz.1.dylib` | 12 |
| 27 | Strong | `/usr/lib/libc++.1.dylib` | 16 |
| 28 | Strong | `/usr/lib/libSystem.B.dylib` | 323 |
| 29 | Strong | `/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation` | 55 |
| 30 | Strong | `/usr/lib/libobjc.A.dylib` | 29 |

---

## 5. Darwin ARM64 Specifics (M1 Loader Requirements)
- **C++ ABI (`libc++.1.dylib`, `16` symbols, `std::__1` namespace — 0 `__ndk1`)**:
  - `__ZNSt3__112__next_primeEm`, `__ZNSt3__15mutex4lockEv`, `__ZNSt3__15mutex6unlockEv`, `__ZSt9terminatev`
  - `__ZTVN10__cxxabiv117__class_type_infoE`, `__ZTVN10__cxxabiv120__si_class_type_infoE`, `__ZTVN10__cxxabiv121__vmi_class_type_infoE`
  - `___cxa_bad_typeid`, `___cxa_begin_catch`, `___cxa_demangle`, `___cxa_guard_abort`, `___cxa_guard_acquire`, `___cxa_guard_release`, `___cxa_pure_virtual`, `___dynamic_cast`, `___gxx_personality_v0`
- **TLS / `TPIDRRO_EL0` / `TPIDR_EL0` Scan**:
  - Zero `__thread_vars`, `__thread_data`, or `__thread_bss` sections.
  - Full scan of `10,507,190` 32-bit ARM64 instruction words (`__TEXT,__text`, `__stubs`, `__stub_helper`): **`0` `MRS TPIDRRO_EL0`**, **`0` `MRS TPIDR_EL0`**, **`0` `MRS`**, **`0` `MSR`**.
  - `OH2` uses POSIX `pthread_key_create` / `pthread_getspecific` / `pthread_setspecific` via `libSystem.B.dylib`.
- **Exception Unwinding**:
  - `__TEXT,__unwind_info`: `25,600` B (v1, `48` common encodings, `1` personality `___gxx_personality_v0`)
  - `__TEXT,__eh_frame`: `1,136` B (`5` CIEs, `19` FDEs)
  - `__TEXT,__gcc_except_tab`: `61,464` B
- **Darwin Variadic & `va_list` Imports (`12 + 2`)**:
  - Darwin ARM64 passes variadic arguments on the stack (`va_list` = `char *`), whereas Android AAPCS64 passes variadic args in `x0..x7` / `d0..d7` (`va_list` = 32-byte `struct __va_list`).
  - **12 variadic imports** requiring stack-to-AAPCS64 trampolines: `_printf`, `_fprintf`, `_sprintf`, `_snprintf`, `___sprintf_chk`, `___snprintf_chk`, `_sscanf`, `_NSLog`, `_open`, `_fcntl`, `_ioctl`, `_syscall`.
  - **2 `va_list` imports** requiring Darwin `char *` -> AAPCS64 `va_list` adapters: `_vfprintf`, `_vsnprintf`.
- **Dynamic `dlsym` Lookups**: `"BELUM DIKETAHUI"` statically — `OH2` imports `_dlsym` (`libSystem.B.dylib`); any runtime-resolved symbol names will be discovered during M1/M2 loader execution via `darwin_resolve_symbol` miss logging.
