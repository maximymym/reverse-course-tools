"""LoadLibrary inject Andromeda DLL в running dota2.exe."""
import ctypes
import ctypes.wintypes as wt
import sys
import os

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 39904
DLL = sys.argv[2] if len(sys.argv) > 2 else r"C:\temp\andromeda_src\x64\Release\Andromeda-Dota2-Base.dll"

if not os.path.exists(DLL):
    print(f"[E] DLL not found: {DLL}")
    sys.exit(1)

# Use ANSI path (LoadLibraryA — need to be ASCII-safe)
dll_bytes = DLL.encode("mbcs") + b"\x00"
print(f"[*] Injecting {DLL} ({len(dll_bytes)-1} chars) into PID {PID}")

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_READWRITE = 0x04

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.GetModuleHandleA.restype = wt.HANDLE
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wt.HANDLE, ctypes.c_char_p]

hProc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, PID)
if not hProc:
    print(f"[E] OpenProcess failed: {ctypes.get_last_error()}")
    sys.exit(1)
print(f"[+] OpenProcess OK, hProc=0x{hProc:X}")

remote_addr = k32.VirtualAllocEx(hProc, None, len(dll_bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
if not remote_addr:
    print(f"[E] VirtualAllocEx failed: {ctypes.get_last_error()}")
    sys.exit(1)
print(f"[+] VirtualAllocEx OK @ 0x{remote_addr:X}")

written = ctypes.c_size_t(0)
ok = k32.WriteProcessMemory(hProc, remote_addr, dll_bytes, len(dll_bytes), ctypes.byref(written))
if not ok:
    print(f"[E] WriteProcessMemory failed: {ctypes.get_last_error()}")
    sys.exit(1)
print(f"[+] WriteProcessMemory OK ({written.value} bytes)")

hKernel32 = k32.GetModuleHandleA(b"kernel32.dll")
loadlib = k32.GetProcAddress(hKernel32, b"LoadLibraryA")
print(f"[+] LoadLibraryA @ 0x{loadlib:X}")

tid = wt.DWORD(0)
hThread = k32.CreateRemoteThread(hProc, None, 0, loadlib, remote_addr, 0, ctypes.byref(tid))
if not hThread:
    print(f"[E] CreateRemoteThread failed: {ctypes.get_last_error()}")
    sys.exit(1)
print(f"[+] CreateRemoteThread OK, thread tid={tid.value}")

# Wait for LoadLibrary to finish
INFINITE = 0xFFFFFFFF
k32.WaitForSingleObject(hThread, 30000)

exit_code = wt.DWORD(0)
k32.GetExitCodeThread(hThread, ctypes.byref(exit_code))
print(f"[+] LoadLibrary returned hModule=0x{exit_code.value:X}")
if exit_code.value == 0:
    print("[!] LoadLibraryA returned NULL — check that the DLL is valid and not already loaded")
else:
    print(f"[OK] DLL loaded successfully — Andromeda init thread starts now")

k32.CloseHandle(hThread)
k32.CloseHandle(hProc)
