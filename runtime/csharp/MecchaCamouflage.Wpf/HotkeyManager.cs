using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace MecchaCamouflage.Wpf;

public sealed class HotkeyManager : IDisposable
{
    private readonly WindowInteropHelper helper;
    private HwndSource? source;
    private Action? start;
    private Action? preview;
    private Action? unpreview;
    private Action? stop;

    public HotkeyManager(System.Windows.Window window)
    {
        helper = new WindowInteropHelper(window);
    }

    public void Register(string startHotkey, string previewHotkey, string unpreviewHotkey, string stopHotkey,
                         Action startAction, Action previewAction, Action unpreviewAction, Action stopAction)
    {
        Dispose();
        start = startAction;
        preview = previewAction;
        unpreview = unpreviewAction;
        stop = stopAction;
        source = HwndSource.FromHwnd(helper.Handle);
        source?.AddHook(WndProc);
        RegisterHotKey(helper.Handle, 1, 0, ParseVirtualKey(startHotkey, 0x70));
        RegisterHotKey(helper.Handle, 2, 0, ParseVirtualKey(previewHotkey, 0x71));
        RegisterHotKey(helper.Handle, 3, 0, ParseVirtualKey(unpreviewHotkey, 0x72));
        RegisterHotKey(helper.Handle, 4, 0, ParseVirtualKey(stopHotkey, 0x73));
    }

    public void Dispose()
    {
        if (helper.Handle != IntPtr.Zero)
        {
            for (var id = 1; id <= 4; ++id)
                UnregisterHotKey(helper.Handle, id);
        }
        source?.RemoveHook(WndProc);
        source = null;
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == 0x0312)
        {
            handled = true;
            switch (wParam.ToInt32())
            {
                case 1: start?.Invoke(); break;
                case 2: preview?.Invoke(); break;
                case 3: unpreview?.Invoke(); break;
                case 4: stop?.Invoke(); break;
            }
        }
        return IntPtr.Zero;
    }

    private static uint ParseVirtualKey(string text, uint fallback)
    {
        var value = text.Trim().ToUpperInvariant();
        if (value.Length >= 2 && value[0] == 'F' && int.TryParse(value[1..], out var f) && f is >= 1 and <= 24)
            return (uint)(0x70 + f - 1);
        return fallback;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);
}
