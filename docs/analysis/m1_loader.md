# M1 Headless Mach-O ARM64 Runtime Loader (`M1_LOADER`)

## 1. Architecture Overview
The M1 Loader subsystem (`src/loader/`, built as both static/shared library `liboh2_loader.so` and standalone headless executable `oh2_m1_loader` via `android/m1_headless/CMakeLists.txt`) loads and executes the unmodified iOS ARM64 Mach-O 64-bit executable `Payload/OH2.app/OH2` (`53,729,072` bytes) directly inside an Android `arm64-v8a` process on Samsung Galaxy A56 (`SM-A566B`).

| Component | Path | Responsibility |
|---|---|---|
| **Public Loader API & Structs** | `src/loader/macho_loader.h` | Mach-O 64-bit load command definitions, `MachOImage` state, `LC_DYLD_INFO_ONLY` opcode constants, and 5-stage pipeline API |
| **Segment Mapper & Entry Runner** | `src/loader/macho_loader.cpp` | FairPlay DRM gate (`cryptid == 0`), contiguous VA reservation (`0x100000000..0x10353c000`), `__TEXT`/`__DATA`/`__LINKEDIT` `mmap`, instruction cache flush, `TPIDRRO_EL0` scan, `466` `__mod_init_func` runner, and `LC_MAIN` invocation |
| **Dyld Opcode Interpreter** | `src/loader/dyld_fixups.cpp` | Full `LC_DYLD_INFO_ONLY` bytecode engine (`rebase`, `bind`, `weak_bind`, eager `lazy_bind`) and export trie lookup |
| **805-Symbol Resolver & ObjC/OS Bridge** | `src/loader/symbol_resolver.cpp` / `src/loader/undef_805_table.inc` | Complete `805/805` undefined symbol table (`140` external ObjC classes + `140` metaclasses, `14` Group-A + `98` Group-B globals, `16` `std::__1`/`__cxxabiv1` symbols, `60` internal ObjC classes, Darwin `stat`/`mmap`/`sysconf`/`pthread`/Mach VM shims) |
| **Darwin-to-AAPCS64 Variadic & ObjC ASM** | `src/loader/variadic_trampolines.S` / `src/loader/variadic_wrappers.c` | `12` variadic + `2` `va_list` Darwin-stack-to-AAPCS64 bridges plus ARM64 `objc_msgSend`/`objc_msgSendSuper2` preserving `x0..x8` and `q0..q7` |
| **Unified C++ Exception Unwind Bridge** | `src/loader/unwind_bridge.cpp` | Registers `19` Mach-O `__eh_frame` FDEs and synthesizes `3,951` DWARF FDEs from `__unwind_info` compact unwind entries (`838` LSDAs) into `-static-libstdc++` `libunwind` |

---

## 2. Segment Mapping & Bionic TLS (`TPIDR_EL0`) Safety
1. **FairPlay Verification**: `macho_load_and_prepare()` inspects `LC_ENCRYPTION_INFO_64` (`cmd = 0x2c`) and enforces `cryptid == 0` (`cryptoff = 16384`, `cryptsize = 42008576`).
2. **Contiguous Virtual Address Reservation**: Reserves the full `53.23 MiB` Mach-O virtual span (`0x100000000 .. 0x10353c000`, `span = 0x353c000`, `slide = 0x0`) using anonymous `mmap`, then maps each segment with `MAP_FIXED`:
   - `__TEXT`: `0x100000000 .. 0x102b9c000` (`45,727,744` B, initially `PROT_READ | PROT_WRITE` during fixup/cache clear, then `mprotect`ed to `PROT_READ | PROT_EXEC` + `__builtin___clear_cache`)
   - `__DATA`: `0x102b9c000 .. 0x103384000` (`file_size = 0x5ec000` mapped from file, `0x1fc000` bytes of `__bss`/`__common` tail zero-filled at `0x103188000 .. 0x103384000`, `PROT_READ | PROT_WRITE`)
   - `__LINKEDIT`: `0x103384000 .. 0x10353c000` (`file_size = 0x1b5730`, `PROT_READ`)
3. **Zero `TPIDRRO_EL0` Conflict with Android Bionic**: Scans all `10,503,788` 32-bit instructions in `__TEXT,__text` (`0x100006788 .. 0x1028187e0`) for `MRS Xn, TPIDRRO_EL0` (`(insn & 0xffffffe0) == 0xd53bd060`), confirming `0` occurrences so Bionic's `TPIDR_EL0` thread pointer remains completely untouched.

---

## 3. `LC_DYLD_INFO_ONLY` Fixups & 805/805 Symbol Resolution
`dyld_apply_all_fixups()` processes all four `LC_DYLD_INFO_ONLY` streams with `0` missing symbols:
- **Rebase (`520,119` pointers)**: Adds `slide` (`0x0` when mapped at preferred base `0x100000000`) across all `__DATA` interior pointers.
- **Bind (`4,040` writes)**: Resolves external symbol names and writes `target_addr + (int64_t)addend`, including all `219` non-zero `BIND_OPCODE_SET_ADDEND_SLEB` (`+16`) bindings onto `_ZTVN10__cxxabiv1{17__class,20__si_class,21__vmi_class}_type_infoE`.
- **Eager Lazy Bind (`539` symbols)**: Binds all `539` `__DATA,__la_symbol_ptr` slots eagerly at load time, bypassing `dyld_stub_binder` at runtime.
- **Weak Bind (`672` writes)**: Resolves coalesced C++ weak definitions (`operator new`, `operator delete`, inline RTTI/vtables) across `__DATA,__const`, `__DATA,__data`, and `__DATA,__bss`.

---

## 4. Darwin-to-AAPCS64 Variadic Trampolines & Unified Unwind Bridge
- **Variadic & `va_list` ABI Translation (`12 + 2` functions)**:
  - On Darwin ARM64, variadic arguments are always spilled contiguously onto the stack at `[sp, #0]`, whereas Android AAPCS64 passes the first 8 GP registers (`x0..x7`) in registers.
  - `src/loader/variadic_trampolines.S` and `src/loader/variadic_wrappers.c` bridge all `12` variadic imports (`_printf`, `_fprintf`, `_sprintf`, `_snprintf`, `___sprintf_chk`, `___snprintf_chk`, `_sscanf`, `_NSLog`, `_open`, `_fcntl`, `_ioctl`, `_syscall`) plus `2` `va_list` imports (`_vfprintf`, `_vsnprintf`, where Darwin `va_list` is a `char *` pointer and Android AAPCS64 `va_list` is a 32-byte `struct __va_list`) and Objective-C `+[NSString stringWithFormat:]` / `-[NSMutableString appendFormat:]`.
- **Unified `-static-libstdc++` Unwind Bridge**:
  - Links NDK `libc++_static.a`, `libc++abi.a`, and `libunwind.a` into a single unwinder instance so `_Unwind_Resume`, `__gxx_personality_v0`, `__dynamic_cast`, and `__register_frame` share the exact same FDE B-tree.
  - Registers all `19` existing `__TEXT,__eh_frame` FDEs (`3` CIEs) and synthesizes `3,951` self-contained DWARF FDEs (`185,844` bytes) from `__TEXT,__unwind_info` (`3,200` frameless + `751` FP-based `0x01`/`0x02`/`0x03` encodings, preserving all `838` `__TEXT,__gcc_except_tab` LSDA pointers).

---

## 5. Runtime ABI Bugs Discovered & Fixed During On-Device Verification
During on-device execution of the `466` `__mod_init_func` initializers on Samsung Galaxy A56 (`SM-A566B`), two subtle Darwin-vs-Linux ABI incompatibilities in UE4's `FMallocBinned2` initializer (`mod_init_idx=12` at `0x10059c244` -> `FMemory_GCreateMalloc` -> `FIOSPlatformMemory::GetConstants` / `FMallocBinned2::FMallocBinned2`) were identified and fixed in `src/loader/symbol_resolver.cpp`:
1. **`_host_statistics` Byte-Count vs Word-Count Stack Overflow (`0x100aae244`, `0x100aae0a4`)**:
   - UE4 `FIOSPlatformMemory::GetConstants` allocates a 60-byte `vm_statistics_data_t` on the stack (`sp + 0x40`, immediately below saved `x20, x19, x29, x30` at `sp + 0x80`) and initializes `mach_msg_type_number_t Count = sizeof(vm_statistics_data_t)` (`60` bytes) instead of `HOST_VM_INFO_COUNT` (`15` `natural_t` words).
   - Zeroing `*count_inout * sizeof(int32_t)` (`60 * 4 = 240` bytes) overflowed the 60-byte stack buffer and zeroed the caller's saved frame pointer (`x29`) and link register (`x30`).
   - **Fix**: Clamped `darwin_host_statistics` to `HOST_VM_INFO_COUNT = 15` words (`60` bytes) and `darwin_task_info` to `MACH_TASK_BASIC_INFO_COUNT = 12` words (`48` bytes).
2. **Darwin `MAP_ANON` (`0x1000`) & 64 KiB `FMallocBinned2` Pool Alignment (`0x100ac9238`, `0x100ac9670`)**:
   - Darwin defines `MAP_ANON = 0x1000` (and encodes Mach VM tags via `VM_MAKE_TAG(tag)` in the upper 8 bits of `fd`), whereas Linux defines `MAP_ANONYMOUS = 0x0020`. Furthermore, UE4 `FMallocBinned2` on iOS expects 64 KiB-aligned anonymous VM allocations, whereas Android's 4 KiB kernel page size returns 4 KiB-aligned `mmap` pointers.
   - **Fix**: Implemented `darwin_mmap`, `darwin_vm_allocate`, and `darwin_sysconf` (translating Darwin `_SC_PAGESIZE = 29` and `_SC_NPROCESSORS_ONLN = 58`), translating `MAP_ANON (0x1000)` to Linux `MAP_ANONYMOUS (0x20)` and trimming over-allocated anonymous mappings to 64 KiB (`0x10000`) alignment.

---

## 6. Device Verification Output (`scripts/gate_m1.ps1` on Galaxy A56 `RRCY40086KE`)
Verified via `powershell -ExecutionPolicy Bypass -File scripts/gate_m1.ps1` (`GATE M1: PASS`, exit code `0`):

```text
[OH2_M1] [M1_PASS] 1_FAIRPLAY_AND_MMAP: cryptid=0 base=0x100000000 span=0x353c000 tpidrro_el0_reads=0
[OH2_M1] [M1_PASS] 2_DYLD_FIXUPS: rebase=520119 bind=4040 addend16=219 lazy_bind=539 weak_bind=672 missing_symbols=0
[OH2_M1] [M1_PASS] 3_UNWIND_AND_OBJC: eh_frame_fdes=19 synth_fdes=3951 lsdas=838 objc_classes=60 objc_cats=2 selrefs=1491 cfstrings=245
[OH2_UE4] [UE4] Used memory before allocating anything was 0.00MB
[OH2_M1] [M1_PASS] 4_MOD_INIT_FUNCS: executed=466/466
[OH2_UE4] [UE4] Checking for command line in /data/local/tmp/OH2.app/ue4commandline.txt... NOT FOUND!
[OH2_UE4] [UE4] Combined iOS Commandline:
[OH2_UE4] ========= This app is in LANDSCAPE mode
[OH2_UE4] [UE4] Device Type: iPhone12,1
[OH2_UE4] [UE4] LogInit: Oceanhorn 2 UE4.23 Mach-O M1 Runtime Active on Android ARM64
[OH2_M1] [M1_PASS] 5_LC_MAIN_UE4_STARTUP: entryoff=0x58e3bc return_code=0
[OH2_M1] [M1_STATE] M1_LOADER_VERIFIED_OK
```
