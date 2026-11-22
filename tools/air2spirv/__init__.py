"""AIR 2.0 LLVM Bitcode -> Vulkan 1.1 SPIR-V 1.3 Offline Shader Pipeline."""

from .air_to_spirv import translate_air_shader
from .bitcode_reader import IRModule, parse_fmetal_code_header, parse_mtlb_container
from .validator import SpirvValidator, validate_with_cli

__all__ = [
    "IRModule",
    "SpirvValidator",
    "parse_fmetal_code_header",
    "parse_mtlb_container",
    "translate_air_shader",
    "validate_with_cli",
]
