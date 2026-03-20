#!/usr/bin/env python3
"""
DLL Injector Script
Injects dbghooks.dll into echovr.exe using LIEF library
"""

import os
import sys
import argparse
from pathlib import Path

try:
    import lief
except ImportError:
    print("Error: LIEF library not found. Install it with: pip install lief")
    sys.exit(1)


def inject_dll(target_exe: str, dll_name: str, output_exe: str) -> bool:
    """
    Inject a DLL into a target executable using LIEF.

    Args:
        target_exe: Path to the target executable
        dll_name: Name of the DLL to inject (e.g., "dbghooks.dll")
        output_exe: Path for the output patched executable

    Returns:
        True if successful, False otherwise
    """
    try:
        # Verify target executable exists
        if not os.path.exists(target_exe):
            print(f"Error: Target executable not found: {target_exe}")
            return False

        # Verify DLL exists (optional, but recommended)
        dll_dir = os.path.dirname(target_exe)
        dll_path = os.path.join(dll_dir, dll_name)
        if not os.path.exists(dll_path):
            print(f"Warning: DLL not found in executable directory: {dll_path}")
            print(f"The DLL should be placed at: {dll_path}")

        print(f"Loading executable: {target_exe}")
        pe = lief.parse(target_exe)

        if pe is None:
            print("Error: Failed to parse executable")
            return False

        print(f"Adding library: {dll_name}")
        pe.add_library(dll_name)

        # Create output directory if it doesn't exist
        output_dir = os.path.dirname(output_exe)
        if output_dir and not os.path.exists(output_dir):
            os.makedirs(output_dir, exist_ok=True)

        print(f"Writing patched executable: {output_exe}")
        pe.write(output_exe)

        print("✓ Successfully injected DLL!")
        return True

    except Exception as e:
        print(f"Error during DLL injection: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Inject a DLL into an executable using LIEF"
    )
    parser.add_argument(
        "target_exe",
        nargs="?",
        default="echovr.exe",
        help="Target executable path (default: echovr.exe)",
    )
    parser.add_argument(
        "-d",
        "--dll",
        default="dbghooks.dll",
        help="DLL to inject (default: dbghooks.dll)",
    )
    parser.add_argument(
        "-o", "--output", help="Output executable path (default: target_patched.exe)"
    )

    args = parser.parse_args()

    # Determine output path
    output_exe = args.output
    if not output_exe:
        base_name = os.path.splitext(args.target_exe)[0]
        output_exe = f"{base_name}_patched.exe"

    print(f"DLL Injector")
    print(f"=" * 50)
    print(f"Target:     {args.target_exe}")
    print(f"DLL:        {args.dll}")
    print(f"Output:     {output_exe}")
    print(f"=" * 50)

    success = inject_dll(args.target_exe, args.dll, output_exe)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
