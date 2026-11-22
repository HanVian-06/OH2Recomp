# Objective-C Metadata Analysis (`Payload/OH2.app/OH2`)

**Reproducible Verification Script**: `python scripts/analysis/inspect_objc.py` (supports `--json`)  
**Target Binary (Read-Only)**: `Payload/OH2.app/OH2`

---

## 1. ObjC Mach-O Sections (`13` Sections)
- `__TEXT,__objc_methname`: `45,921` B (`0x1028e09e0`)
- `__TEXT,__objc_classname`: `2,090` B (`0x1028ebd41`)
- `__TEXT,__objc_methtype`: `30,315` B (`0x1028ec56b`)
- `__DATA,__objc_classlist`: `480` B (`0x10312ec30`) -> **`60` classes**
- `__DATA,__objc_catlist`: `16` B (`0x10312ee10`) -> **`2` categories**
- `__DATA,__objc_protolist`: `224` B (`0x10312ee20`) -> **`28` protocols**
- `__DATA,__objc_imageinfo`: `8` B (`0x10312ef00`)
- `__DATA,__objc_const`: `51,048` B (`0x10312ef08`)
- `__DATA,__objc_selrefs`: `11,928` B (`0x10313b670`) -> **`1,491` selectors** (`1,491` unique)
- `__DATA,__objc_classrefs`: `1,536` B (`0x10313e508`) -> **`192` classrefs** (`134` external across `16` frameworks, `58` local)
- `__DATA,__objc_superrefs`: `416` B (`0x10313eb08`) -> **`52` superrefs**
- `__DATA,__objc_ivar`: `904` B (`0x10313eca8`)
- `__DATA,__objc_data`: `4,800` B (`0x10313f030`)

---

## 2. Defined ObjC Classes (`60`), Categories (`2`), Protocols (`28`)
- **UE4 App / UI / Input / Lifecycle (`8`)**: `IOSAppDelegate`, `FIOSView`, `IOSViewController`, `FIOSFramePacer`, `SlateTextField`, `FIOSAsyncTask`, `IndexedPosition`, `IndexedRange`
- **UE4 Metal RHI / Apple Platform (`11`)**: `FApplePlatformObject`, `FApplePlatformObjectZombie`, `FMetalResourceTracker`, `FMetalResourceTrackCount`, `FMetalCommandBufferDebug`, `FMetalDebugFence`, `FMetalDeallocHandler`, `FMetalShaderPipeline`, `FMetalDeferredStats`, `FMetalBufferData`, `CommandBufferFenceImpl`
- **UE4 Accessibility (`3`)**: `FIOSAccessibilityCache`, `FIOSAccessibilityContainer`, `FIOSAccessibilityLeaf`
- **UE4 AVFoundation Media (`3`)**: `FAVPlayerDelegate`, `FAVMediaAssetResourceLoaderDelegate`, `FAVPlayerItemLegibleOutputPushDelegate`
- **UE4 Network / HTTP (`2`)**: `FHttpResponseAppleWrapper`, `BackgroundDownloadDelegate`
- **UE4 OnlineSubsystemIOS (`9`)**: `FAppStoreUtils`, `FSKProductsRequestHelper`, `FStoreKitHelper`, `FStoreKitHelperV2`, `IOSCloudStorage`, `FGameCenterSessionDelegate`, `FGameCenterSessionDelegateMC`, `FTurnBasedEventListenerIOS`, `FTurnBasedMatchmakerDelegateIOS`
- **Third-Party Crash Reporter (`24`)**: `PLCrashReporter`, `PLCrashReporterConfig`, `PLCrashSignalHandler`, `PLCrashReport*` (`14` classes), `PLCrashMachException*` (`3` classes), `PLCrashProcessInfo`, `PLCrashHostInfo`, `PLCrashUncaughtExceptionHandler`
- **Categories (`2`)**:
  - `NSObject (IMetalDebugGroupAssociation)`
  - `NSString (FString_Extensions)`
- **Protocols (`28`)**: `PLCrashReportFormatter`, `NSFastEnumeration`, `NSObject`, `MCSessionDelegate`, `SKRequestDelegate`, `SKProductsRequestDelegate`, `SKPaymentTransactionObserver`, `GKChallengeListener`, `GKInviteEventListener`, `GKTurnBasedEventListener`, `GKSavedGameListener`, `GKLocalPlayerListener`, `GKTurnBasedMatchmakerViewControllerDelegate`, `AVAssetResourceLoaderDelegate`, `AVPlayerItemOutputPushDelegate`, `AVPlayerItemLegibleOutputPushDelegate`, `MTLFence`, `UIApplicationDelegate`, `GKGameCenterControllerDelegate`, `UNUserNotificationCenterDelegate`, `UITextFieldDelegate`, `NSURLSessionDelegate`, `NSURLSessionTaskDelegate`, `NSURLSessionDownloadDelegate`, `UITextInputTraits`, `UIKeyInput`, `UITextInput`, `UIAlertViewDelegate`

---

## 3. External Bound ObjC Classrefs (`134` across `16` Frameworks)

| Framework | Classrefs | Role in M2 / M5 |
|---|---:|---|
| `Metal` | 26 | **Real Vulkan Shim (`src/metal_vk/`)**: `MTLRenderPipelineDescriptor`, `MTLTextureDescriptor`, `MTLDepthStencilDescriptor`, `MTLSamplerDescriptor`, `MTLRenderPassDescriptor`, `MTLVertexDescriptor`, etc. |
| `Foundation` | 24 | **Real Shim (`src/runtime/`)**: `NSString`, `NSBundle`, `NSFileManager`, `NSNotificationCenter`, `NSThread`, `NSOperationQueue`, `NSAutoreleasePool`, `NSError`, `NSNumber`, `NSURLSession`, etc. |
| `CoreFoundation` | 19 | **Real Shim (`src/runtime/`)**: `NSArray`, `NSDictionary`, `NSData`, `NSDate`, `NSURL`, `NSUserDefaults`, `NSLocale`, `NSRunLoop`, `NSTimer`, `NSSet`, etc. |
| `UIKit` | 15 | **Real Bridge + Stubs**: `UIApplication`, `UIWindow`, `UIView`, `UIScreen`, `UIDevice`, `UIImage`, `UIFont` (real); `UIAlertController`, `UIActivityViewController`, haptic generators (minimal/stub) |
| `GameKit` | 11 | **Stub**: `GKLocalPlayer`, `GKAchievement`, `GKLeaderboard`, `GKScore`, `GKTurnBasedMatch`, etc. |
| `AVFoundation` | 9 | **Audio/Media Shim**: `AVAudioSession`, `AVAudioPlayer`, `AVPlayer`, `AVAssetReader`, etc. |
| `CloudKit` | 7 | **Stub**: `CKContainer`, `CKDatabase`, `CKRecord`, `CKQuery`, etc. |
| `StoreKit` | 6 | **Stub**: `SKPaymentQueue`, `SKProductsRequest`, `SKPayment`, `SKStoreReviewController`, etc. |
| `UserNotifications` | 5 | **Stub**: `UNUserNotificationCenter`, `UNNotificationRequest`, etc. |
| `CoreImage` | 3 | **Minimal/Stub**: `CIContext`, `CIFilter`, `CIImage` |
| `QuartzCore` | 3 | **Real Bridge**: `CAMetalLayer`, `CADisplayLink` (real); `CAEAGLLayer` (unused/minimal) |
| `MultipeerConnectivity` | 2 | **Stub**: `MCPeerID`, `MCSession` |
| `DeviceCheck` | 1 | **Stub**: `DCDevice` |
| `GameController` | 1 | **Input Bridge/Stub**: `GCController` |
| `CoreMotion` | 1 | **Minimal/Stub**: `CMMotionManager` |
| `OpenGLES` | 1 | **Minimal/Unused**: `EAGLContext` |

---

## 4. M2 ObjC Runtime & `libobjc` Imports (`29`)
- `OH2` imports `29` symbols from `/usr/lib/libobjc.A.dylib`, including `_objc_msgSend`, `_objc_msgSendSuper2` (`52` superrefs), ARC (`_objc_retain`, `_objc_release`, `_objc_autorelease`, `_objc_autoreleasePoolPush/Pop`), property accessors, associated objects, `_objc_sync_enter/exit`, and `_OBJC_CLASS_$_NSObject`.
- Exact dynamic selector dispatch graph at runtime is `"BELUM DIKETAHUI"` until M2 execution (`objc_msgSend` runtime trace / unimplemented selector log).
