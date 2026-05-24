# dota2_minify_wrapper PyInstaller bundle

Bundle H — превращает `dota2_minify_wrapper.py` (Python 3.8+) в standalone
`dota2_minify_wrapper.exe` для portable distribution. Клиент НЕ должен ставить
Python или `pip install vpk` — wrapper'ный .exe self-contained.

## Build

```bash
cd tools/dota2
pip install pyinstaller vpk vdf
pyinstaller --clean dota2_minify_wrapper_build/wrapper.spec --distpath dist --workpath build_pyi
```

Output: `tools/dota2/dist/dota2_minify_wrapper.exe` (~10-25 MB, single file).

## Smoke test

```bash
./dist/dota2_minify_wrapper.exe --help
./dist/dota2_minify_wrapper.exe status
```

Должен запуститься на машине БЕЗ Python в PATH. Проверка:
```bash
where python  # должен быть пустой output (или ENV-only) — wrapper всё равно
              # должен работать.
```

## Как используется orchestrator'ом

`DotaFarm.exe` (после Bundle H) использует `MinifierConfig::wrapperExe`
(default `scripts\dota2_minify_wrapper.exe`) ВМЕСТО `pythonExe` +
`wrapperScript`. Если `wrapperExe` не существует — fallback на python+script
(dev mode). См. `dota_minifier.cpp::RunPython`.

## UPX disabled

`upx=False` в spec намеренно. PyInstaller + UPX часто триггерит false positives
у Defender / Kaspersky / Avast. Размер бандла +5-8MB по сравнению с UPX-сжатым,
но antivirus clean.
