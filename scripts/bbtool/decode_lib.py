"""Coredump / backtrace decode library — framework-agnostic.

Importable by bbtool commands AND by external tools (e.g. fleet harness) with
zero bbtool CLI dependencies (no imports from cli, registry, or core modules).

Chip → arch mapping
--------------------
  xtensa : ESP32, ESP32-S2, ESP32-S3  (and -S2S / -S3S silicon variants)
  riscv  : ESP32-C3, ESP32-C6, ESP32-H2, ESP32-C2  (and -V silicon variants)

Toolchain search order (find_addr2line)
----------------------------------------
  1. explicit toolchain_path argument
  2. BBTOOL_ADDR2LINE env var  (bb-neutral name)
  3. FLEET_ADDR2LINE env var   (fleet harness compat alias)
  4. Glob ~/.platformio/packages/toolchain-*/bin/<arch-prefix>-esp-elf-addr2line
  5. System PATH (shutil.which)

Exception cause tables
-----------------------
  Xtensa EXCCAUSE register  — common subset; full table in Xtensa ISA ref §4.4.
  RISC-V mcause             — ESP32-C3/C6.
"""
from __future__ import annotations

import logging
import os
import shutil
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Chip → arch mapping
# ---------------------------------------------------------------------------
_XTENSA_CHIPS = {"ESP32", "ESP32S2", "ESP32S2S", "ESP32S3", "ESP32S3S"}
_RISCV_CHIPS  = {"ESP32C2", "ESP32C3", "ESP32C3V", "ESP32C6", "ESP32C6V",
                  "ESP32H2", "ESP32H2V"}


def chip_arch(chip_model: str) -> str:
    """Return 'xtensa' or 'riscv' for a chip_model string from /api/info.

    Normalises the chip_model by upper-casing and stripping spaces/hyphens,
    then checks the riscv set first (more specific), defaulting to xtensa.

    Examples:
        chip_arch("ESP32")     -> "xtensa"
        chip_arch("ESP32-S3")  -> "xtensa"
        chip_arch("ESP32-C3")  -> "riscv"
        chip_arch("ESP32-C6")  -> "riscv"
        chip_arch("ESP32-H2")  -> "riscv"
    """
    norm = chip_model.upper().replace(" ", "").replace("-", "")
    for chip in _RISCV_CHIPS:
        if norm.startswith(chip):
            return "riscv"
    return "xtensa"


# ---------------------------------------------------------------------------
# Toolchain discovery
# ---------------------------------------------------------------------------

def find_addr2line(arch: str, toolchain_path: Optional[str] = None) -> Optional[str]:
    """Locate the addr2line binary for the given arch.

    Search order:
      1. toolchain_path arg (explicit override)
      2. BBTOOL_ADDR2LINE env var  (bb-neutral)
      3. FLEET_ADDR2LINE env var   (fleet harness compat alias)
      4. ~/.platformio/packages/toolchain-*/bin/
      5. System PATH
    """
    if arch == "riscv":
        names = ["riscv32-esp-elf-addr2line"]
    else:
        names = ["xtensa-esp-elf-addr2line"]

    # Explicit overrides (arg first, then env vars)
    candidates = [
        toolchain_path,
        os.environ.get("BBTOOL_ADDR2LINE"),
        os.environ.get("FLEET_ADDR2LINE"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate

    # ~/.platformio/packages/toolchain-*/bin/
    pio_packages = Path.home() / ".platformio" / "packages"
    if pio_packages.is_dir():
        for tc_dir in sorted(pio_packages.glob("toolchain-*")):
            bin_dir = tc_dir / "bin"
            for name in names:
                p = bin_dir / name
                if p.is_file() and os.access(str(p), os.X_OK):
                    return str(p)

    # System PATH
    for name in names:
        found = shutil.which(name)
        if found:
            return found

    return None


# ---------------------------------------------------------------------------
# addr2line invocation
# ---------------------------------------------------------------------------

def addr2line_decode(
    binary: str,
    addresses: List[int],
    addr2line_bin: str,
) -> List[str]:
    """Decode a list of integer PC addresses using addr2line.

    Returns a list of decoded frame strings, one per address.
    Format: "function @ file:line"  or  "?? @ ??:0"  on failure.
    """
    if not addresses:
        return []

    hex_addrs = [hex(a) for a in addresses]
    cmd = [addr2line_bin, "-e", binary, "-f", "-C"] + hex_addrs
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, timeout=30)
        lines = out.decode(errors="replace").splitlines()
    except subprocess.CalledProcessError as exc:
        logger.warning("addr2line failed (rc=%d): %s", exc.returncode,
                       exc.output.decode(errors="replace")[:200])
        return ["?? @ ??:0" for _ in addresses]
    except FileNotFoundError:
        logger.error("addr2line not found: %s", addr2line_bin)
        return ["?? @ ??:0" for _ in addresses]
    except subprocess.TimeoutExpired:
        logger.warning("addr2line timed out")
        return ["?? @ ??:0" for _ in addresses]

    # addr2line -f emits two lines per address: function name, then file:line
    frames: List[str] = []
    for i in range(len(addresses)):
        fn_line  = lines[i * 2]     if i * 2 < len(lines) else "??"
        loc_line = lines[i * 2 + 1] if i * 2 + 1 < len(lines) else "??:0"
        frames.append(f"{fn_line} @ {loc_line}")
    return frames


# ---------------------------------------------------------------------------
# Exception cause name tables
# ---------------------------------------------------------------------------
_XTENSA_CAUSE_NAMES = {
    0:  "IllegalInstruction",
    1:  "Syscall",
    2:  "InstructionFetchError",
    3:  "LoadStoreError",
    4:  "Level1Interrupt",
    5:  "Alloca",
    6:  "IntegerDivideByZero",
    7:  "PCValueError",
    8:  "PrivilegedInstruction",
    9:  "LoadStoreAlignmentCause",
    12: "InstrPIFDataError",
    13: "LoadStorePIFDataError",
    14: "InstrPIFAddrError",
    15: "LoadStorePIFAddrError",
    16: "InstTLBMiss",
    17: "InstTLBMultiHit",
    18: "InstFetchPrivilege",
    20: "InstFetchProhibited",
    24: "LoadStoreTLBMiss",
    25: "LoadStoreTLBMultiHit",
    26: "LoadStorePrivilege",
    28: "LoadProhibited",
    29: "StoreProhibited",
    32: "Coprocessor0Disabled",
    33: "Coprocessor1Disabled",
    34: "Coprocessor2Disabled",
    35: "Coprocessor3Disabled",
    36: "Coprocessor4Disabled",
    37: "Coprocessor5Disabled",
    38: "Coprocessor6Disabled",
    39: "Coprocessor7Disabled",
    128: "DoubleException",
}

_RISCV_CAUSE_NAMES = {
    0:  "InstructionAddressMisaligned",
    1:  "InstructionAccessFault",
    2:  "IllegalInstruction",
    3:  "Breakpoint",
    4:  "LoadAddressMisaligned",
    5:  "LoadAccessFault",
    6:  "StoreAddressMisaligned",
    7:  "StoreAccessFault",
    8:  "EnvironmentCallFromU",
    9:  "EnvironmentCallFromS",
    11: "EnvironmentCallFromM",
    12: "InstructionPageFault",
    13: "LoadPageFault",
    15: "StorePageFault",
}


def cause_name(exc_cause: int, arch: str = "xtensa") -> str:
    """Return a human-readable exception cause name."""
    table = _RISCV_CAUSE_NAMES if arch == "riscv" else _XTENSA_CAUSE_NAMES
    return table.get(exc_cause, f"Unknown({exc_cause})")


# ---------------------------------------------------------------------------
# High-level decode entry point
# ---------------------------------------------------------------------------

def decode_panic(
    panic: dict,
    elf_path: str,
    arch: str = "xtensa",
    toolchain_path: Optional[str] = None,
) -> "DecodeResult":
    """Decode a panic dict (from /api/diag/panic) using the given ELF.

    panic dict keys used:
      app_sha256, exc_pc, backtrace (list[int|str]), task, exc_cause

    Returns a DecodeResult with decoded frames and metadata.
    """
    addr2line_bin = find_addr2line(arch, toolchain_path)
    if addr2line_bin is None:
        return DecodeResult(
            ok=False,
            error=f"addr2line not found for arch '{arch}'; "
                  "install ESP-IDF toolchain or set BBTOOL_ADDR2LINE",
            elf_path=elf_path,
            arch=arch,
        )

    exc_pc    = panic.get("exc_pc", 0)
    bt        = panic.get("backtrace") or []
    task      = panic.get("task", "")
    exc_cause = panic.get("exc_cause", 0)
    app_sha   = panic.get("app_sha256", "")

    # Collect all PCs: exc_pc first, then backtrace
    all_pcs: List[int] = []
    if exc_pc:
        all_pcs.append(int(exc_pc))
    all_pcs.extend(int(a) for a in bt if a)

    if not all_pcs:
        return DecodeResult(
            ok=True,
            error="no PC addresses to decode",
            elf_path=elf_path,
            arch=arch,
            task=task,
            exc_cause=exc_cause,
            cause_name_str=cause_name(exc_cause, arch),
            app_sha256=app_sha,
        )

    frames = addr2line_decode(elf_path, all_pcs, addr2line_bin)

    labeled: List[Tuple[str, int, str]] = []
    if exc_pc and frames:
        labeled.append(("exc_pc", int(exc_pc), frames[0]))
        bt_frames = frames[1:]
    else:
        bt_frames = frames

    for i, (addr, frame) in enumerate(zip(bt, bt_frames)):
        labeled.append((f"bt[{i}]", int(addr), frame))

    return DecodeResult(
        ok=True,
        elf_path=elf_path,
        arch=arch,
        addr2line=addr2line_bin,
        task=task,
        exc_cause=exc_cause,
        cause_name_str=cause_name(exc_cause, arch),
        app_sha256=app_sha,
        frames=labeled,
    )


# ---------------------------------------------------------------------------
# Result type
# ---------------------------------------------------------------------------

class DecodeResult:
    """Result of decode_panic()."""

    def __init__(
        self,
        ok: bool,
        elf_path: str = "",
        arch: str = "xtensa",
        addr2line: str = "",
        task: str = "",
        exc_cause: int = 0,
        cause_name_str: str = "",
        app_sha256: str = "",
        frames: Optional[List[Tuple[str, int, str]]] = None,
        error: str = "",
    ):
        self.ok = ok
        self.elf_path = elf_path
        self.arch = arch
        self.addr2line = addr2line
        self.task = task
        self.exc_cause = exc_cause
        self.cause_name_str = cause_name_str
        self.app_sha256 = app_sha256
        self.frames = frames or []
        self.error = error
