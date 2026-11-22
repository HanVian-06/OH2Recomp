# M2 Objective-C Runtime Bridge, Darwin `libc_wrapper`, Framework Stubs & `.pak` Verification (`M2_RUNTIME_SHIM`)

## 1. Architecture Overview
Milestone M2 extends M1's headless Mach-O loader (`src/loader/`) with a full Darwin ABI-compatible POSIX wrapper (`src/libc_wrapper/`), ARM64 Objective-C dispatch & Foundation/UIKit/GCD runtime bridges (`src/runtime/`), offline framework stubs, and an Android verification runner/APK (`android/m2_runtime/`).

| Component | Path | Responsibility |
|---|---|---|
| **M1 Non-Breaking Hooks** | `src/loader/macho_loader.h`, `variadic_wrappers.c`, `symbol_resolver.cpp` | Extension hooks (`oh2_symbol_register_override`, method registration, toll-free CF <-> Foundation helpers, bundle root setter, `open` interceptor) preserving `gate_m1.ps1` verbatim when M2 is inactive |
| **Errno Wrapper** | `src/libc_wrapper/errno_wrapper.c` | Bidirectional Darwin <-> Linux errno translation for `___error()` (e.g., Linux `EAGAIN = 11` <-> Darwin `35`, `EDEADLK = 35` <-> `11`, `EINPROGRESS = 115` <-> `36`, `ETIMEDOUT = 110` <-> `60`) |
| **BSD Socket Wrapper** | `src/libc_wrapper/socket_wrapper.c` | Translates BSD `sockaddr_in`/`sockaddr_in6` (`sin_len` prefix, `AF_INET6 = 30`), `SOL_SOCKET = 0xffff`, `SO_NOSIGPIPE`, `getaddrinfo`/`DarwinAddrInfo`, `getsockname`/`getpeername`/`sendto`/`recvfrom` |
| **Stat & Dirent64 Wrapper** | `src/libc_wrapper/stat_wrapper.c` | Translates Linux `struct stat` -> 144-byte `DarwinStat64`, and per-stream `DIR *` Bionic `dirent` -> Darwin 64-bit `DarwinDirent64` (`d_name` at byte offset `21`) |
| **Open, Path & Pak I/O Telemetry** | `src/libc_wrapper/open_wrapper.c` | Darwin open flags (`O_NONBLOCK`, `O_APPEND`, `O_CREAT`, `O_TRUNC`, `O_EXCL`, `O_NOCTTY`, `O_DIRECTORY`, `O_CLOEXEC`), `@executable_path/` & `/var/.../OH2.app/` -> `/sdcard/Android/data/com.oceanhorn2.port/files/`, multi-component case-insensitive lookup, `/cookeddata/` fallback, 16 KiB-aligned `oh2_darwin_malloc`, and live `oh2-ios.pak` FD/offset telemetry |
| **Pthread & Sync Wrapper** | `src/libc_wrapper/pthread_wrapper.c` | Lazy-initialization of Darwin static mutexes (`0x32aaaba0..0x32aaabaf`), static conds (`0x3cb0b1b0..0x3cb0b1bf`), static rwlocks (`0x2da8b3b0`), Darwin `RECURSIVE = 2` -> Bionic `1`, 64-bit zero-extended TLS keys, 2 MiB minimum worker stack, 15-char safe `pthread_setname_np` |
| **ObjC ARM64 ASM Dispatch** | `src/runtime/objc_msgSend_arm64.S` | `oh2_objc_msgSend`, `oh2_objc_msgSendSuper2`, `oh2_objc_msgSend_stret` saving/restoring `x0..x8` and full 128-bit `q0..q7` (224-byte stack frame) across lookup, plus `oh2_asm_msgsend_abi_probe` verification trampoline |
| **ObjC/Blocks/GCD Runtime** | `src/runtime/objc_runtime.cpp` | Recursive `@synchronized` locks, thread-local `NSAutoreleasePool` stack, heap ARC refcounts, `_Block_copy`/`_Block_release` (copy/dispose helpers & `BLOCK_IS_GLOBAL`), `dispatch_once_f`/`dispatch_async`/`dispatch_sync`/main-queue draining/timed semaphores |
| **UIKit & QuartzCore Bridges** | `src/runtime/ui_bridges.cpp` | `UIApplication`, `UIWindow`, `UIView`, `UIViewController` (lazy view), `UIScreen` (`2340x1080`, `scale = 2.0`), `UIDevice` (`iPhone`, `16.0`), `CAMetalLayer`, and `CADisplayLink` real monotonic worker thread |
| **Foundation Bridges** | `src/runtime/ns_bridges.cpp` | `NSBundle` (`/sdcard/Android/data/com.oceanhorn2.port/files`), `NSFileManager`, `NSThread` (spawning `MainAppThread:` on real POSIX thread with >= 8 MiB stack), `NSTimer` + `NSRunLoop` main-thread timer drain, `NSUserDefaults`, `NSDate`, `NSNumber`, `NSData`, `NSURL`, `NSArray`/`NSDictionary`/`NSSet`, `NSString` canonical mapping + UTF-8/UTF-16 decoding |
| **Framework Stubs & Isolation** | `src/runtime/framework_stubs.cpp` | Inert offline stubs for `GameKit`, `StoreKit`, `CloudKit`, `MultipeerConnectivity`, `UserNotifications`, `DeviceCheck`, `AVFoundation`, `Mach` exception ports (`_exception_raise*`, `_mach_port_*`, `_mach_msg`), and post-load `PLCrashReporter` isolation |
| **Runner & Android APK** | `android/m2_runtime/`, `scripts/gate_m2.ps1` | Builds `liboh2_runtime.so`, `oh2_m2_runner`, and signed `com.oceanhorn2.port.m2runtime` APK (`m2_runtime-debug.apk`), stages bundle/pak files, runs all 14 `--test` checks & full gate |

---

## 2. Apple OSS Evaluation Summary
Evaluated Apple OSS `objc4`, `swift-corelibs-foundation` / `CoreFoundation`, `libdispatch`, and `libclosure` (`Blocks`) vs GNUstep `libobjc2` (see **ADR-007** in `docs/DECISIONS.md`):
- **GNUstep `libobjc2` rejected**: Incompatible 64-bit class/`ivar`/`method_list_t` and SEL ABI (`OH2` Mach-O sections use Apple's non-fragile ARM64 Mach-O layout checked by `static_assert` in `symbol_resolver.cpp`: 40-byte `objc_class`, 72-byte `class_ro_t`).
- **Full `objc4` + CoreFoundation build skipped for M2**: Building upstream `objc4`/`libdispatch` cleanly on Android Bionic without Mach kernel IPC/XPC/mach_ports requires rewriting the same OS primitives anyway, whereas extending M1's exact Mach-O 64-bit `DarwinObjCClass` registry with full ARM64 `q0..q7`/`x0..x8`-preserving ASM dispatch, real Blocks/GCD, toll-free CF bridging, and Mach-port neutralization avoids Mach-port kernel dependencies completely.

---

## 3. Live `MainAppThread:` Bringup Fixes Discovered on Galaxy A56
During real execution of `IOSAppDelegate`'s `MainAppThread:` (`0x100dbbb38`) on `SM-A566B`, live tracing exposed five real Darwin runtime requirements needed for UE4 4.23 to discover and mount `oh2-ios.pak`:
1. **`NSTimer` Execution on Main RunLoop (`NoUrlCommandLine`)**: `[IOSAppDelegate application:didFinishLaunchingWithOptions:]` schedules `[NSTimer scheduledTimerWithTimeInterval:0.01 target:self selector:@selector(NoUrlCommandLine) ...]` and blocks `MainAppThread:` until `bCommandLineReady` is set. Implementing real `NSTimer` storage and draining due timers in `oh2_runtime_wait_for_pak_or_boundary()` unblocked `MainAppThread:`.
2. **`-[NSString precomposedStringWithCanonicalMapping]` & `+[NSMutableArray arrayWithCapacity:]`**: Returning `0` (`nil`) from `precomposedStringWithCanonicalMapping` during UE4's `FIOSFileManager::IterateDirectory` scan of `/sdcard/Android/data/com.oceanhorn2.port/files/cookeddata/oh2/content/paks/` discarded every discovered filename, causing UE4 to skip `oh2-ios.pak`. Implementing `precomposedStringWithCanonicalMapping` and `arrayWithCapacity:` allowed UE4 to find `oh2-ios.pak`.
3. **Darwin 21-byte-header `struct dirent` (`DarwinDirent64`)**: Bionic places `d_name` at offset `19`, whereas Darwin places `d_seekoff` + `d_namlen` ahead of `d_name` at offset `21`. Per-stream wrapping in `oh2_darwin_opendir`/`oh2_darwin_readdir`/`oh2_darwin_closedir` preserves exact Darwin filenames.
4. **`-[NSObject performSelectorOnMainThread:withObject:waitUntilDone:]`**: Marshaled via condition-variable task queue drained by the main thread alongside `dispatch_async(dispatch_get_main_queue(), ...)`.
5. **16 KiB `oh2_darwin_malloc` Alignment**: `FMallocBinned2::Realloc` (`0x100ae78f8`) masks allocations against iOS `0x4000` (16 KiB) page assumptions; routing `malloc(size >= 0x4000)` through `posix_memalign` (`0x4000` / `0x10000`) satisfies iOS alignment on Android's 4 KiB kernel.

---

## 4. Verification Output (`scripts/gate_m2.ps1` on Galaxy A56 `RRCY40086KE`)
Verified all `14 / 14` `--test <name>` subtests (`objc_metadata_registration`, `objc_msgsend_abi`, `objc_arc_sync`, `blocks_gcd`, `objc_oh2_smoke`, `libc_file_stat`, `libc_sockaddr`, `libc_pthread_errno`, `nsbundle_filemanager`, `uikit_cadisplaylink`, `stubs_gamekit_storekit`, `stubs_cloud_mc_un_dc`, `stub_plcrashreporter`, `pak_extract_verify`) AND full `scripts/gate_m2.ps1` (`GATE M2: PASS`, exit `0`) with zero regressions in `scripts/gate_m1.ps1` (`GATE M1: PASS`) and `python scripts/gate_m0.py` (`GATE M0: PASS`).

Observed live logcat evidence (`build/m2/m2_logcat.txt`):
```text
[OH2_UE4] [UE4] Checking for command line in /sdcard/Android/data/com.oceanhorn2.port/files/ue4commandline.txt... FOUND!
[OH2_UE4] [UE4] Combined iOS Commandline: ../../../OH2/OH2.uproject
[OH2_M1] [M2] Starting NSThread on POSIX thread (target=0xb40000733b3142a0 selector=MainAppThread:)
[OH2_M1] [M2] nsthread entering selector=MainAppThread: target=0xb40000733b3142a0 imp=0x100dbbb38
[OH2_M1] [M2] Firing NSTimer selector=NoUrlCommandLine target=0xb40000733b3142a0
[OH2_M2] [IO_PATH] '/sdcard/Android/data/com.oceanhorn2.port/files/cookeddata/oh2/content/paks/oh2-ios.pak' -> '/sdcard/Android/data/com.oceanhorn2.port/files/cookeddata/oh2/content/paks/oh2-ios.pak'
[OH2_M1] [M2] MainAppThread wait complete (ok=1): opened=1 fstat_size=2430963623 footer=1 index_bytes=3550490 index_complete=1 asset_reads=2 metal_boundary=0
[OH2_M2] [M2_PASS] 5_PAK_OPEN: UE4 opened /sdcard/Android/data/com.oceanhorn2.port/files/cookeddata/oh2/content/paks/oh2-ios.pak magic=0x5a6f12e1 version=8 entries=19920 MountPoint=../../../ fstat_size=2430963623 index_bytes_read=3550490
[OH2_M2] [M2_STATE] M2_RUNTIME_VERIFIED_OK
```
