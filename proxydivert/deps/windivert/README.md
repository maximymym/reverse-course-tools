# WinDivert vendored deps

This directory should contain WinDivert 2.2.2 release binaries:

- `WinDivert.dll`     — x64 user-mode lib
- `WinDivert.lib`     — import lib for linker
- `WinDivert64.sys`   — signed kernel driver (Microsoft-compat cert from basil00)
- `WinDivert.h`       — header (already vendored, MIT/LGPL)

## Download

```
curl -L https://github.com/basil00/WinDivert/releases/download/v2.2.2/WinDivert-2.2.2-A.zip -o wd.zip
unzip wd.zip
cp WinDivert-2.2.2-A/x64/WinDivert.{dll,lib} .
cp WinDivert-2.2.2-A/x64/WinDivert64.sys .
```

## Why vendored as binaries

WinDivert kernel driver must be signed by Microsoft to load on production
Win10/11 (testsigning is not an option for ban-sensitive products). The release
zip from basil00 is the only redistributable form with valid signature.

License: BSD-3 (header) + LGPL-3 (binary lib) — internal/private use OK.
