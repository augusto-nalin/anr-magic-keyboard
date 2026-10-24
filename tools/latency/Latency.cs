// Measures the bridge from "report arrived at the ESP32" to "Windows delivered the key".
//
// With CONFIG_MKB_LATENCY_MARKERS the firmware prints "@K" on its console the moment a report
// comes in from the keyboard. This reads those markers from the serial port and, through a
// low-level keyboard hook, when Windows hands out each key event. Both get timestamped on the
// same Stopwatch and are paired up at the end.
//
// The keyboard -> ESP32 hop (sniff interval) isn't in here, and the marker's own trip over the
// UART and USB makes the numbers a little lower than the real thing.
//
// C# 5 on purpose: builds with the csc.exe that ships with .NET Framework, nothing to install.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

static class Latency {
    const int WH_KEYBOARD_LL = 13;
    const int WM_KEYDOWN     = 0x0100;
    const int WM_KEYUP       = 0x0101;
    const int WM_SYSKEYDOWN  = 0x0104;
    const int WM_SYSKEYUP    = 0x0105;
    const uint WM_QUIT       = 0x0012;
    const uint LLKHF_INJECTED = 0x10;

    // A key event pairs with the earliest unused marker in this window around it. Markers that
    // never turn into a key (Globe alone) or key events without a report (auto-repeat) drop out.
    const double MATCH_BEFORE_MS = 100;
    const double MATCH_AFTER_MS  = 5;

    [StructLayout(LayoutKind.Sequential)]
    struct KBDLLHOOKSTRUCT {
        public uint vkCode;
        public uint scanCode;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MSG {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int x;
        public int y;
    }

    delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SetWindowsHookEx(int idHook, HookProc proc, IntPtr module, uint threadId);

    [DllImport("user32.dll")]
    static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    static extern bool UnhookWindowsHookEx(IntPtr hook);

    [DllImport("user32.dll")]
    static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);

    [DllImport("user32.dll")]
    static extern bool PostThreadMessage(uint threadId, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetModuleHandle(string name);

    [DllImport("kernel32.dll")]
    static extern uint GetCurrentThreadId();

    static readonly Stopwatch clock = Stopwatch.StartNew();
    static readonly object gate = new object();
    static readonly List<double> markers = new List<double>();
    static readonly List<double> keys = new List<double>();

    // Kept in a field, the hook only holds a native pointer to it
    static HookProc hookProc;
    static uint hookThread;
    static int target;

    static double Now() {
        return clock.Elapsed.TotalMilliseconds;
    }

    static int Main(string[] args) {
        if (args.Length < 1) {
            Console.Error.WriteLine("usage: latency <COMx> [key events, default 40] [report file]");
            return 1;
        }

        string port = args[0];
        target = args.Length > 1 ? int.Parse(args[1]) : 40;
        string reportFile = args.Length > 2 ? args[2] : null;

        var serial = new SerialPort(port, 115200);
        // Both released so the ESP32's auto-reset circuit leaves it running
        serial.DtrEnable = false;
        serial.RtsEnable = false;
        serial.Open();

        var reader = new Thread(() => ReadMarkers(serial));
        reader.IsBackground = true;
        reader.Start();

        hookThread = GetCurrentThreadId();
        Console.CancelKeyPress += (sender, e) => {
            e.Cancel = true;
            PostThreadMessage(hookThread, WM_QUIT, IntPtr.Zero, IntPtr.Zero);
        };

        Console.WriteLine("Listening on {0}.", port);
        Console.WriteLine("Tap ONE key on the Magic Keyboard, slowly, about {0} times. Ctrl+C stops early.", target / 2);

        hookProc = OnKey;
        IntPtr hook = SetWindowsHookEx(WH_KEYBOARD_LL, hookProc, GetModuleHandle(null), 0);
        if (hook == IntPtr.Zero) {
            Console.Error.WriteLine("SetWindowsHookEx failed: {0}", Marshal.GetLastWin32Error());
            return 1;
        }

        MSG msg;
        while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) { }

        UnhookWindowsHookEx(hook);
        serial.Close();

        string report = Report();
        Console.WriteLine();
        Console.WriteLine(report);
        if (reportFile != null) {
            File.WriteAllText(reportFile, report);
        }
        return 0;
    }

    static void ReadMarkers(SerialPort serial) {
        int previous = -1;
        while (true) {
            int current;
            try {
                current = serial.ReadByte();
            } catch (Exception) {
                return;
            }

            double at = Now();
            if (previous == '@' && current == 'K') {
                lock (gate) {
                    markers.Add(at);
                }
            }
            previous = current;
        }
    }

    // Runs inside Windows' input processing, so only timestamp here and get out
    static IntPtr OnKey(int code, IntPtr wParam, IntPtr lParam) {
        double at = Now();

        if (code >= 0) {
            var info = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(KBDLLHOOKSTRUCT));
            int message = wParam.ToInt32();
            bool isKey = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;

            if (isKey && (info.flags & LLKHF_INJECTED) == 0) {
                int count;
                lock (gate) {
                    keys.Add(at);
                    count = keys.Count;
                }

                Console.Write("\r{0}/{1} key events", count, target);
                if (count >= target) {
                    PostThreadMessage(hookThread, WM_QUIT, IntPtr.Zero, IntPtr.Zero);
                }
            }
        }

        return CallNextHookEx(IntPtr.Zero, code, wParam, lParam);
    }

    static string Report() {
        List<double> markerTimes;
        List<double> keyTimes;
        lock (gate) {
            markerTimes = new List<double>(markers);
            keyTimes = new List<double>(keys);
        }

        var used = new bool[markerTimes.Count];
        var samples = new List<double>();
        int unmatched = 0;

        foreach (double key in keyTimes) {
            int match = -1;
            for (int i = 0; i < markerTimes.Count; ++i) {
                double delta = key - markerTimes[i];
                if (!used[i] && delta <= MATCH_BEFORE_MS && delta >= -MATCH_AFTER_MS) {
                    match = i;
                    break;
                }
            }

            if (match < 0) {
                unmatched++;
                continue;
            }

            used[match] = true;
            samples.Add(key - markerTimes[match]);
        }

        var lines = new List<string>();
        lines.Add(string.Format("markers {0}, key events {1}, matched {2}, unmatched {3}",
                                markerTimes.Count, keyTimes.Count, samples.Count, unmatched));

        if (samples.Count == 0) {
            lines.Add("no matched samples: is CONFIG_MKB_LATENCY_MARKERS on, and were you typing on the Magic Keyboard?");
            return string.Join(Environment.NewLine, lines);
        }

        samples.Sort();
        Func<double, double> percentile = p => samples[Math.Min(samples.Count - 1, (int)Math.Ceiling(p * samples.Count) - 1)];

        lines.Add("ESP32 report in -> Windows key event (ms):");
        lines.Add(string.Format("  min {0:F2}  median {1:F2}  mean {2:F2}  p95 {3:F2}  max {4:F2}",
                                samples[0], percentile(0.5), samples.Average(), percentile(0.95), samples[samples.Count - 1]));
        lines.Add("  samples: " + string.Join(" ", samples.Select(s => s.ToString("F1"))));
        return string.Join(Environment.NewLine, lines);
    }
}
