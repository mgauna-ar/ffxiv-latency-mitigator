#!/usr/bin/env python3
"""
Embeds a binary DLL into a C++ header as a constexpr byte array.
"""
import sys
import os

def embed_file(input_path: str, output_path: str):
    if not os.path.exists(input_path):
        print(f"Error: Input file '{input_path}' not found.", file=sys.stderr)
        sys.exit(1)

    with open(input_path, "rb") as f:
        data = f.read()

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

    with open(output_path, "w", encoding="utf-8") as out:
        out.write("#pragma once\n\n")
        out.write("#include <cstdint>\n")
        out.write("#include <cstddef>\n\n")
        out.write("namespace mitigator::loader {\n\n")
        out.write(f"// Embedded DLL payload size: {len(data)} bytes\n")
        out.write("inline const uint8_t g_embedded_payload_dll[] = {\n")

        line = []
        for i, byte in enumerate(data):
            line.append(f"0x{byte:02x}")
            if len(line) == 16:
                out.write("    " + ", ".join(line) + ",\n")
                line = []
        if line:
            out.write("    " + ", ".join(line) + "\n")

        out.write("};\n\n")
        out.write("inline const size_t g_embedded_payload_dll_size = sizeof(g_embedded_payload_dll);\n\n")
        out.write("} // namespace mitigator::loader\n")

    print(f"[+] Successfully embedded {len(data)} bytes from '{input_path}' into '{output_path}'")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: embed_dll.py <input_dll_path> <output_header_path>", file=sys.stderr)
        sys.exit(1)
    embed_file(sys.argv[1], sys.argv[2])
