"""Pre-build script: convert HTML files to C source with gzip-compressed byte arrays.

Accepts CLI arguments for input directory, output directory, and product name.
Reads .html and related files from the input directory and generates corresponding
_gz.c files in the output directory that can be compiled as part of a component.
Avoids PlatformIO's EMBED_TXTFILES path issues. Compresses with gzip (level 9)
before embedding.
"""
import os
import gzip
import sys
import argparse

# Support both standalone execution and PlatformIO pre-build script usage
try:
    Import("env")
except NameError:
    # Running standalone, not in PlatformIO SCons context
    pass


def main():
    parser = argparse.ArgumentParser(
        description="Embed HTML/CSS/JS files as gzip-compressed C byte arrays"
    )
    parser.add_argument(
        "--input-dir",
        required=True,
        help="Directory containing HTML/CSS/JS files to embed"
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory where generated .c files will be written"
    )
    parser.add_argument(
        "--product-name",
        default="firmware",
        help="Product name for auto-generated header comment (default: firmware)"
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="Tuples of (filename, c_variable_name) to embed (can pass as pairs)"
    )

    args = parser.parse_args()

    # Parse file pairs from positional arguments
    # Expected format: file1.html var1_name file2.html var2_name ...
    files = []
    for i in range(0, len(args.files), 2):
        if i + 1 < len(args.files):
            files.append((args.files[i], args.files[i + 1]))
        elif i < len(args.files):
            print(f"embed_html: warning: {args.files[i]} has no variable name, skipping")

    # Ensure output directory exists
    os.makedirs(args.output_dir, exist_ok=True)

    for html_name, var_name in files:
        html_path = os.path.join(args.input_dir, html_name)
        c_path = os.path.join(args.output_dir, f"{var_name}.c")

        if not os.path.exists(html_path):
            print(f"embed_html: {html_path} not found, skipping")
            continue

        # Skip if .c is newer than .html
        if os.path.exists(c_path):
            if os.path.getmtime(c_path) >= os.path.getmtime(html_path):
                continue

        with open(html_path, "rb") as f:
            raw_data = f.read()

        # Compress with gzip at maximum compression
        data = gzip.compress(raw_data, compresslevel=9)

        with open(c_path, "w") as out:
            out.write(f"// Auto-generated from {html_name} (gzip-compressed) for {args.product_name} — do not edit\n")
            out.write(f"const unsigned char {var_name}[] = {{\n")
            for i, b in enumerate(data):
                out.write(f"0x{b:02x},")
                if (i + 1) % 16 == 0:
                    out.write("\n")
            out.write("};\n")
            out.write(f"const unsigned int {var_name}_len = sizeof({var_name});\n")

        print(f"embed_html: {html_name} -> {c_path} ({len(raw_data)} bytes -> {len(data)} bytes compressed)")


if __name__ == "__main__":
    main()
