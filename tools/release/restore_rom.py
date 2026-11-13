"""Rebuild the ROM from the DD2_ROM_0..7 secrets (see upload-rom-secret.sh).

    python tools/release/restore_rom.py dd2.nes [parts.b64]

The release workflow writes the secrets into a file (Windows limits one
environment variable to 32,767 characters, less than a part); without a file
the parts are read from DD2_ROM_0..7 environment variables. The result
must be Double Dragon II - The Revenge (USA) (Rev 1); anything else stops the
build.
"""
import base64
import hashlib
import lzma
import os
import sys

# SHA-256 of the ROM after its 16-byte iNES header (the header varies by dump
# tool; the game data does not).
EXPECTED = "c564704993cf79c94db5c2954792e99e86100586d0ef1d4bf1aefc6d008d4c0f"


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "dd2.nes"
    if len(sys.argv) > 2:
        with open(sys.argv[2], encoding="ascii") as f:
            text = "".join(f.read().split())
    else:
        parts = [os.environ.get(f"DD2_ROM_{i}", "") for i in range(8)]
        text = "".join(p.strip() for p in parts)
    if not text:
        print("No DD2_ROM_* secrets: run tools/release/upload-rom-secret.sh first",
              file=sys.stderr)
        return 1
    rom = lzma.decompress(base64.b64decode(text))
    digest = hashlib.sha256(rom[16:]).hexdigest()
    if digest != EXPECTED:
        print(f"The stored ROM is not DD2 (USA) (Rev 1): {digest}", file=sys.stderr)
        return 1
    with open(out, "wb") as f:
        f.write(rom)
    print(f"ROM restored ({len(rom)} bytes, verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
