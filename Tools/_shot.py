import ctypes, sys, time
from ctypes import wintypes
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass
TARGET_PID = int(sys.argv[1])
OUT = sys.argv[2]
TITLE = "ADS1115 上位机"
user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
found = []


@ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def cb(hwnd, lp):
    if user32.IsWindowVisible(hwnd):
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, b, n + 1)
            if b.value == TITLE:
                pid = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                found.append((hwnd, pid.value))
    return True


user32.EnumWindows(cb, 0)
m = [h for h, p in found if p == TARGET_PID]
if not m:
    print("NOTFOUND")
    sys.exit(1)
hwnd = m[0]
user32.ShowWindow(hwnd, 9)
user32.SetWindowPos(hwnd, ctypes.c_void_p(-1), 60, 60, 0, 0,
                    0x0001 | 0x0004 | 0x0010)
user32.SetForegroundWindow(hwnd)
time.sleep(2)
rect = wintypes.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rect))
w, h = rect.right - rect.left, rect.bottom - rect.top


class BMIH(ctypes.Structure):
    _fields_ = [("a", wintypes.DWORD), ("bw", wintypes.LONG),
                ("bh", wintypes.LONG), ("p", wintypes.WORD),
                ("bc", wintypes.WORD), ("c", wintypes.DWORD),
                ("si", wintypes.DWORD), ("x", wintypes.LONG),
                ("y", wintypes.LONG), ("cu", wintypes.DWORD),
                ("ci", wintypes.DWORD)]


bmi = BMIH()
bmi.a = ctypes.sizeof(BMIH)
bmi.bw = w
bmi.bh = -h
bmi.p = 1
bmi.bc = 32
hdc = user32.GetWindowDC(hwnd)
mem = gdi32.CreateCompatibleDC(hdc)
bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
gdi32.SelectObject(mem, bmp)
user32.PrintWindow(hwnd, mem, 2)
buf = ctypes.create_string_buffer(w * h * 4)
gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
from PIL import Image
img = Image.frombuffer("RGB", (w, h), buf.raw, "raw", "BGRX", 0, 1)
img.save(OUT)
print("SAVED", img.size)
