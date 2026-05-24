#!/usr/bin/env python3
"""Encrypt an arbitrary binary for embedding as RT_RCDATA in DotaFarm.exe.

Layout (matches payload_loader::LoadEmbeddedResource on the C++ side):
  [0..4)    magic (4 ASCII bytes — distinguishes artifact kind)
  [4..16)   IV (12 random bytes)
  [16..)    ciphertext || 16-byte GCM tag  (Go gcm.Seal layout)

Magic vocabulary (must stay in sync with payload_loader.h):
  DFDL  — DLLs (Andromeda, ProxyHook)
  DFSP  — Spoofer family (spoofer.sys, kdu.exe)

Key derivation:
  key = HKDF_SHA256(
            IKM  = kEmbedSecret (read from src/crypto/embed_secret.h),
            salt = --salt   (per-artifact, e.g. "dotafarm-sys-v1"),
            info = --info   (per-artifact, e.g. "sys-embed"),
            len  = 32)

Usage:
  python encrypt_resource.py --in <plain.bin> --out <encrypted.bin> \\
      --secret-header <src/crypto/embed_secret.h> \\
      [--magic DFDL] [--salt dotafarm-dll-v1] [--info dll-embed]

Defaults reproduce the DFDL/DLL pipeline so existing CMake invocations
without --magic/--salt/--info keep working.

Requires: cryptography (`pip install cryptography`).
"""
import argparse
import re
import secrets
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives import hashes, hmac
except ImportError:
    sys.stderr.write(
        "ERROR: install dependency first: pip install cryptography\n")
    sys.exit(2)


IV_LEN = 12
KEY_LEN = 32
MAGIC_LEN = 4
DEFAULT_MAGIC = "DFDL"
DEFAULT_SALT = "dotafarm-dll-v1"
DEFAULT_INFO = "dll-embed"


def parse_secret_header(path: Path) -> bytes:
    """Pull kEmbedSecret[] bytes out of crypto/embed_secret.h."""
    src = path.read_text(encoding="utf-8")
    m = re.search(r"kEmbedSecret\s*\[\s*32\s*\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        raise RuntimeError(f"could not find kEmbedSecret in {path}")
    body = m.group(1)
    nums = re.findall(r"0x[0-9A-Fa-f]{1,2}", body)
    if len(nums) != 32:
        raise RuntimeError(
            f"expected 32 bytes, found {len(nums)} in {path}")
    return bytes(int(n, 16) for n in nums)


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    """Single-block HKDF (length must be <= 32)."""
    if length > 32:
        raise ValueError("length must be <= 32")
    h = hmac.HMAC(salt, hashes.SHA256())
    h.update(ikm)
    prk = h.finalize()
    h2 = hmac.HMAC(prk, hashes.SHA256())
    h2.update(info + b"\x01")
    return h2.finalize()[:length]


def main() -> int:
    p = argparse.ArgumentParser(
        description="Encrypt an arbitrary binary for RC embedding")
    p.add_argument("--in", dest="in_path", required=True,
                   help="plaintext input file (DLL / SYS / EXE)")
    p.add_argument("--out", dest="out_path", required=True,
                   help="encrypted output (.bin)")
    p.add_argument("--secret-header", dest="secret_header", required=True,
                   help="path to src/crypto/embed_secret.h")
    p.add_argument("--magic", default=DEFAULT_MAGIC,
                   help=f"4-byte ASCII magic header (default: {DEFAULT_MAGIC})")
    p.add_argument("--salt", default=DEFAULT_SALT,
                   help=f"HKDF salt (default: {DEFAULT_SALT})")
    p.add_argument("--info", default=DEFAULT_INFO,
                   help=f"HKDF info string (default: {DEFAULT_INFO})")
    args = p.parse_args()

    if len(args.magic) != MAGIC_LEN or not args.magic.isascii():
        sys.stderr.write(
            f"ERROR: --magic must be exactly {MAGIC_LEN} ASCII bytes, "
            f"got '{args.magic}' ({len(args.magic)}B)\n")
        return 2

    src = Path(args.in_path)
    dst = Path(args.out_path)
    if not src.is_file():
        sys.stderr.write(f"ERROR: input not found: {src}\n")
        return 1

    secret = parse_secret_header(Path(args.secret_header))
    salt_bytes = args.salt.encode("utf-8")
    info_bytes = args.info.encode("utf-8")
    magic_bytes = args.magic.encode("ascii")

    key = hkdf_sha256(secret, salt_bytes, info_bytes, KEY_LEN)
    iv = secrets.token_bytes(IV_LEN)

    plain = src.read_bytes()
    cipher = AESGCM(key).encrypt(iv, plain, associated_data=None)
    # cipher already = ciphertext || tag (the cryptography lib appends tag).

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(magic_bytes + iv + cipher)

    total = len(plain) + MAGIC_LEN + IV_LEN + 16
    sys.stdout.write(
        f"[+] {src.name} ({len(plain):,}B) -> {dst.name} "
        f"({total:,}B encrypted, magic={args.magic})\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
