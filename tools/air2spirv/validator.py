#!/usr/bin/env python3
"""
SPIR-V Vulkan 1.1 Validator wrapper:
- Uses C:\\VulkanSDK\\1.4.350.0\\Bin\\SPIRV-Tools-shared.dll (spvValidateBinary, target-env vulkan1.1)
  for zero-overhead in-process validation of all 4,544 modules.
- Also provides subprocess wrapper for C:\\VulkanSDK\\1.4.350.0\\Bin\\spirv-val.exe --target-env vulkan1.1.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
from typing import Tuple

VULKAN_SDK = os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.350.0")
SPIRV_VAL_EXE = os.path.join(VULKAN_SDK, "Bin", "spirv-val.exe")
SPIRV_TOOLS_DLL = os.path.join(VULKAN_SDK, "Bin", "SPIRV-Tools-shared.dll")


class _SpvPosition(ctypes.Structure):
    _fields_ = [
        ("line", ctypes.c_size_t),
        ("column", ctypes.c_size_t),
        ("index", ctypes.c_size_t),
    ]


class _SpvDiagnostic(ctypes.Structure):
    _fields_ = [
        ("position", _SpvPosition),
        ("error", ctypes.c_char_p),
        ("isTextSource", ctypes.c_bool),
    ]


class SpirvValidator:
    """In-process SPIRV-Tools validator targeting vulkan1.1 (SPIR-V 1.3)."""

    def __init__(self, target_env: str = "vulkan1.1"):
        self.target_env = target_env
        self._dll = None
        self._ctx = None
        if os.path.isfile(SPIRV_TOOLS_DLL):
            dll = ctypes.CDLL(SPIRV_TOOLS_DLL)
            dll.spvParseTargetEnv.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
            dll.spvParseTargetEnv.restype = ctypes.c_bool
            dll.spvContextCreate.argtypes = [ctypes.c_int]
            dll.spvContextCreate.restype = ctypes.c_void_p
            dll.spvContextDestroy.argtypes = [ctypes.c_void_p]
            dll.spvContextDestroy.restype = None
            dll.spvValidateBinary.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_uint32),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.POINTER(_SpvDiagnostic)),
            ]
            dll.spvValidateBinary.restype = ctypes.c_int
            dll.spvDiagnosticDestroy.argtypes = [ctypes.POINTER(_SpvDiagnostic)]
            dll.spvDiagnosticDestroy.restype = None

            env_val = ctypes.c_int(0)
            if not dll.spvParseTargetEnv(target_env.encode("ascii"), ctypes.byref(env_val)):
                raise RuntimeError(f"spvParseTargetEnv failed for {target_env}")
            self._ctx = dll.spvContextCreate(env_val.value)
            self._dll = dll

    def close(self) -> None:
        if self._dll is not None and self._ctx is not None:
            self._dll.spvContextDestroy(self._ctx)
            self._ctx = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def validate(self, spirv_bytes: bytes) -> Tuple[bool, str]:
        if not spirv_bytes or (len(spirv_bytes) & 3) != 0:
            return False, f"Invalid SPIR-V byte length: {len(spirv_bytes)}"
        if self._dll is not None and self._ctx is not None:
            word_count = len(spirv_bytes) // 4
            buf = (ctypes.c_uint32 * word_count).from_buffer_copy(spirv_bytes)
            diag_ptr = ctypes.POINTER(_SpvDiagnostic)()
            rc = self._dll.spvValidateBinary(self._ctx, buf, word_count, ctypes.byref(diag_ptr))
            if rc == 0:
                if diag_ptr:
                    self._dll.spvDiagnosticDestroy(diag_ptr)
                return True, ""
            err_msg = f"spvValidateBinary rc={rc}"
            if diag_ptr:
                d = diag_ptr.contents
                err_str = d.error.decode("utf-8", errors="replace") if d.error else ""
                err_msg = f"[word {d.position.index}] {err_str}"
                self._dll.spvDiagnosticDestroy(diag_ptr)
            return False, err_msg
        return validate_with_cli(spirv_bytes, self.target_env)


def validate_with_cli(spirv_bytes: bytes, target_env: str = "vulkan1.1") -> Tuple[bool, str]:
    """Validate SPIR-V binary bytes via spirv-val.exe --target-env vulkan1.1 stdin."""
    proc = subprocess.run(
        [SPIRV_VAL_EXE, "--target-env", target_env, "-"],
        input=spirv_bytes,
        capture_output=True,
    )
    if proc.returncode == 0:
        return True, ""
    msg = (proc.stderr or proc.stdout).decode("utf-8", errors="replace").strip()
    return False, msg
