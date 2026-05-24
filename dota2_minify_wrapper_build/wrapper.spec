# PyInstaller spec — Bundle H: standalone .exe wrapper для dota2_minify.
#
# Build:
#   cd tools/dota2
#   pip install pyinstaller vpk vdf
#   pyinstaller --clean dota2_minify_wrapper_build/wrapper.spec --distpath dist --workpath build_pyi
#
# Result:
#   tools/dota2/dist/dota2_minify_wrapper.exe (~10-25 MB single file)
#
# Smoke test:
#   ./dist/dota2_minify_wrapper.exe --help
#   ./dist/dota2_minify_wrapper.exe status
#
# Vendor (dota2_minify/Minify/{mods,bin/blank-files}) НЕ упакован внутрь .exe —
# package.sh копирует его рядом, чтобы можно было обновлять без rebuild .exe.

import os

# spec-файл выполняется PyInstaller'ом из tools/dota2/dota2_minify_wrapper_build/
HERE = os.path.dirname(os.path.abspath(SPEC))            # noqa: F821
SCRIPT = os.path.normpath(os.path.join(HERE, "..", "dota2_minify_wrapper.py"))

block_cipher = None

a = Analysis(
    [SCRIPT],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=[
        "vpk",
        "vdf",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # Exclude tk/numpy/etc — wrapper им не пользуется. Уменьшает размер на ~10MB.
    excludes=[
        "tkinter",
        "_tkinter",
        "Tkinter",
        "matplotlib",
        "numpy",
        "pandas",
        "PIL",
        "scipy",
        "dearpygui",
        "lib2to3",
        "test",
        "unittest",
        "pydoc_data",
        "xml.dom.expatbuilder",
    ],
    noarchive=False,
    cipher=block_cipher,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="dota2_minify_wrapper",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    # UPX отключён намеренно — даёт false positives на multiple antivirus engines
    # (PyInstaller + UPX = известный malware-pattern). Размер +5-8MB, но AV clean.
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
