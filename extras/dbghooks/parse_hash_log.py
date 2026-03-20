#!/usr/bin/env python3
"""
Hash Discovery Log Parser
Parses hash_discovery.log and generates structured output for integration into nevr-server

Usage:
    python3 parse_hash_log.py hash_discovery.log
    python3 parse_hash_log.py hash_discovery.log --format cpp
    python3 parse_hash_log.py hash_discovery.log --format yaml
    python3 parse_hash_log.py hash_discovery.log --filter sns
    python3 parse_hash_log.py hash_discovery.log --filter symbols
"""

import re
import sys
import argparse
from collections import defaultdict
from typing import Dict, List, Tuple


class HashEntry:
    def __init__(self, hash_type: str, name: str, result: str, extra: dict = None):
        self.hash_type = hash_type
        self.name = name
        self.result = result
        self.extra = extra or {}

    def __repr__(self):
        return f"{self.hash_type}({self.name}) -> {self.result}"


def parse_log_file(filepath: str) -> Dict[str, List[HashEntry]]:
    """Parse hash_discovery.log into structured data"""

    sns_messages = {}  # name -> hash
    symbols = {}  # name -> hash
    cms_hashes = {}  # intermediate hashes
    sms_hashes = {}  # final hashes

    with open(filepath, "r") as f:
        for line in f:
            line = line.strip()

            # Skip comments and empty lines
            if not line or line.startswith("#"):
                continue

            # Parse SNS_COMPLETE (SNS message with string)
            match = re.match(r'\[SNS_COMPLETE\] "([^"]+)" -> (0x[0-9a-f]+)', line)
            if match:
                name, hash_val = match.groups()
                sns_messages[name] = hash_val
                continue

            # Parse CMatSym_Hash (intermediate)
            match = re.match(r'\[CMatSym_Hash\] "([^"]+)" -> (0x[0-9a-f]+)', line)
            if match:
                name, hash_val = match.groups()
                cms_hashes[name] = hash_val
                continue

            # Parse SMatSymData_HashA (final with seed)
            match = re.match(
                r"\[SMatSymData_HashA\] seed=(0x[0-9a-f]+), hash=(0x[0-9a-f]+) -> (0x[0-9a-f]+)",
                line,
            )
            if match:
                seed, input_hash, result = match.groups()
                sms_hashes[result] = {"seed": seed, "input": input_hash}
                continue

            # Parse CSymbol64_Hash (symbols, replicated variables, assets)
            match = re.match(
                r'\[CSymbol64_Hash\] "([^"]+)" -> (0x[0-9a-f]+) \(seed=(0x[0-9a-f]+)\)(.*)$',
                line,
            )
            if match:
                name, hash_val, seed, flags = match.groups()
                is_default = "[DEFAULT_SEED]" in flags
                symbols[name] = {
                    "hash": hash_val,
                    "seed": seed,
                    "default_seed": is_default,
                }
                continue

    return {
        "sns_messages": sns_messages,
        "symbols": symbols,
        "cms_intermediate": cms_hashes,
        "sms_final": sms_hashes,
    }


def format_cpp_header(data: Dict) -> str:
    """Generate C++ header with hash definitions"""

    output = []
    output.append("// Generated from hash_discovery.log")
    output.append("// DO NOT EDIT - Regenerate from log file\n")
    output.append("#pragma once\n")
    output.append("#include <cstdint>\n")

    # SNS Messages
    if data["sns_messages"]:
        output.append(
            "// ============================================================================"
        )
        output.append("// SNS Message Hashes")
        output.append(
            "// ============================================================================\n"
        )
        output.append("namespace SNSMessageHash {")

        for name in sorted(data["sns_messages"].keys()):
            hash_val = data["sns_messages"][name]
            const_name = re.sub(r"[^A-Za-z0-9_]", "_", name)
            output.append(
                f'    constexpr uint64_t {const_name} = {hash_val}ULL;  // "{name}"'
            )

        output.append("}\n")

    # Replicated Variables (symbols with default seed)
    replicated_vars = {
        k: v for k, v in data["symbols"].items() if v.get("default_seed", False)
    }
    if replicated_vars:
        output.append(
            "// ============================================================================"
        )
        output.append("// Replicated Variable Hashes (seed=0xFFFFFFFFFFFFFFFF)")
        output.append(
            "// ============================================================================\n"
        )
        output.append("namespace ReplicatedVarHash {")

        for name in sorted(replicated_vars.keys()):
            hash_val = replicated_vars[name]["hash"]
            const_name = re.sub(r"[^A-Za-z0-9_]", "_", name)
            output.append(
                f'    constexpr uint64_t {const_name} = {hash_val}ULL;  // "{name}"'
            )

        output.append("}\n")

    return "\n".join(output)


def format_yaml(data: Dict) -> str:
    """Generate YAML format"""

    output = []
    output.append("# Generated from hash_discovery.log")
    output.append("# DO NOT EDIT - Regenerate from log file\n")

    # SNS Messages
    if data["sns_messages"]:
        output.append("sns_messages:")
        for name in sorted(data["sns_messages"].keys()):
            hash_val = data["sns_messages"][name]
            output.append(f'  - name: "{name}"')
            output.append(f"    hash: {hash_val}")
            output.append("")

    # Replicated Variables
    replicated_vars = {
        k: v for k, v in data["symbols"].items() if v.get("default_seed", False)
    }
    if replicated_vars:
        output.append("replicated_variables:")
        for name in sorted(replicated_vars.keys()):
            hash_val = replicated_vars[name]["hash"]
            output.append(f'  - name: "{name}"')
            output.append(f"    hash: {hash_val}")
            output.append("")

    return "\n".join(output)


def format_summary(data: Dict) -> str:
    """Generate human-readable summary"""

    output = []
    output.append("=" * 80)
    output.append("Hash Discovery Summary")
    output.append("=" * 80)
    output.append("")

    # Statistics
    output.append("Statistics:")
    output.append(f"  SNS Messages:           {len(data['sns_messages'])}")
    output.append(f"  Symbols (total):        {len(data['symbols'])}")

    replicated_vars = {
        k: v for k, v in data["symbols"].items() if v.get("default_seed", False)
    }
    output.append(f"  Replicated Variables:   {len(replicated_vars)}")

    output.append(f"  CMatSym (intermediate): {len(data['cms_intermediate'])}")
    output.append(f"  SMatSymData (final):    {len(data['sms_final'])}")
    output.append("")

    # SNS Messages
    if data["sns_messages"]:
        output.append("SNS Messages (sample, first 20):")
        for name in sorted(data["sns_messages"].keys())[:20]:
            hash_val = data["sns_messages"][name]
            output.append(f"  {name:40} -> {hash_val}")

        if len(data["sns_messages"]) > 20:
            output.append(f"  ... and {len(data['sns_messages']) - 20} more")
        output.append("")

    # Replicated Variables
    if replicated_vars:
        output.append("Replicated Variables (sample, first 20):")
        for name in sorted(replicated_vars.keys())[:20]:
            hash_val = replicated_vars[name]["hash"]
            output.append(f"  {name:40} -> {hash_val}")

        if len(replicated_vars) > 20:
            output.append(f"  ... and {len(replicated_vars) - 20} more")
        output.append("")

    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser(
        description="Parse hash_discovery.log and generate structured output"
    )
    parser.add_argument("logfile", help="Path to hash_discovery.log")
    parser.add_argument(
        "--format",
        choices=["summary", "cpp", "yaml"],
        default="summary",
        help="Output format (default: summary)",
    )
    parser.add_argument(
        "--filter",
        choices=["all", "sns", "symbols", "replicated"],
        default="all",
        help="Filter output (default: all)",
    )

    args = parser.parse_args()

    # Parse log file
    try:
        data = parse_log_file(args.logfile)
    except FileNotFoundError:
        print(f"Error: File not found: {args.logfile}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error parsing file: {e}", file=sys.stderr)
        sys.exit(1)

    # Apply filter
    if args.filter == "sns":
        data["symbols"] = {}
    elif args.filter in ["symbols", "replicated"]:
        data["sns_messages"] = {}
        if args.filter == "replicated":
            # Keep only symbols with default seed
            data["symbols"] = {
                k: v for k, v in data["symbols"].items() if v.get("default_seed", False)
            }

    # Generate output
    if args.format == "cpp":
        output = format_cpp_header(data)
    elif args.format == "yaml":
        output = format_yaml(data)
    else:
        output = format_summary(data)

    print(output)


if __name__ == "__main__":
    main()
