#!/usr/bin/env python3
"""Conservative stack-budget gate for the polling-only Cortex-M0+ C1104 build.

Use GCC .su frames and the linked call graph, including out-of-function tail
branches. For prebuilt libraries without .su, sum all fixed stack allocations
in their Thumb code (an upper bound, not a path-sensitive estimate). Refuse
recursion, unknown indirect calls, dynamic frames and unrecognized stack
manipulation instead of treating them as zero. This is not a general ARM/RTOS
analyzer: custom interrupt handlers and constructors require revisiting it.
"""

import argparse
from bisect import bisect_right
from functools import lru_cache
from pathlib import Path
import re
import subprocess
import sys


def canonical(name):
    # GCC spells optimized clones differently in .su and the ELF symbol table.
    return re.sub(r"\.(constprop|isra)\.\d+", r".\1", name)


def read_frames(directory):
    frames = {}
    files = list(Path(directory).rglob("*.su"))
    if not files:
        raise ValueError("no .su files: compile with -fstack-usage")
    for path in files:
        for line in path.read_text().splitlines():
            location, amount, kind = line.split("\t")
            name = canonical(location.rsplit(":", 1)[1])
            if kind not in ("static", "dynamic,bounded"):
                raise ValueError(f"unbounded stack frame: {name} ({kind})")
            frames[name] = max(frames.get(name, 0), int(amount))
    return frames


def parse_symbols(text):
    symbols = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]+)\s+\w\s+(\S+)", line.strip())
        if match:
            symbols[match[2]] = int(match[1], 16)
    return symbols


def parse_code(text):
    functions = {}
    current = None
    for line in text.splitlines():
        header = re.fullmatch(r"([0-9a-fA-F]+) <([^>]+)>:", line)
        if header:
            current = (canonical(header[2]), [])
            functions[int(header[1], 16)] = current
        instruction = re.match(
            r"\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{4,8}\s+)+"
            r"([a-z][a-z0-9.]*)\s*(.*)", line)
        if instruction and current:
            operands = instruction[3].split(";")[0].strip()
            current[1].append((int(instruction[1], 16), instruction[2], operands))
    if not functions:
        raise ValueError("no disassembly found")
    return functions


BRANCHES = {"b", "bl", "blx", "beq", "bne", "bcs", "bcc", "bhs", "blo",
            "bmi", "bpl", "bvs", "bvc", "bhi", "bls", "bge", "blt", "bgt", "ble"}


def direct_target(mnemonic, operands):
    if mnemonic.split(".")[0] not in BRANCHES:
        return None
    match = re.match(r"([0-9a-fA-F]+)\s+<", operands)
    return int(match[1], 16) if match else None


def library_frame(name, instructions):
    allocations = {}
    for addr, mnemonic, operands in instructions:
        op = mnemonic.split(".")[0]
        if op == "push":
            regs = re.fullmatch(r"\{([^}]+)\}", operands)
            if not regs or "-" in regs[1]:
                raise ValueError(f"unsupported push in {name}: {operands}")
            allocations[addr] = 4 * len(regs[1].split(","))
        elif re.match(r"sp(?:,|!)", operands):
            size = re.fullmatch(r"sp,\s*(?:sp,\s*)?#(0x[0-9a-f]+|\d+)", operands)
            if op not in ("add", "sub") or not size:
                raise ValueError(f"unsupported stack adjustment in {name}: {mnemonic} {operands}")
            if op == "sub":
                allocations[addr] = int(size[1], 0)
    # Counting each allocation once is only safe if it cannot be repeated.
    for addr, mnemonic, operands in instructions:
        target = direct_target(mnemonic, operands)
        if target is not None and instructions[0][0] <= target <= addr:
            if any(target <= alloc <= addr for alloc in allocations):
                raise ValueError(f"loop crosses a stack allocation in {name}")
    return sum(allocations.values())


def analyze(functions, frames, symbols, root="Reset_Handler"):
    starts = sorted(functions)
    graph = {addr: set() for addr in starts}
    local = {}

    def owner(address):
        index = bisect_right(starts, address) - 1
        if index < 0:
            raise ValueError(f"unresolved code address: {address:x}")
        start = starts[index]
        instructions = functions[start][1]
        if not any(item[0] == address for item in instructions):
            raise ValueError(f"branch to undisassembled code: {address:x}")
        return start

    for start, (name, instructions) in functions.items():
        local[start] = frames[name] if name in frames else library_frame(name, instructions)
        indirect = []
        for addr, mnemonic, operands in instructions:
            op = mnemonic.split(".")[0]
            target = direct_target(mnemonic, operands)
            if target is not None:
                callee = owner(target)
                if callee != start or op in ("bl", "blx"):
                    graph[start].add(callee)
            elif op == "blx":
                indirect.append(addr)
            elif (op == "bx" and operands != "lr") or \
                 (op in ("mov", "add", "ldr") and operands.startswith("pc,")) or \
                 op in ("tbb", "tbh"):
                raise ValueError(f"unresolved indirect branch in {name}: {mnemonic} {operands}")
        if indirect:
            # The supplied startup has exactly two constructor loops. Both
            # arrays must be empty; no reachable indirect call is exempted.
            empty_arrays = all(symbols.get(f"__{kind}_array_start") is not None and
                               symbols[f"__{kind}_array_start"] == symbols.get(f"__{kind}_array_end")
                               for kind in ("preinit", "init"))
            if name != "Reset_Handler" or len(indirect) != 2 or not empty_arrays:
                raise ValueError(f"unresolved indirect calls in {name}")

    # The tiny board polls its peripherals. Only the terminal default handler
    # is allowed; the margin also accommodates hardware exception stacking.
    for name, addr in symbols.items():
        if name.endswith(("_Handler", "IRQHandler")) and name != "Reset_Handler":
            if addr != symbols.get("Default_Handler"):
                raise ValueError(f"custom interrupt handler needs a stack model: {name}")

    visiting = set()

    @lru_cache(None)
    def deepest(node):
        if node in visiting:
            raise ValueError(f"recursive call graph at {functions[node][0]}")
        visiting.add(node)
        paths = [(0, [])] + [deepest(child) for child in sorted(graph[node])]
        depth, path = max(paths, key=lambda item: item[0])
        visiting.remove(node)
        return local[node] + depth, [node] + path

    depth, path = deepest(owner(symbols[root]))
    return depth, " -> ".join(f"{functions[node][0]}({local[node]})" for node in path)


def check_budget(depth, available, margin):
    if depth + margin > available:
        raise ValueError(f"stack budget exceeded: {depth} + {margin} margin > {available} available")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--su-dir", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--margin", type=int, default=64)
    args = parser.parse_args()
    symbols = parse_symbols(subprocess.check_output([args.nm, "-n", args.elf], text=True))
    code = parse_code(subprocess.check_output([args.objdump, "-d", args.elf], text=True))
    depth, path = analyze(code, read_frames(args.su_dir), symbols)
    available = symbols["__StackTop"] - symbols["_stack"]
    print(f"C1104 stack: {depth} B call-chain bound + {args.margin} B margin; {available} B available")
    print(path)
    check_budget(depth, available, args.margin)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f"stack check failed: {exc}")
