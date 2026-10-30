// Drives a portable Notepad++ on a hidden desktop, so GUI tests never touch the user's session:
// no focus stealing, and nothing appears on screen. All calls must come from the thread that
// Run() starts, because window input can only be attached between threads of one desktop.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class NppDriver : IDisposable {
    const uint WM_CLOSE = 0x10, WM_COMMAND = 0x111, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102;
    const uint WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203;
    const uint WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MOUSEWHEEL = 0x20A, WM_SETTEXT = 0xC;
    public const uint NPPMSG = 0x400 + 1000;
    public const uint NPPM_GETCURRENTVIEW = NPPMSG + 88, NPPM_GETCURRENTBUFFERID = NPPMSG + 60, NPPM_MENUCOMMAND = NPPMSG + 48;
    public const uint SCI_GETLENGTH = 2006, SCI_GETCHARAT = 2007, SCI_GETCURRENTPOS = 2008, SCI_GETREADONLY = 2140;
    public const uint SCI_GOTOPOS = 2025, SCI_GETMODIFY = 2159, SCI_GETSELECTIONEMPTY = 2650;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; public int Width => Right - Left; public int Height => Bottom - Top; public override string ToString() => $"({Left},{Top},{Right},{Bottom})"; }
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    struct GUITHREADINFO { public int cbSize, flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }

    delegate bool EnumProc(IntPtr hwnd, IntPtr l);
    [DllImport("user32", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr mode, int flags, uint access, IntPtr sa);
    [DllImport("user32")] static extern bool SetThreadDesktop(IntPtr desktop);
    [DllImport("user32")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32")] static extern bool EnumDesktopWindows(IntPtr desktop, EnumProc proc, IntPtr l);
    [DllImport("user32")] static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr l);
    [DllImport("user32")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32")] static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32")] static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
    [DllImport("user32")] static extern bool MoveWindow(IntPtr hwnd, int x, int y, int w, int h, bool repaint);
    public void Resize(int width, int height) { MoveWindow(Main, 0, 0, width, height, true); Settle(30); }
    [DllImport("user32")] static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] static extern IntPtr SendText(IntPtr hwnd, uint msg, IntPtr w, string l);
    [DllImport("user32")] static extern bool SendMessageTimeoutW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32")] static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern bool PostThreadMessageW(int thread, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32")] static extern bool GetGUIThreadInfo(int thread, ref GUITHREADINFO info);
    [DllImport("user32")] static extern bool AttachThreadInput(int from, int to, bool attach);
    [DllImport("user32")] static extern bool SetKeyboardState(byte[] state);
    [DllImport("user32")] static extern bool GetKeyboardState(byte[] state);
    [DllImport("user32")] static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32")] static extern IntPtr GetMenu(IntPtr hwnd);
    [DllImport("user32")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32")] static extern IntPtr GetSubMenu(IntPtr menu, int pos);
    [DllImport("user32")] static extern uint GetMenuItemID(IntPtr menu, int pos);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetMenuStringW(IntPtr menu, uint id, StringBuilder text, int size, uint flags);
    [DllImport("user32")] static extern uint GetMenuState(IntPtr menu, uint id, uint flags);
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, int flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32")] static extern int GetCurrentThreadId();
    [DllImport("kernel32")] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("user32")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32")] static extern uint GetDpiForWindow(IntPtr hwnd);

    public int Dpi => (int)GetDpiForWindow(Main);
    public int Scale(int v) => (int)Math.Round((double)v * Dpi / 96);

    readonly IntPtr desktop;
    public readonly string DesktopName;
    public Process Process;
    IntPtr processHandle;
    public int Thread0;
    public IntPtr Main, Sci0, Sci1;
    bool attached;

    public NppDriver(string desktopName) {
        DesktopName = desktopName;
        desktop = CreateDesktopW(desktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
        if (desktop == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
        if (!SetThreadDesktop(desktop)) throw new Exception("SetThreadDesktop failed: " + Marshal.GetLastWin32Error());
    }

    public void Start(string exe, string arguments, int width = 1500, int height = 900) {
        var si = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>(), lpDesktop = DesktopName };
        var cmd = new StringBuilder("\"" + exe + "\" " + arguments);
        if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, System.IO.Path.GetDirectoryName(exe), ref si, out PROCESS_INFORMATION pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        Process = Process.GetProcessById(pi.dwProcessId);
        processHandle = pi.hProcess;
        Main = IntPtr.Zero;
        attached = false;  // the link to the previous process's input went with it

        for (int i = 0; i < 200 && Main == IntPtr.Zero; ++i) {
            System.Threading.Thread.Sleep(100);
            EnumDesktopWindows(desktop, (h, l) => {
                GetWindowThreadProcessId(h, out int pid);
                if (pid == Process.Id && ClassOf(h) == "Notepad++") { Main = h; return false; }
                return true;
            }, IntPtr.Zero);
        }
        if (Main == IntPtr.Zero) throw new Exception("Notepad++ main window did not appear");
        Thread0 = GetWindowThreadProcessId(Main, out _);
        Thread.Sleep(1200);
        MoveWindow(Main, 0, 0, width, height, true);
        var editors = Children(Main, "Scintilla");
        Sci0 = editors[0]; Sci1 = editors[1];
        Thread.Sleep(300);
    }

    public static string ClassOf(IntPtr h) { var b = new StringBuilder(128); GetClassNameW(h, b, 128); return b.ToString(); }
    public static string TextOf(IntPtr h) { var b = new StringBuilder(512); GetWindowTextW(h, b, 512); return b.ToString(); }
    public static bool Visible(IntPtr h) => IsWindowVisible(h);
    public static bool Exists(IntPtr h) => IsWindow(h);
    public static RECT RectOf(IntPtr h) { GetWindowRect(h, out RECT r); return r; }
    public static RECT ClientOf(IntPtr h) { GetClientRect(h, out RECT r); return r; }

    public List<IntPtr> Children(IntPtr parent, string className) {
        var found = new List<IntPtr>();
        EnumChildWindows(parent, (h, l) => { if (className == null || ClassOf(h) == className) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }

    public List<IntPtr> TopLevel(string className) {
        var found = new List<IntPtr>();
        EnumDesktopWindows(desktop, (h, l) => {
            GetWindowThreadProcessId(h, out int pid);
            if (pid == Process.Id && (className == null || ClassOf(h) == className) && IsWindowVisible(h)) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public long Send(IntPtr h, uint msg, long w = 0, long l = 0) {
        if (!SendMessageTimeoutW(h, msg, (IntPtr)w, (IntPtr)l, 0, 15000, out IntPtr r)) throw new TimeoutException($"Notepad++ did not answer message 0x{msg:X} within 15 s");
        return (long)r;
    }
    public void Post(IntPtr h, uint msg, long w = 0, long l = 0) => PostMessageW(h, msg, (IntPtr)w, (IntPtr)l);
    public void SetText(IntPtr h, string text) => SendText(h, WM_SETTEXT, IntPtr.Zero, text);

    // GetWindowText cannot read a control of another process; WM_GETTEXT is marshalled and can.
    public string ControlText(IntPtr h) {
        var b = new StringBuilder(1024);
        SendGetText(h, 0x0D, (IntPtr)1024, b);
        return b.ToString();
    }
    [DllImport("user32", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] static extern IntPtr SendGetText(IntPtr hwnd, uint msg, IntPtr w, StringBuilder l);

    public IntPtr Focus() {
        var info = new GUITHREADINFO { cbSize = Marshal.SizeOf<GUITHREADINFO>() };
        GetGUIThreadInfo(Thread0, ref info);
        return info.hwndFocus;
    }

    // Waits until Notepad++ has gone through its queue, posted messages included.
    public void Settle(int extraMs = 0) {
        Send(Main, 0);
        if (extraMs > 0) Thread.Sleep(extraMs);
        Send(Main, 0);
    }

    void WithModifiers(bool ctrl, bool shift, bool alt, Action action) {
        if (!attached) attached = AttachThreadInput(GetCurrentThreadId(), Thread0, true);
        var state = new byte[256];
        GetKeyboardState(state);
        state[0x11] = (byte)(ctrl ? 0x80 : 0); state[0x10] = (byte)(shift ? 0x80 : 0); state[0x12] = (byte)(alt ? 0x80 : 0);
        SetKeyboardState(state);
        action();
        Settle(60);
        state[0x11] = state[0x10] = state[0x12] = 0;
        SetKeyboardState(state);
    }

    public void Key(int vk, bool ctrl = false, bool shift = false, bool alt = false) {
        var target = Focus();
        if (target == IntPtr.Zero) target = Main;
        long l = 1L | ((long)vk << 16);
        WithModifiers(ctrl, shift, alt, () => { Post(target, WM_KEYDOWN, vk, l); Post(target, WM_KEYUP, vk, l | (3L << 30)); });
    }

    public void Char(char c) { var target = Focus(); Post(target, WM_CHAR, c, 1); Settle(30); }

    // For a real popup menu, whose loop takes keys from the thread's queue: nothing may wait on it.
    public void MenuKey(int vk) {
        var target = Focus();
        Post(target, WM_KEYDOWN, vk, 1);
        Post(target, WM_KEYUP, vk, 0xC0000001L);
        Thread.Sleep(40);
    }

    [DllImport("user32")] static extern uint GetGuiResources(IntPtr process, uint flags);
    public uint GdiObjects => GetGuiResources(processHandle, 0);

    static long Lp(int x, int y) => (long)(((uint)y << 16) | ((uint)x & 0xFFFF));
    public void Click(IntPtr h, int x, int y, bool ctrl = false, bool shift = false) {
        long keys = 1 | (ctrl ? 8 : 0) | (shift ? 4 : 0);
        WithModifiers(ctrl, shift, false, () => { Post(h, WM_LBUTTONDOWN, keys, Lp(x, y)); Post(h, WM_LBUTTONUP, keys & ~1L, Lp(x, y)); });
    }
    public void DoubleClick(IntPtr h, int x, int y) {
        Post(h, WM_LBUTTONDOWN, 1, Lp(x, y)); Post(h, WM_LBUTTONUP, 0, Lp(x, y));
        Post(h, WM_LBUTTONDBLCLK, 1, Lp(x, y)); Post(h, WM_LBUTTONUP, 0, Lp(x, y));
        Settle(60);
    }
    public void Drag(IntPtr h, int x0, int y0, int x1, int y1) {
        Post(h, WM_LBUTTONDOWN, 1, Lp(x0, y0));
        Settle(20);
        int steps = 6;
        for (int i = 1; i <= steps; ++i) { Post(h, WM_MOUSEMOVE, 1, Lp(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)); Settle(15); }
        Post(h, WM_LBUTTONUP, 0, Lp(x1, y1));
        Settle(60);
    }
    public void RightClick(IntPtr h, int x, int y) { Post(h, WM_RBUTTONDOWN, 2, Lp(x, y)); Post(h, WM_RBUTTONUP, 0, Lp(x, y)); Thread.Sleep(250); }
    public void Wheel(IntPtr h, int delta, int x = 200, int y = 200, int keys = 0) { Post(h, WM_MOUSEWHEEL, (long)(uint)(((delta & 0xFFFF) << 16) | keys), Lp(x, y)); Settle(40); }

    public Dictionary<string, uint> PluginMenu(string pluginName) {
        var items = new Dictionary<string, uint>();
        var bar = GetMenu(Main);
        for (int i = 0; i < GetMenuItemCount(bar); ++i) {
            var sub = GetSubMenu(bar, i);
            if (sub == IntPtr.Zero) continue;
            for (int j = 0; j < GetMenuItemCount(sub); ++j) {
                var b = new StringBuilder(128);
                GetMenuStringW(sub, (uint)j, b, 128, 0x400);
                if (!b.ToString().StartsWith(pluginName)) continue;
                var plugin = GetSubMenu(sub, j);
                for (int k = 0; k < GetMenuItemCount(plugin); ++k) {
                    var t = new StringBuilder(128);
                    GetMenuStringW(plugin, (uint)k, t, 128, 0x400);
                    if (t.Length > 0) items[t.ToString().Split('\t')[0]] = GetMenuItemID(plugin, k);
                }
                return items;
            }
        }
        return items;
    }
    public bool MenuChecked(uint id) => (GetMenuState(GetMenu(Main), id, 0) & 8) != 0;

    // A modal dialog would block a sent message, so commands that may open one are posted.
    public void Command(uint id, int waitMs = 150) { Post(Main, WM_COMMAND, id, 0); Thread.Sleep(waitMs); }

    public string Text(IntPtr sci, int max = 4000) {
        long length = Math.Min(Send(sci, SCI_GETLENGTH), max);
        var bytes = new byte[length];
        for (int i = 0; i < length; ++i) bytes[i] = (byte)Send(sci, SCI_GETCHARAT, i);
        return Encoding.UTF8.GetString(bytes);
    }

    public IntPtr ActiveSci() => Send(Main, NPPM_GETCURRENTVIEW) == 0 ? Sci0 : Sci1;

    public void Screenshot(IntPtr h, string path) {
        var r = RectOf(h);
        using var bitmap = new Bitmap(Math.Max(r.Width, 1), Math.Max(r.Height, 1), PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bitmap)) {
            var dc = g.GetHdc();
            PrintWindow(h, dc, 2);
            g.ReleaseHdc(dc);
        }
        bitmap.Save(path, ImageFormat.Png);
    }

    [DllImport("kernel32")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32")] static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, IntPtr size, uint type, uint protect);
    [DllImport("kernel32")] static extern bool VirtualFreeEx(IntPtr process, IntPtr address, IntPtr size, uint type);
    [DllImport("kernel32")] static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] data, IntPtr size, out IntPtr written);
    [DllImport("kernel32")] static extern bool CloseHandle(IntPtr handle);

    // Ticks the checkbox of a list view item. The LVITEM has to live inside the other process.
    public void ListCheck(IntPtr list, int index) {
        const uint LVM_SETITEMSTATE = 0x102B, VmAccess = 0x0008 | 0x0020, Commit = 0x1000, ReadWrite = 4, Release = 0x8000;
        var process = OpenProcess(VmAccess, false, Process.Id);
        var memory = VirtualAllocEx(process, IntPtr.Zero, (IntPtr)128, Commit, ReadWrite);
        var item = new byte[128];
        BitConverter.GetBytes(2u << 12).CopyTo(item, 12);    // state: checkbox image 2, checked
        BitConverter.GetBytes(0xF000u).CopyTo(item, 16);     // stateMask: LVIS_STATEIMAGEMASK
        WriteProcessMemory(process, memory, item, (IntPtr)item.Length, out _);
        Send(list, LVM_SETITEMSTATE, index, (long)memory);
        VirtualFreeEx(process, memory, IntPtr.Zero, Release);
        CloseHandle(process);
    }

    // After Notepad++ quits for the updater, the updater starts a new one by itself.
    public bool WaitForExit(int timeoutMs) => Process.WaitForExit(timeoutMs);

    public bool AttachToNew(int timeoutMs) {
        Main = IntPtr.Zero;
        for (int waited = 0; waited < timeoutMs && Main == IntPtr.Zero; waited += 200) {
            Thread.Sleep(200);
            EnumDesktopWindows(desktop, (h, l) => {
                if (ClassOf(h) == "Notepad++" && IsWindowVisible(h)) { Main = h; return false; }
                return true;
            }, IntPtr.Zero);
        }
        if (Main == IntPtr.Zero) return false;
        Thread0 = GetWindowThreadProcessId(Main, out int pid);
        Process = Process.GetProcessById(pid);
        processHandle = OpenProcess(0x1000 | 0x00100000, false, pid);  // query limited information, synchronize
        attached = false;
        Thread.Sleep(1200);
        var editors = Children(Main, "Scintilla");
        Sci0 = editors[0]; Sci1 = editors[1];
        return true;
    }

    // Names of the windows the updater opened, for reporting what it is doing.
    public List<string> UpdaterWindows() {
        var names = new List<string>();
        EnumDesktopWindows(desktop, (h, l) => {
            if (IsWindowVisible(h)) names.Add(ClassOf(h) + ": " + TextOf(h));
            return true;
        }, IntPtr.Zero);
        return names;
    }

    public IntPtr WaitDialogExcept(IntPtr known, int timeoutMs = 4000) {
        for (int waited = 0; waited < timeoutMs; waited += 50) {
            foreach (var dialog in TopLevel("#32770")) {
                if (dialog != known) return dialog;
            }
            Thread.Sleep(50);
        }
        return IntPtr.Zero;
    }

    public IntPtr WaitDialog(int timeoutMs = 3000) {
        for (int waited = 0; waited < timeoutMs; waited += 50) {
            var dialogs = TopLevel("#32770");
            if (dialogs.Count > 0) return dialogs[0];
            Thread.Sleep(50);
        }
        return IntPtr.Zero;
    }

    public IntPtr Item(IntPtr dialog, int id) => GetDlgItem(dialog, id);
    [DllImport("user32")] static extern IntPtr GetDlgItem(IntPtr hwnd, int id);

    [DllImport("user32")] static extern bool RedrawWindow(IntPtr hwnd, IntPtr rect, IntPtr region, uint flags);

    // On a hidden desktop a dialog may not have painted yet, however long one waits.
    public void Repaint(IntPtr h) {
        const uint RDW_INVALIDATE = 1, RDW_ERASE = 4, RDW_ALLCHILDREN = 0x80, RDW_UPDATENOW = 0x100;
        RedrawWindow(h, IntPtr.Zero, IntPtr.Zero, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Settle(100);
    }

    // For the documentation: the title bar is left out, because it shows the full file path.
    public void DocumentationShot(string path) {
        var r = RectOf(Main);
        using var bitmap = new Bitmap(r.Width, r.Height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bitmap)) {
            var dc = g.GetHdc();
            PrintWindow(Main, dc, 2);
            g.ReleaseHdc(dc);
        }
        int top = Scale(33), side = Scale(8);
        using var cropped = bitmap.Clone(new Rectangle(side, top, r.Width - 2 * side, r.Height - top - side), PixelFormat.Format32bppArgb);
        cropped.Save(path, ImageFormat.Png);
    }

    public bool Close(int timeoutMs = 8000) {
        Post(Main, WM_CLOSE);
        bool exited = Process.WaitForExit(timeoutMs);
        Main = IntPtr.Zero;
        if (!exited) return false;
        GetExitCodeProcess(processHandle, out uint code);
        if (code != 0) Console.WriteLine($"  Notepad++ exit code: 0x{code:X8}");
        return code == 0;
    }

    // Waits, so that the plugin DLL is free again for the next run.
    public void Kill() {
        if (Process == null || Process.HasExited) return;
        Process.Kill();
        Process.WaitForExit(5000);
    }

    // A hidden desktop paints little by itself; printing a window makes it draw now.
    public void ForcePaint(IntPtr h) {
        using var bitmap = new Bitmap(16, 16, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bitmap);
        var dc = g.GetHdc();
        PrintWindow(h, dc, 0);
        g.ReleaseHdc(dc);
    }

    public void Dispose() {
        try { Kill(); } catch { }
        if (attached) { AttachThreadInput(GetCurrentThreadId(), Thread0, false); attached = false; }
        CloseDesktop(desktop);
    }

    // Runs body on a thread that lives on a fresh hidden desktop, and rethrows its failure.
    public static void Run(string desktopName, Action<NppDriver> body) {
        Exception failure = null;
        var thread = new Thread(() => {
            try {
                SetThreadDpiAwarenessContext((IntPtr)(-4));  // per-monitor v2: coordinates as Notepad++ sees them
                using var driver = new NppDriver(desktopName);
                body(driver);
            } catch (Exception e) { failure = e; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure != null) throw new Exception(failure.ToString());
    }
}
