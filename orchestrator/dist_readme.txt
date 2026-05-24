DotaFarm — Production Bundle
==================================

УСТАНОВКА
---------
1. Распакуйте zip в любую папку (например C:\DotaFarm). Полный путь не должен
   содержать кириллицы — Steam и Dota плохо переносят non-ASCII пути в command
   line. Английская папка обязательна.
2. Запустите DotaFarm.exe от имени администратора (требуется для symlink'ов
   BotSteam, инжекта DLL и WinDivert driver).

ПЕРВЫЙ ЗАПУСК
-------------
1. Откройте config\accounts.json и заполните Steam credentials для 5
   (или 10 — если 5+5 self-play) аккаунтов. Шаблон уже создан.
2. Откройте config\farm.json — настройте relay credentials (получите user_id
   и user_auth_token от admin'а relay-сервиса), heroes, region.
3. Кнопка Start в DotaFarm GUI.

ЗАВИСИМОСТИ
-----------
Никаких. Bundle полностью self-contained:
  - DotaFarm.exe — static-linked /MT, не требует VC++ Redistributable.
  - Andromeda-Dota2-Base.dll — static-linked /MT.
  - dota2_minify_wrapper.exe — PyInstaller bundle, не требует Python.
  - sing-box.exe + wintun.dll — vendored в bundle.
  - WinDivert.dll + WinDivert64.sys — vendored в bundle (legacy path).

ТРЕБОВАНИЯ К СИСТЕМЕ
--------------------
- Windows 10 1809+ или Windows 11 (x64).
- 16 GB RAM (для 5 ботов) / 32 GB RAM (для 10 ботов self-play).
- Steam установлен и хотя бы один раз залогинен.
- Dota 2 установлен (любой Steam library, главное чтобы был установлен).
- Стабильный интернет (10+ Mbit рекомендуется).

ОПЦИОНАЛЬНЫЕ ДОПОЛНЕНИЯ
------------------------
1. handle64.exe (Sysinternals) — на 99% машин не нужен, DotaFarm использует
   native NtQuerySystemInformation для kill dota_singleton_mutex. Если в
   логах увидите "KillMutexNative failed" — скачайте handle64.exe с
   https://learn.microsoft.com/sysinternals/downloads/handle и положите в
   корень рядом с DotaFarm.exe.

2. HwidSpoofer.exe — kernel-level HWID spoofer, нужен только для antiban на
   аккаунтах под угрозой. Per-process HWID (через ProxyHook.dll) включён по
   умолчанию и достаточен в 95% случаев.

ОЧИСТКА И ОБНОВЛЕНИЕ
--------------------
- DotaFarm не пишет в реестр и не создаёт глобальных файлов вне C:\BotSteam,
  C:\BotProfiles, C:\BotDota и C:\temp\andromeda. Удалить = снести эти 4
  директории + папку с DotaFarm.exe.
- Обновление: распакуйте новый zip ПОВЕРХ старой папки (config/ файлы не
  затрутся; .dist_version в scripts/bots/ скажет orchestrator'у пересинкать
  Lua boт-логику в C:\temp\andromeda\scripts).

ПОДДЕРЖКА
---------
[admin contact placeholder]
