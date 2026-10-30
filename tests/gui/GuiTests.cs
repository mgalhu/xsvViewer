// End-to-end checks against a real Notepad++ on a hidden desktop. See run.ps1.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

public static class GuiTests {
    const int VK_BACK = 8, VK_TAB = 9, VK_RETURN = 13, VK_ESCAPE = 27, VK_END = 35, VK_HOME = 36, VK_DOWN = 40, VK_RIGHT = 39, VK_DELETE = 46, VK_PRIOR = 33, VK_NEXT = 34;
    const uint TCM_SETCURFOCUS = 0x1330, CB_SETCURSEL = 0x14E, WM_COMMAND = 0x111;
    const int IDM_VIEW_GOTO_ANOTHER_VIEW = 10001, IDM_VIEW_CLONE_TO_ANOTHER_VIEW = 10002, IDM_FILE_CLOSE = 41003, IDM_FILE_RELOAD = 41014, IDM_FILE_NEW = 41001;
    const uint SCI_SETSAVEPOINT = 2014, NPPM_ACTIVATEDOC = NppDriver.NPPMSG + 28;
    const int IDM_FORMAT_ANSI = 45004;
    const int IDOK = 1, IDCANCEL = 2, IDYES = 6;

    static int passed, failed;
    static string exe, samples, shots, config;

    public static void Check(string name, bool ok, string detail = "") {
        if (ok) ++passed; else ++failed;
        Console.WriteLine($"  {(ok ? "ok  " : "FAIL")} {name}{(detail.Length > 0 ? "  [" + detail + "]" : "")}");
    }

    static string Sample(string name) => Path.Combine(samples, name);
    static string Shot(string name) => Path.Combine(shots, name + ".png");
    static string Open(params string[] names) {
        var parts = new List<string> { "-multiInst", "-nosession" };
        foreach (var n in names) parts.Add("\"" + Sample(n) + "\"");
        return string.Join(" ", parts);
    }

    static string LayoutsFile => Path.Combine(config, "xsvViewer.layouts.ini");
    static string OptionsFile => Path.Combine(config, "xsvViewer.ini");
    static void ResetSettings() {
        foreach (var f in new[] { LayoutsFile, OptionsFile }) if (File.Exists(f)) File.Delete(f);
    }

    public static int Run(string nppExe, string sampleDir, string shotDir, string which) {
        exe = nppExe; samples = sampleDir; shots = shotDir;
        config = Path.Combine(Path.GetDirectoryName(nppExe), "plugins", "Config");
        var scenarios = new (string, Action<NppDriver>)[] {
            ("toggle", Toggle), ("hiddenEdits", HiddenEdits), ("tabs", Tabs), ("closeTab", CloseTab), ("sort", SortAndExport), ("filter", Filters),
            ("columns", Columns), ("clipboard", Clipboard), ("persist", Persist), ("options", OptionsDialog), ("columnsDialog", ColumnsDialog),
            ("files", Files), ("ansi", Ansi), ("caret", CaretSync), ("split", Split), ("clone", Clone), ("panel", Panel), ("panelRestore", PanelRestore), ("reload", Reload), ("realMenu", RealMenu), ("filterSync", FilterSync), ("resources", Resources), ("encoding", Encoding8), ("variants", Variants), ("theme", Theme), ("big", Big),
        };
        int index = 0;
        if (which == "docs") {
            ResetSettings();
            NppDriver.Run("xsvtest_docs_" + Environment.ProcessId, Docs);
            return 0;
        }
        if (which == "pluginsAdmin") {
            Console.WriteLine(which);
            try {
                NppDriver.Run("xsvtest_admin_" + Environment.ProcessId, PluginsAdmin);
            } catch (Exception e) {
                Check("scenario finished", false, e.Message.Split('\n')[0]);
            }
            Console.WriteLine($"{passed} passed, {failed} failed");
            return failed;
        }
        foreach (var (name, scenario) in scenarios) {
            if (which != "all" && which != name) continue;
            Console.WriteLine(name);
            ResetSettings();
            var clock = Stopwatch.StartNew();
            try {
                NppDriver.Run("xsvtest_" + name + "_" + (++index) + "_" + Environment.ProcessId, scenario);
            } catch (Exception e) {
                Check("scenario finished", false, e.Message.Split('\n')[0]);
            }
            Console.WriteLine($"  ({clock.ElapsedMilliseconds} ms)");
        }
        Console.WriteLine($"{passed} passed, {failed} failed");
        return failed;
    }

    // ---- helpers ------------------------------------------------------------------------------

    static IntPtr VisibleHost(NppDriver d) => d.Children(d.Main, "xsvViewerHost").FirstOrDefault(NppDriver.Visible);
    static IntPtr GridOf(NppDriver d, IntPtr host) => d.Children(host, "xsvViewerGrid").FirstOrDefault();
    static IntPtr FilterBox(NppDriver d, IntPtr host) => d.Children(host, "Edit").FirstOrDefault(h => (NppDriver.RectOf(h).Width > d.Scale(150)));

    static int RowHeight(NppDriver d) => (int)Math.Round(9.0 * d.Dpi / 72 * 1.33) + 2 * d.Scale(3);
    static int HeaderHeight(NppDriver d) => (int)Math.Round(9.0 * d.Dpi / 72 * 1.33) + 2 * d.Scale(5);
    static int RowY(NppDriver d, int row) => HeaderHeight(d) + RowHeight(d) * row + RowHeight(d) / 2;

    // Exports the current view into a new tab, returns its text, closes the tab again.
    static string[] ExportView(NppDriver d, string format = "TSV", int max = 200000) {
        var menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Export View as " + format], 100);
        d.Settle(100);
        var sci = d.ActiveSci();
        string text = d.Text(sci, max);
        d.Send(sci, SCI_SETSAVEPOINT);
        d.Send(d.Main, NppDriver.NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        d.Settle(150);
        return text.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries);
    }

    static string[] Column(string[] lines, int column) => lines.Skip(1).Select(l => { var c = l.Split('\t'); return column < c.Length ? c[column] : ""; }).ToArray();
    static double Num(string s) => double.Parse(s, System.Globalization.CultureInfo.InvariantCulture);

    static bool Ascending(IEnumerable<double> v) { double last = double.NegativeInfinity; foreach (var x in v) { if (x < last) return false; last = x; } return true; }

    // Left edge of a visible column, from cumulative header widths found by hit-testing a sort click is
    // fragile, so columns are addressed through a fraction of the known sample layout at the current DPI.
    static int ColumnX(NppDriver d, int logicalX) => d.Scale(logicalX);

    // ---- scenarios ----------------------------------------------------------------------------

    // The real popup menu, shim switched off: its modal loop runs timers and sees the keyboard hook.
    static void RealMenu(NppDriver d) {
        var marker = Path.Combine(config, "shim_real_menus.txt");
        File.WriteAllText(marker, "");
        try {
            d.Start(exe, Open("sample.tsv"));
            d.Settle(500);
            var host = VisibleHost(d);
            var grid = GridOf(d, host);
            var box = FilterBox(d, host);

            d.SetText(box, "treated");
            d.Settle(450);
            d.RightClick(grid, d.Scale(100), RowY(d, 1));
            Check("a real menu opens", d.TopLevel("#32768").Count > 0);
            d.MenuKey(VK_ESCAPE);
            Thread.Sleep(200);
            Check("Esc closes the menu", d.TopLevel("#32768").Count == 0);
            if (d.TopLevel("#32768").Count > 0) { d.Kill(); return; }
            Check("and does not reach the table behind it", ExportView(d).Length == 31);

            // 0 copy, 1 with header, 2 csv, 3 markdown, 4 only this value, 5 hide, 6 clear, 7 show in text
            d.RightClick(grid, d.Scale(100), RowY(d, 1));
            for (int i = 0; i < 5; ++i) d.MenuKey(VK_DOWN);
            d.MenuKey(VK_RETURN);
            Thread.Sleep(250);
            Check("Enter picks the menu item, not the table's Enter", VisibleHost(d) != IntPtr.Zero && ExportView(d).Length == 2);
            d.Key(VK_ESCAPE);
            d.Settle(150);

            // The row filter's delay runs out while the menu is open, so the rows change under it.
            d.SetText(box, "control");
            d.RightClick(grid, d.Scale(100), RowY(d, 20));
            Thread.Sleep(500);
            for (int i = 0; i < 8; ++i) d.MenuKey(VK_DOWN);
            d.MenuKey(VK_RETURN);
            Thread.Sleep(300);
            Check("rows changing under an open menu is survived", !d.Process.HasExited && d.TopLevel("#32768").Count == 0);
            d.Kill();
        } finally {
            File.Delete(marker);
        }
    }

    static void FilterSync(NppDriver d) {
        d.Start(exe, Open("sample.tsv", "r_rownames.tsv"));
        d.Settle(500);
        d.Send(d.Main, NPPM_ACTIVATEDOC, 0, 0);
        d.Settle(200);
        var host = VisibleHost(d);
        var grid = GridOf(d, host);
        var box = FilterBox(d, host);

        // The filter box must show what is really applied.
        d.SetText(box, "treated");
        d.Settle(450);
        d.Click(grid, d.Scale(100), RowY(d, 1));
        d.Key(VK_ESCAPE);
        d.Settle(150);
        Check("clearing filters empties the filter box", d.ControlText(box) == "", d.ControlText(box));

        // Leaving the box applies what was typed, without waiting for the delay.
        d.Key('F', ctrl: true);
        d.Settle(50);
        Check("the filter box has focus", d.Focus() == box, NppDriver.ClassOf(d.Focus()));
        d.SetText(box, "treated");
        d.Click(grid, d.Scale(100), RowY(d, 1));
        Check("a click away applies the filter at once", ExportView(d).Length == 31);
        d.Key(VK_ESCAPE);
        d.Settle(150);

        // A column filter being edited must not survive its table going away.
        d.Key('L', ctrl: true, shift: true);
        d.Settle(150);
        d.Click(grid, FindColumnX(d, grid, 1), HeaderHeight(d) + RowHeight(d) / 2);
        d.Settle(80);
        var edit = d.Children(grid, "Edit").First();
        d.SetText(edit, "treated");
        d.Key('T', ctrl: true, alt: true);
        d.Settle(150);
        d.Key('T', ctrl: true, alt: true);
        d.Settle(150);
        Check("no stale column filter editor comes back", !NppDriver.Visible(edit));
        Check("what was typed there was applied", ExportView(d).Length == 31);
        d.Key(VK_ESCAPE);
        d.Key('L', ctrl: true, shift: true);
        d.Settle(150);

        // Same for the delimiter box: text typed for one table must not land on the next.
        var combo = d.Children(host, "ComboBox").First();
        var delimiter = d.Children(combo, "Edit").First();
        d.Click(delimiter, 10, 10);
        d.Settle(100);
        Check("delimiter box has focus", d.Focus() == delimiter, NppDriver.ClassOf(d.Focus()));
        d.SetText(delimiter, ";");
        d.Send(d.Main, NPPM_ACTIVATEDOC, 0, 1);
        d.Settle(250);
        var second = GridOf(d, VisibleHost(d));
        d.Click(second, d.Scale(100), RowY(d, 1));
        d.Settle(150);
        var lines = ExportView(d);
        Check("the other table keeps its own delimiter", lines[0] == "row.names\tgeneA\tgeneB\tgeneC", lines[0]);
        Check("closes cleanly", d.Close());
    }

    static void Resources(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var grid = GridOf(d, VisibleHost(d));
        d.ForcePaint(grid);

        // Each step beyond the largest size so far needs a new back buffer.
        uint before = d.GdiObjects;
        for (int i = 1; i <= 30; ++i) {
            d.Resize(1500 + i * 10, 900 + i * 5);
            d.ForcePaint(grid);
        }
        d.Settle(200);
        uint after = d.GdiObjects;
        Check("the window really grew", NppDriver.RectOf(d.Main).Width > 1600, NppDriver.RectOf(d.Main).ToString());
        Check("growing the window leaks no bitmaps", after <= before + 6, $"{before} -> {after} GDI objects");

        before = d.GdiObjects;
        for (int i = 0; i < 20; ++i) {
            d.Wheel(grid, i % 2 == 0 ? 120 : -120, keys: 8);
            d.ForcePaint(grid);
        }
        d.Settle(200);
        after = d.GdiObjects;
        Check("zooming leaks no fonts", after <= before + 6, $"{before} -> {after} GDI objects");
        Check("closes cleanly", d.Close());
    }

    static void Encoding8(NppDriver d) {
        d.Start(exe, Open("german.csv"));
        d.Settle(500);
        Check("reads as UTF-8 first", ExportView(d)[0] == "Name\tGröße\tWert");
        d.Send(d.Main, NppDriver.NPPM_MENUCOMMAND, 0, IDM_FORMAT_ANSI);
        d.Settle(300);
        d.Key('T', ctrl: true, alt: true);
        d.Key('T', ctrl: true, alt: true);
        d.Settle(200);
        var header = ExportView(d)[0];
        Check("a changed encoding reaches the table", header != "Name\tGröße\tWert", header);
        Check("closes cleanly", d.Close());
    }

    // The manual's "Test your plugins locally": a debug Notepad++ reads nppPluginList.json, and the
    // plugin goes the way users will get it. Plugins Admin passes the release URL and hash to the
    // updater, Notepad++ quits, the updater fetches and checks the zip, and restarts Notepad++.
    // Not part of "all". Needs a prepared folder and -Exe, -NoDeploy; see docs\DEVELOPMENT.md.
    static void PluginsAdmin(NppDriver d) {
        const int IDM_SETTING_PLUGINADM = 48015, SearchEdit = 5502, InstallButton = 5503, RemoveButton = 5505;
        const uint LVM_GETNEXTITEM = 0x100C;
        string folder = Path.GetDirectoryName(exe);
        string dll = Path.Combine(folder, "plugins", "xsvViewer", "xsvViewer.dll");
        string guide = Path.Combine(folder, "plugins", "xsvViewer", "doc", "USER_GUIDE.md");

        Check("the plugin is not there before", !File.Exists(dll));
        d.Start(exe, "-multiInst -nosession");
        d.Settle(800);
        Check("and not in the Plugins menu", d.PluginMenu("xsvViewer").Count == 0);

        // Install, from the Available tab.
        d.Post(d.Main, WM_COMMAND, IDM_SETTING_PLUGINADM);
        var admin = d.WaitDialog(6000);
        Check("Plugins Admin opens", admin != IntPtr.Zero);
        if (admin == IntPtr.Zero) return;
        d.SetText(d.Item(admin, SearchEdit), "xsvViewer");
        d.Settle(400);
        var list = d.Children(admin, "SysListView32").First(NppDriver.Visible);
        int item = (int)d.Send(list, LVM_GETNEXTITEM, -1, 2);
        Check("the list offers xsvViewer++", item >= 0, "index " + item);
        if (item < 0) return;
        d.ListCheck(list, item);
        d.Settle(200);
        d.Repaint(admin);
        Thread.Sleep(200);
        d.Screenshot(admin, Shot("admin_available"));

        var install = d.Item(admin, InstallButton);
        d.Post(admin, WM_COMMAND, InstallButton, (long)install);
        var confirm = d.WaitDialogExcept(admin);
        Check("Notepad++ asks before it quits", confirm != IntPtr.Zero);
        if (confirm == IntPtr.Zero) return;
        var quitting = d.Process;
        d.Post(confirm, WM_COMMAND, IDYES);
        Check("Notepad++ quits for the updater", quitting.WaitForExit(15000));

        // The updater downloads from the public release, checks the SHA-256, and unpacks.
        var clock = Stopwatch.StartNew();
        bool back = d.AttachToNew(90000);
        Console.WriteLine($"  updater took {clock.ElapsedMilliseconds} ms");
        if (!back) {
            foreach (var w in d.UpdaterWindows()) Console.WriteLine("    window: " + w);
            Check("Notepad++ comes back after installing", false);
            return;
        }
        Check("Notepad++ comes back after installing", true);
        Check("the DLL landed in its folder", File.Exists(dll), dll);
        Check("the guide came with it", File.Exists(guide), guide);
        var menu = d.PluginMenu("xsvViewer");
        Check("the plugin is in the Plugins menu", menu.ContainsKey("Table / Text"), string.Join(", ", menu.Keys));
        Check("it closes cleanly", d.Close(15000));

        // Use it.
        d.Start(exe, Open("sample.tsv"));
        d.Settle(700);
        var host = VisibleHost(d);
        Check("a tsv opens as a table", host != IntPtr.Zero);
        Check("the table has its rows", ExportView(d).Length == 121);
        d.Screenshot(d.Main, Shot("admin_installed"));

        // Remove, from the Installed tab.
        d.Post(d.Main, WM_COMMAND, IDM_SETTING_PLUGINADM);
        admin = d.WaitDialog(6000);
        Check("Plugins Admin opens again", admin != IntPtr.Zero);
        if (admin == IntPtr.Zero) return;
        d.Send(d.Children(admin, "SysTabControl32").First(), TCM_SETCURFOCUS, 2);
        Thread.Sleep(300);
        d.SetText(d.Item(admin, SearchEdit), "xsvViewer");
        d.Settle(400);
        list = d.Children(admin, "SysListView32").First(NppDriver.Visible);
        item = (int)d.Send(list, LVM_GETNEXTITEM, -1, 2);
        Check("it is listed as installed", item >= 0, "index " + item);
        if (item < 0) return;
        d.ListCheck(list, item);
        d.Settle(200);
        var remove = d.Item(admin, RemoveButton);
        d.Post(admin, WM_COMMAND, RemoveButton, (long)remove);
        confirm = d.WaitDialogExcept(admin);
        if (confirm == IntPtr.Zero) { Check("Notepad++ asks before removing", false); return; }
        quitting = d.Process;
        d.Post(confirm, WM_COMMAND, IDYES);
        quitting.WaitForExit(15000);
        back = d.AttachToNew(60000);
        Check("Notepad++ comes back after removing", back);
        if (!back) return;
        Check("the plugin folder is gone", !Directory.Exists(Path.GetDirectoryName(dll)));
        Check("the guide is gone", !File.Exists(guide));
        Check("and the menu entry", d.PluginMenu("xsvViewer").Count == 0);
        Check("closes cleanly", d.Close(15000));
    }

    // Regenerates the pictures in docs\images. Not part of "all"; run it with -Scenario docs.
    static void Docs(NppDriver d) {
        string images = Path.Combine(shots, "docs");
        Directory.CreateDirectory(images);
        string Image(string name) => Path.Combine(images, name + ".png");

        d.Start(exe, Open("sample.tsv"), 1280, 760);
        d.Settle(600);
        var host = VisibleHost(d);
        var grid = GridOf(d, host);
        d.DocumentationShot(Image("table"));

        d.SetText(FilterBox(d, host), "treated");
        d.Settle(450);
        d.Key('L', ctrl: true, shift: true);
        d.Settle(150);
        d.Click(grid, FindColumnX(d, grid, 2), HeaderHeight(d) + RowHeight(d) / 2);
        d.Settle(80);
        d.SetText(d.Children(grid, "Edit").First(), ">100000");
        d.Key(VK_RETURN);
        d.Settle(150);
        d.Click(grid, FindColumnX(d, grid, 3), d.Scale(10));
        d.Settle(150);
        d.DocumentationShot(Image("filter"));
        d.Key('L', ctrl: true, shift: true);
        d.Key(VK_ESCAPE);
        d.SetText(FilterBox(d, host), "");
        d.Settle(450);
        d.Click(grid, FindColumnX(d, grid, 3), d.Scale(10));
        d.Click(grid, FindColumnX(d, grid, 3), d.Scale(10));
        d.Settle(150);

        var menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Options..."], 300);
        var dialog = d.WaitDialog();
        var tabs = d.Item(dialog, 1001);
        d.Send(tabs, TCM_SETCURFOCUS, 2);
        d.Repaint(dialog);
        Thread.Sleep(200);
        d.Screenshot(dialog, Image("options"));
        d.Send(tabs, TCM_SETCURFOCUS, 3);
        Thread.Sleep(300);
        var choices = d.Children(dialog, "ComboBox").Where(NppDriver.Visible).ToList();
        d.Send(choices[0], CB_SETCURSEL, 3);
        d.Post(dialog, WM_COMMAND, (long)((1u << 16) | (uint)GetId(choices[0])), (long)choices[0]);
        d.Send(choices[1], CB_SETCURSEL, 1);
        d.Post(dialog, WM_COMMAND, (long)((1u << 16) | (uint)GetId(choices[1])), (long)choices[1]);
        Thread.Sleep(400);
        d.Post(dialog, WM_COMMAND, IDOK);
        Thread.Sleep(300);
        d.DocumentationShot(Image("heatmap"));
        d.Close();

        ResetSettings();
        string file = Path.Combine(Path.GetDirectoryName(exe), "config.xml");
        string original = File.ReadAllText(file);
        File.WriteAllText(file, System.Text.RegularExpressions.Regex.Replace(original, "(<GUIConfig name=\"DarkMode\" enable=)\"no\"", "$1\"yes\""));
        try {
            d.Start(exe, Open("sample.tsv"), 1280, 760);
            d.Settle(700);
            d.DocumentationShot(Image("dark"));
            d.Close();
        } finally {
            File.WriteAllText(file, original);
        }

        ResetSettings();
        d.Start(exe, Open("sample.tsv"), 1280, 760);
        d.Settle(500);
        d.Key('T', ctrl: true, alt: true);
        d.Settle(200);
        menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Table Panel"], 400);
        d.Settle(400);
        d.DocumentationShot(Image("panel"));
        d.Command(menu["Table Panel"], 200);
        d.Close();
    }

    // Many settings far from their defaults at once, loaded from the file a previous session wrote.
    static void Variants(NppDriver d) {
        File.WriteAllText(OptionsFile, string.Join("\n",
            "[Appearance]", "fontFace=Consolas", "fontSize=11", "linesPerRow=3", "rowPadding=6", "cellPadding=10",
            "maxAutoColumnWidth=220", "showGridV=0", "zebra=0", "headerBold=0", "showToolbar=0",
            "[Values]", "decimals=2", "thousands=1", "rightAlignNumbers=0", "colorScale=2", "") + "\n", new UTF8Encoding(false));
        d.Start(exe, Open("sample.tsv"));
        d.Settle(600);
        d.Screenshot(d.Main, Shot("variant_wrapped"));
        var host = VisibleHost(d);
        Check("no toolbar means no toolbar controls", !d.Children(host, "ComboBox").Any(NppDriver.Visible));

        d.Key('F', ctrl: true);
        d.Settle(150);
        Check("Ctrl+F brings the hidden toolbar back", d.Children(host, "ComboBox").Any(NppDriver.Visible));
        d.Key(VK_ESCAPE);

        d.Wheel(GridOf(d, host), 120, keys: 8);
        d.Wheel(GridOf(d, host), 120, keys: 8);
        d.Settle(150);
        d.Screenshot(d.Main, Shot("variant_zoomed"));
        Check("closes cleanly", d.Close());
        Check("zoom is remembered", File.ReadAllText(OptionsFile).Contains("fontSize=13"));

        File.WriteAllText(OptionsFile, string.Join("\n",
            "[Appearance]", "showStatusBar=0", "showRowNumbers=0", "showGridH=0", "minColumnWidth=200",
            "[Theme]", "theme=1", "[Light]", "background=#FFF8E7", "text=#3B2F2F", "headerBackground=#7A1F1F", "headerText=#FFFFFF", "") + "\n", new UTF8Encoding(false));
        d.Start(exe, Open("sample.tsv"));
        d.Settle(600);
        d.Screenshot(d.Main, Shot("variant_colours"));
        Check("custom colours close cleanly", d.Close());
    }

    // Notepad++ itself in dark mode, as opposed to the table's own theme setting.
    static void Theme(NppDriver d) {
        string file = Path.Combine(Path.GetDirectoryName(exe), "config.xml");
        string original = File.ReadAllText(file);
        File.WriteAllText(file, System.Text.RegularExpressions.Regex.Replace(original, "(<GUIConfig name=\"DarkMode\" enable=)\"no\"", "$1\"yes\""));
        try {
            d.Start(exe, Open("sample.tsv"));
            d.Settle(600);
            d.Screenshot(d.Main, Shot("dark_table"));
            var menu = d.PluginMenu("xsvViewer");
            d.Command(menu["Options..."], 300);
            var dialog = d.WaitDialog();
            Check("options dialog opens in dark mode", dialog != IntPtr.Zero);
            if (dialog != IntPtr.Zero) {
                d.Screenshot(dialog, Shot("dark_options"));
                d.Post(dialog, WM_COMMAND, IDCANCEL);
                Thread.Sleep(200);
            }
            d.Command(menu["Columns..."], 300);
            dialog = d.WaitDialog();
            if (dialog != IntPtr.Zero) {
                Thread.Sleep(800);
                d.Screenshot(dialog, Shot("dark_columns"));
                d.Post(dialog, WM_COMMAND, IDOK);
                Thread.Sleep(200);
            }
            Check("closes cleanly", d.Close());
        } finally {
            File.WriteAllText(file, original);
        }
    }

    static void Toggle(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var menu = d.PluginMenu("xsvViewer");
        var host = VisibleHost(d);
        Check("a csv/tsv opens as a table by itself", host != IntPtr.Zero);
        Check("overlay exactly covers the editor", NppDriver.RectOf(host).ToString() == NppDriver.RectOf(d.Sci0).ToString(), NppDriver.RectOf(host) + " vs " + NppDriver.RectOf(d.Sci0));
        Check("text is locked while covered", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 1);
        Check("menu item is checked", d.MenuChecked(menu["Table / Text"]));
        Check("opening did not mark the file modified", d.Send(d.Sci0, NppDriver.SCI_GETMODIFY) == 0);
        d.Screenshot(d.Main, Shot("toggle_table"));

        d.Key('T', ctrl: true, alt: true);
        d.Settle(100);
        Check("Ctrl+Alt+T shows the text", VisibleHost(d) == IntPtr.Zero);
        Check("text is editable again", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 0);
        Check("focus is in the editor", d.Focus() == d.Sci0, NppDriver.ClassOf(d.Focus()));
        Check("menu item is unchecked", !d.MenuChecked(menu["Table / Text"]));
        d.Screenshot(d.Main, Shot("toggle_text"));

        d.Key('T', ctrl: true, alt: true);
        d.Settle(100);
        host = VisibleHost(d);
        Check("Ctrl+Alt+T shows the table again", host != IntPtr.Zero);
        Check("text is locked again", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 1);
        Check("focus is in the grid", d.Focus() == GridOf(d, host), NppDriver.ClassOf(d.Focus()));

        Check("closes cleanly", d.Close());
    }

    static void HiddenEdits(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        long length = d.Send(d.Sci0, NppDriver.SCI_GETLENGTH);
        d.Key(VK_DELETE);
        d.Key(VK_BACK);
        d.Char('x');
        d.Key('V', ctrl: true);        // paste
        d.Key('X', ctrl: true);        // cut
        d.Key('D', ctrl: true);        // duplicate line
        d.Key('Y', ctrl: true);
        d.Key('Z', ctrl: true);
        d.Key(0xBE, ctrl: true, alt: false);
        d.Settle(100);
        Check("no keystroke changed the hidden text", d.Send(d.Sci0, NppDriver.SCI_GETLENGTH) == length, $"{length} -> {d.Send(d.Sci0, NppDriver.SCI_GETLENGTH)}");
        Check("document is not modified", d.Send(d.Sci0, NppDriver.SCI_GETMODIFY) == 0);
        Check("table still showing", VisibleHost(d) != IntPtr.Zero);
        Check("no prompt on closing", d.Close());
    }

    static void Tabs(NppDriver d) {
        d.Start(exe, Open("sample.tsv", "plain.txt"));
        d.Settle(500);
        Check("a .txt stays text", VisibleHost(d) == IntPtr.Zero);
        Check("a .txt stays editable", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 0);

        long buffer = d.Send(d.Main, NppDriver.NPPM_GETCURRENTBUFFERID);
        d.Key(VK_PRIOR, ctrl: true);
        d.Settle(200);
        Check("Ctrl+PgUp reaches Notepad++ from the text", d.Send(d.Main, NppDriver.NPPM_GETCURRENTBUFFERID) != buffer);
        var host = VisibleHost(d);
        Check("tab with a table shows it", host != IntPtr.Zero);
        Check("focus follows into the grid", d.Focus() == GridOf(d, host), NppDriver.ClassOf(d.Focus()));

        buffer = d.Send(d.Main, NppDriver.NPPM_GETCURRENTBUFFERID);
        d.Key(VK_NEXT, ctrl: true);
        d.Settle(200);
        Check("Ctrl+PgDn works from inside the grid", d.Send(d.Main, NppDriver.NPPM_GETCURRENTBUFFERID) != buffer);
        Check("back on the .txt, no table", VisibleHost(d) == IntPtr.Zero);

        d.Key('N', ctrl: true);
        d.Settle(200);
        Check("Ctrl+N makes a plain new tab", VisibleHost(d) == IntPtr.Zero && d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 0);
        d.Key('W', ctrl: true);
        d.Settle(200);

        d.Key(VK_PRIOR, ctrl: true);
        d.Settle(200);
        Check("table tab still works", VisibleHost(d) != IntPtr.Zero);

        // Ctrl+Tab opens Notepad++'s document switcher, which only commits when Ctrl is really released,
        // so this is the last key of the scenario.
        d.Key(VK_TAB, ctrl: true);
        d.Settle(200);
        Check("Ctrl+Tab reaches Notepad++ from inside the grid", NppDriver.ClassOf(d.Focus()) == "SysListView32", NppDriver.ClassOf(d.Focus()));
        d.Kill();
    }

    static void CloseTab(NppDriver d) {
        d.Start(exe, Open("plain.txt", "sample.tsv"));
        d.Settle(500);
        Check("table showing", VisibleHost(d) != IntPtr.Zero);
        d.Key('W', ctrl: true);
        d.Settle(300);
        Check("closing a table tab leaves no overlay", VisibleHost(d) == IntPtr.Zero && !d.Process.HasExited);
        Check("the remaining text tab is editable", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 0);
        d.Key('W', ctrl: true);
        d.Settle(300);
        Check("closing the last tab is clean", !d.Process.HasExited);
        Check("closes cleanly", d.Close());
    }

    static void SortAndExport(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var host = VisibleHost(d);
        var grid = GridOf(d, host);

        var original = ExportView(d);
        Check("export has all rows and the header", original.Length == 121, "lines=" + original.Length);
        Check("export header is right", original[0].StartsWith("sample_id\tgroup\tcount\tlog2fc"), original[0]);

        // sample_id, group and then count: the count header is well inside the third column.
        int countX = FindColumnX(d, grid, 2);
        d.Click(grid, countX, d.Scale(10));
        d.Settle(100);
        var sorted = ExportView(d);
        Check("header click sorts ascending", Ascending(Column(sorted, 2).Select(Num)), string.Join(",", Column(sorted, 2).Take(4)));

        d.Click(grid, countX, d.Scale(10));
        d.Settle(100);
        var descending = ExportView(d);
        Check("second click sorts descending", Ascending(Column(descending, 2).Select(s => -Num(s))));
        d.Screenshot(d.Main, Shot("sort_desc"));

        d.Click(grid, countX, d.Scale(10));
        d.Settle(100);
        Check("third click restores file order", ExportView(d).SequenceEqual(original));

        int lfcX = FindColumnX(d, grid, 3);
        d.Click(grid, lfcX, d.Scale(10));
        d.Settle(100);
        var withNa = Column(ExportView(d), 3);
        Check("NA sorts last", withNa.Last() == "NA" && withNa.First() != "NA", withNa.First() + " .. " + withNa.Last());

        d.Click(grid, lfcX, d.Scale(10));
        d.Settle(100);
        withNa = Column(ExportView(d), 3);
        Check("NA stays last in descending order", withNa.Last() == "NA" && withNa.First() != "NA");
        d.Click(grid, lfcX, d.Scale(10));

        var md = ExportView(d, "Markdown");
        Check("markdown export", md[0].StartsWith("| sample_id | group |") && md[1].StartsWith("| --- |"), md[0]);
        var csv = ExportView(d, "CSV");
        Check("csv export", csv[0].StartsWith("sample_id,group,count"), csv[0]);
        Check("exporting left the file unmodified", d.Send(d.Sci0, NppDriver.SCI_GETMODIFY) == 0);
        Check("closes cleanly", d.Close());
    }

    // A point inside a column of sample.tsv with default settings, in 96-DPI units, from its screenshot.
    static int FindColumnX(NppDriver d, IntPtr grid, int column) {
        int[] inside = { 80, 160, 230, 285 };
        return d.Scale(inside[column]);
    }

    // Right-clicks a spot in the grid; the test shim then chooses the named menu item.
    static string Pick(NppDriver d, IntPtr grid, int x, int y, string item) {
        var pick = Path.Combine(config, "shim_pick.txt");
        File.WriteAllText(pick, item, new UTF8Encoding(false));
        d.RightClick(grid, x, y);
        d.Settle(150);
        return File.ReadAllText(Path.Combine(config, "shim_menu.txt"));
    }
    static string PickHeader(NppDriver d, IntPtr grid, int column, string item) => Pick(d, grid, FindColumnX(d, grid, column), d.Scale(10), item);

    static void Filters(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var host = VisibleHost(d);
        var grid = GridOf(d, host);
        var box = FilterBox(d, host);
        Check("filter box found", box != IntPtr.Zero);

        d.SetText(box, "treated");
        d.Settle(450);
        var lines = ExportView(d);
        Check("row filter keeps matches", lines.Length == 31 && Column(lines, 1).All(c => c == "treated"), "lines=" + lines.Length);
        d.Screenshot(d.Main, Shot("filter_global"));

        d.SetText(box, "länge");
        d.Settle(450);
        lines = ExportView(d);
        Check("filter is case-insensitive across umlauts", lines.Length == 31, "lines=" + lines.Length);

        d.SetText(box, "/(/");
        d.Settle(450);
        Check("broken regex does not crash or hide rows", ExportView(d).Length == 121);

        d.SetText(box, "");
        d.Settle(450);
        Check("clearing shows every row", ExportView(d).Length == 121);

        d.Key('L', ctrl: true, shift: true);
        d.Settle(150);
        d.Click(grid, FindColumnX(d, grid, 2), HeaderHeight(d) + RowHeight(d) / 2);
        d.Settle(80);
        var edit = d.Children(grid, "Edit").FirstOrDefault(NppDriver.Visible);
        Check("column filter cell opens an editor", edit != IntPtr.Zero);
        if (edit != IntPtr.Zero) {
            d.SetText(edit, ">100000 & <150000");
            Check("focus is in the filter editor", d.Focus() == edit);
            d.Settle(100);
            d.Screenshot(d.Main, Shot("filter_editing"));
            d.Key(VK_RETURN);
            d.Settle(150);
            lines = ExportView(d);
            var counts = Column(lines, 2).Select(Num).ToArray();
            Check("numeric range filter", counts.Length > 0 && counts.All(c => c > 100000 && c < 150000), "rows=" + counts.Length);
            Check("focus returned to the grid", d.Focus() == grid, NppDriver.ClassOf(d.Focus()));
            d.Screenshot(d.Main, Shot("filter_column"));
        }

        d.Key(VK_ESCAPE);
        d.Settle(100);
        Check("Esc clears all filters", ExportView(d).Length == 121);

        d.Key('F', ctrl: true);
        d.Settle(100);
        Check("Ctrl+F goes to the filter box", d.Focus() == box, NppDriver.ClassOf(d.Focus()));
        Check("Notepad++'s Find dialog did not open", d.WaitDialog(300) == IntPtr.Zero);
        d.Key(VK_ESCAPE);
        d.Settle(100);
        Check("Esc leaves the filter box", d.Focus() == grid, NppDriver.ClassOf(d.Focus()));
        Check("closes cleanly", d.Close());
    }

    static void Columns(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var host = VisibleHost(d);
        var grid = GridOf(d, host);

        // Reorder: drag "count" to the far left.
        d.Drag(grid, FindColumnX(d, grid, 2), d.Scale(10), d.Scale(50), d.Scale(10));
        d.Settle(100);
        d.Screenshot(d.Main, Shot("columns_reordered"));
        var lines = ExportView(d);
        Check("dragging a header moves the column", lines[0].StartsWith("count\t"), lines[0].Substring(0, 30));

        var items = Pick(d, grid, d.Scale(60), d.Scale(10), "Hide column");
        Check("header menu lists column settings", items.Contains("Freeze up to this column") && items.Contains("Set width...") && items.Contains("Use default"));
        Check("nothing to clear yet, so Clear sort is greyed", items.Contains("disabled\tClear sort"));
        lines = ExportView(d);
        Check("hide column", !lines[0].StartsWith("count"), lines[0].Substring(0, 30));

        Pick(d, grid, d.Scale(60), d.Scale(10), "Set width...");
        var prompt = d.WaitDialog();
        Check("set width asks for a number", prompt != IntPtr.Zero);
        if (prompt != IntPtr.Zero) {
            d.SetText(d.Item(prompt, 1202), "333");
            d.Post(prompt, WM_COMMAND, IDOK);
            Thread.Sleep(200);
        }

        Pick(d, grid, d.Scale(60), d.Scale(10), "Show all columns");
        Check("show all columns", ExportView(d)[0].Contains("count"));

        Pick(d, grid, FindColumnX(d, grid, 0), d.Scale(10), "Sort descending");
        lines = ExportView(d);
        Check("sort descending from the menu", Ascending(Column(lines, 0).Select(s => -Num(s))), Column(lines, 0)[0]);
        Pick(d, grid, FindColumnX(d, grid, 0), d.Scale(10), "Clear sort");
        lines = ExportView(d);
        Check("clear sort from the menu", Column(lines, 1)[0] == "s1", Column(lines, 1)[0]);

        // Per-column display: right-aligned text and two decimals in the log2fc column.
        Pick(d, grid, d.Scale(410), d.Scale(10), "Center");
        Pick(d, grid, d.Scale(300), d.Scale(10), "2");
        d.Screenshot(d.Main, Shot("columns_display"));

        // Freeze the first two columns and scroll: the frozen line must be visible in the picture.
        Pick(d, grid, d.Scale(130), d.Scale(10), "Freeze up to this column");
        d.Key(VK_END);
        d.Settle(100);
        d.Screenshot(d.Main, Shot("columns_frozen"));

        // Values from the cell menu.
        var cellItems = Pick(d, grid, d.Scale(600), RowY(d, 2), "Show only this value");
        Check("cell menu lists copy and filter items", cellItems.Contains("Copy as Markdown table") && cellItems.Contains("Show in text"));
        var kept = ExportView(d);
        Check("show only this value filters", kept.Length >= 2 && kept.Length < 121, "lines=" + kept.Length);
        d.Key(VK_ESCAPE);
        d.Settle(150);
        Check("Esc clears the filter", ExportView(d).Length == 121);

        Check("closes cleanly", d.Close());
        Check("manual width is stored", File.ReadAllText(LayoutsFile).Contains("333"));
    }

    static void Clipboard(NppDriver d) {
        var dump = Path.Combine(config, "shim_clipboard.txt");
        if (File.Exists(dump)) File.Delete(dump);
        d.Start(exe, Open("r_rownames.tsv"));
        d.Settle(500);
        var grid = GridOf(d, VisibleHost(d));

        Func<string> copied = () => File.ReadAllText(dump, Encoding.Unicode);   // the shim writes UTF-16

        d.Key('C', ctrl: true);
        d.Settle(100);
        Check("Ctrl+C copies the current cell", File.Exists(dump) && copied().Trim() == "donor1", File.Exists(dump) ? copied().Trim() : "no copy");

        d.Key('A', ctrl: true);
        d.Key('C', ctrl: true);
        d.Settle(100);
        var all = copied();
        Check("Ctrl+A, Ctrl+C copies the whole table", all.Split("\r\n", StringSplitOptions.RemoveEmptyEntries).Length == 3 && all.Contains("donor3\t2.25\t0.125\t-1.5"), all.Replace("\r\n", " | "));

        d.Key('C', ctrl: true, shift: true);
        d.Settle(100);
        all = copied();
        Check("Ctrl+Shift+C adds the header", all.StartsWith("row.names\tgeneA", StringComparison.Ordinal), all.Substring(0, 24));

        Pick(d, grid, d.Scale(120), RowY(d, 1), "Copy as Markdown table");
        all = copied();
        Check("copy as markdown", all.StartsWith("| row.names | geneA |", StringComparison.Ordinal), all.Substring(0, 24));

        // A plain text selection in the filter box must keep its own Ctrl+A.
        d.Key('F', ctrl: true);
        var box = d.Focus();
        d.SetText(box, "abc");
        d.Settle(350);
        d.Key('A', ctrl: true);
        Check("Ctrl+A inside the filter box selects its text, not the file", d.Send(d.Sci0, NppDriver.SCI_GETSELECTIONEMPTY) == 1);
        Check("closes cleanly", d.Close());
    }

    static void Persist(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var grid = GridOf(d, VisibleHost(d));
        d.Drag(grid, FindColumnX(d, grid, 2), d.Scale(10), d.Scale(50), d.Scale(10));
        d.Settle(100);
        Check("first run closes cleanly", d.Close());
        Check("layout written", File.Exists(LayoutsFile) && File.ReadAllText(LayoutsFile).Contains("sample.tsv"));
        Check("options written", File.Exists(OptionsFile) && File.ReadAllText(OptionsFile).Contains("fontFace=Segoe UI"));

        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var lines = ExportView(d);
        Check("column order survives a restart", lines[0].StartsWith("count\t"), lines[0].Substring(0, 30));
        var menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Reset Layout of This File"], 200);
        d.Settle(100);
        lines = ExportView(d);
        Check("reset restores the file's order", lines[0].StartsWith("sample_id\t"), lines[0].Substring(0, 30));
        Check("second run closes cleanly", d.Close());
    }

    static void OptionsDialog(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Options..."], 300);
        var dialog = d.WaitDialog();
        Check("options dialog opens", dialog != IntPtr.Zero);
        if (dialog == IntPtr.Zero) return;
        var tabs = d.Item(dialog, 1001);
        for (int page = 0; page < 5; ++page) {
            d.Send(tabs, TCM_SETCURFOCUS, page);
            Thread.Sleep(120);
            d.Screenshot(dialog, Shot("options_page" + page));
        }

        // Theme is the first control of the Colours page: pick Dark and watch the table change.
        d.Send(tabs, TCM_SETCURFOCUS, 4);
        var combo = d.Children(dialog, "ComboBox").First(NppDriver.Visible);
        d.Send(combo, CB_SETCURSEL, 2);
        d.Post(dialog, WM_COMMAND, (long)((1u << 16) | (uint)GetId(combo)), (long)combo);
        Thread.Sleep(300);
        d.Screenshot(d.Main, Shot("options_live_dark"));
        d.Post(dialog, WM_COMMAND, IDCANCEL);
        Thread.Sleep(300);
        Check("dialog closes on cancel", d.TopLevel("#32770").Count == 0);
        d.Screenshot(d.Main, Shot("options_after_cancel"));

        // Change a setting for real and keep it.
        d.Command(menu["Options..."], 300);
        dialog = d.WaitDialog();
        tabs = d.Item(dialog, 1001);
        d.Send(tabs, TCM_SETCURFOCUS, 3);
        Thread.Sleep(100);
        var choices = d.Children(dialog, "ComboBox").Where(NppDriver.Visible).ToList();
        d.Send(choices[0], CB_SETCURSEL, 3);           // decimals: 2
        d.Post(dialog, WM_COMMAND, (long)((1u << 16) | (uint)GetId(choices[0])), (long)choices[0]);
        d.Send(choices[1], CB_SETCURSEL, 1);           // colour scale: per column
        d.Post(dialog, WM_COMMAND, (long)((1u << 16) | (uint)GetId(choices[1])), (long)choices[1]);
        Thread.Sleep(300);
        d.Post(dialog, WM_COMMAND, IDOK);
        Thread.Sleep(300);
        d.Screenshot(d.Main, Shot("options_heatmap"));
        Check("OK writes the options", File.ReadAllText(OptionsFile).Contains("decimals=2") && File.ReadAllText(OptionsFile).Contains("colorScale=1"));
        Check("closes cleanly", d.Close());
    }

    static int GetId(IntPtr h) => GetDlgCtrlID(h);
    [System.Runtime.InteropServices.DllImport("user32")] static extern int GetDlgCtrlID(IntPtr hwnd);

    static void ColumnsDialog(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Columns..."], 300);
        var dialog = d.WaitDialog();
        Check("column manager opens", dialog != IntPtr.Zero);
        if (dialog == IntPtr.Zero) return;
        d.Screenshot(dialog, Shot("columns_dialog"));
        d.Post(dialog, WM_COMMAND, 1104);   // hide all
        Thread.Sleep(150);
        d.Post(dialog, WM_COMMAND, 1103);   // show all
        Thread.Sleep(150);
        d.Post(dialog, WM_COMMAND, IDOK);
        Thread.Sleep(200);
        Check("columns back after hide all + show all", ExportView(d)[0].StartsWith("sample_id\tgroup\tcount"));
        Check("closes cleanly", d.Close());
    }

    static void Files(NppDriver d) {
        d.Start(exe, Open("r_rownames.tsv"));
        d.Settle(500);
        var lines = ExportView(d);
        Check("R row names: header shifted", lines[0] == "row.names\tgeneA\tgeneB\tgeneC", lines[0]);
        d.Screenshot(d.Main, Shot("file_r"));
        d.Close();

        d.Start(exe, Open("german.csv"));
        d.Settle(500);
        lines = ExportView(d);
        Check("semicolon + decimal comma: three columns", lines[0] == "Name\tGröße\tWert", lines[0]);
        d.Screenshot(d.Main, Shot("file_german"));
        d.Close();

        d.Start(exe, Open("variants.vcf.tsv"));
        d.Settle(500);
        lines = ExportView(d);
        Check("VCF: ## skipped, #CHROM is the header", lines[0] == "#CHROM\tPOS\tID\tREF\tALT" && lines.Length == 3, lines[0]);
        d.Close();

        d.Start(exe, Open("quoted.csv"));
        d.Settle(500);
        d.Screenshot(d.Main, Shot("file_quoted"));
        lines = ExportView(d);
        Check("quoted file: header", lines[0].StartsWith("id\tcomment\tvalue"), lines[0]);
        d.Close();

        foreach (var name in new[] { "empty.csv", "one_column.csv", "header_only.csv", "garbage.csv" }) {
            d.Start(exe, Open(name));
            d.Settle(400);
            var host = VisibleHost(d);
            var grid = GridOf(d, host);
            var fit = d.Children(host, "Button").First(b => NppDriver.TextOf(b) == "Fit");
            d.Post(host, WM_COMMAND, GetId(fit), (long)fit);
            d.Settle(100);
            Check(name + " survives Fit", !d.Process.HasExited);
            if (d.Process.HasExited) break;
            d.Key(VK_DOWN);
            d.Key(VK_RIGHT);
            d.Key(VK_NEXT);
            d.Key('A', ctrl: true);
            d.Wheel(grid, -120);
            d.Click(grid, d.Scale(100), d.Scale(10));
            d.Click(grid, d.Scale(100), d.Scale(60));
            d.Screenshot(d.Main, Shot("file_" + Path.GetFileNameWithoutExtension(name)));
            Check(name + " survives input", !d.Process.HasExited);
            Check(name + " closes cleanly", d.Close());
        }

        var clock = Stopwatch.StartNew();
        d.Start(exe, Open("wide.tsv"));
        d.Settle(300);
        Console.WriteLine($"  wide.tsv ready after {clock.ElapsedMilliseconds} ms");
        var wideGrid = GridOf(d, VisibleHost(d));
        d.Key(VK_END);
        d.Settle(100);
        d.Screenshot(d.Main, Shot("file_wide"));
        clock.Restart();
        for (int i = 0; i < 20; ++i) d.Key(VK_RIGHT);
        Console.WriteLine($"  20 steps in a 3001-column table: {clock.ElapsedMilliseconds} ms");
        Check("wide table survives input", !d.Process.HasExited);
        Check("wide table closes cleanly", d.Close());
    }

    static void CaretSync(NppDriver d) {
        byte[] bytes = File.ReadAllBytes(Sample("sample.tsv"));
        string text = Encoding.UTF8.GetString(bytes);
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        var grid = GridOf(d, VisibleHost(d));

        // Cell of row 3, third column, reached with the keyboard.
        d.Key(VK_DOWN); d.Key(VK_DOWN); d.Key(VK_RIGHT); d.Key(VK_RIGHT);
        d.Settle(50);
        d.Key(VK_RETURN);
        d.Settle(150);
        Check("Enter leaves the table", VisibleHost(d) == IntPtr.Zero);
        long caret = d.Send(d.Sci0, NppDriver.SCI_GETCURRENTPOS);
        string[] allLines = text.Split('\n');
        string[] row3 = allLines[3].Split('\t');
        long expected = Encoding.UTF8.GetByteCount(string.Join("\n", allLines.Take(3)) + "\n" + row3[0] + "\t" + row3[1] + "\t");
        Check("caret lands on the cell", caret == expected, $"caret={caret} expected={expected}");
        Check("focus in the editor", d.Focus() == d.Sci0);

        // And back: the caret picks the cell.
        d.Send(d.Sci0, NppDriver.SCI_GOTOPOS, expected + 1);
        d.Settle(50);
        d.Key('T', ctrl: true, alt: true);
        d.Settle(150);
        Check("table is back", VisibleHost(d) != IntPtr.Zero);
        d.Key('T', ctrl: true, alt: true);
        d.Settle(150);
        Check("the same cell is selected after a round trip", d.Send(d.Sci0, NppDriver.SCI_GETCURRENTPOS) == expected, "caret=" + d.Send(d.Sci0, NppDriver.SCI_GETCURRENTPOS));

        // Double-click opens the cell in the text.
        d.Key('T', ctrl: true, alt: true);
        d.Settle(150);
        grid = GridOf(d, VisibleHost(d));
        d.DoubleClick(grid, d.Scale(100), RowY(d, 1));
        d.Settle(150);
        Check("double-click shows the cell in the text", VisibleHost(d) == IntPtr.Zero);
        Check("closes cleanly", d.Close());
    }

    static void Split(NppDriver d) {
        d.Start(exe, Open("plain.txt", "sample.tsv"));
        d.Settle(500);
        d.Send(d.Main, NppDriver.NPPM_MENUCOMMAND, 0, IDM_VIEW_GOTO_ANOTHER_VIEW);
        d.Settle(400);
        d.Screenshot(d.Main, Shot("split"));
        Check("table moved to the second view", d.Send(d.Main, NppDriver.NPPM_GETCURRENTVIEW) == 1);
        var host = VisibleHost(d);
        Check("overlay follows to the second view", host != IntPtr.Zero && NppDriver.RectOf(host).ToString() == NppDriver.RectOf(d.Sci1).ToString(),
              (host == IntPtr.Zero ? "none" : NppDriver.RectOf(host).ToString()) + " vs " + NppDriver.RectOf(d.Sci1));
        Check("left view is uncovered text", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 0);
        Check("only the table's view is covered", d.Children(d.Main, "xsvViewerHost").Count(NppDriver.Visible) == 1,
              "visible overlays: " + d.Children(d.Main, "xsvViewerHost").Count(NppDriver.Visible));

        d.Click(d.Sci0, 100, 100);
        d.Settle(150);
        Check("clicking the left view activates it", d.Send(d.Main, NppDriver.NPPM_GETCURRENTVIEW) == 0);
        d.Click(GridOf(d, host), d.Scale(100), RowY(d, 2));
        d.Settle(200);
        Check("clicking the table activates its view", d.Send(d.Main, NppDriver.NPPM_GETCURRENTVIEW) == 1, "view=" + d.Send(d.Main, NppDriver.NPPM_GETCURRENTVIEW));
        Check("focus is in the table's grid", d.Focus() == GridOf(d, host), NppDriver.ClassOf(d.Focus()));

        d.Send(d.Main, NppDriver.NPPM_MENUCOMMAND, 0, IDM_VIEW_GOTO_ANOTHER_VIEW);
        d.Settle(300);
        host = VisibleHost(d);
        Check("moving back keeps the table", host != IntPtr.Zero && NppDriver.RectOf(host).ToString() == NppDriver.RectOf(d.Sci0).ToString());
        Check("closes cleanly", d.Close());
    }

    // One document open in both views at once.
    static void Clone(NppDriver d) {
        d.Start(exe, Open("sample.tsv"));
        d.Settle(500);
        d.Send(d.Main, NppDriver.NPPM_MENUCOMMAND, 0, IDM_VIEW_CLONE_TO_ANOTHER_VIEW);
        d.Settle(400);
        Check("a clone is a table in both views", d.Children(d.Main, "xsvViewerHost").Count(NppDriver.Visible) == 2);

        d.Key('T', ctrl: true, alt: true);
        d.Settle(250);
        Check("switching to text uncovers both views", d.Children(d.Main, "xsvViewerHost").Count(NppDriver.Visible) == 0,
              "visible overlays: " + d.Children(d.Main, "xsvViewerHost").Count(NppDriver.Visible));
        Check("text is editable", d.Send(d.ActiveSci(), NppDriver.SCI_GETREADONLY) == 0);

        d.Key('T', ctrl: true, alt: true);
        d.Settle(250);
        Check("and back to a table in both", d.Children(d.Main, "xsvViewerHost").Count(NppDriver.Visible) == 2);

        // Closing one of the two must not leave the survivor unusable.
        d.Key('W', ctrl: true);
        d.Settle(300);
        d.Key('T', ctrl: true, alt: true);
        d.Settle(250);
        Check("the remaining view can leave the table", VisibleHost(d) == IntPtr.Zero);
        Check("and its text is editable", d.Send(d.ActiveSci(), NppDriver.SCI_GETREADONLY) == 0, "ro=" + d.Send(d.ActiveSci(), NppDriver.SCI_GETREADONLY));
        Check("closes cleanly", d.Close());
    }

    static void Ansi(NppDriver d) {
        d.Start(exe, Open("ansi.csv"));
        d.Settle(500);
        var host = VisibleHost(d);
        Check("ansi file opens as a table", host != IntPtr.Zero);
        d.Screenshot(d.Main, Shot("file_ansi"));
        d.SetText(FilterBox(d, host), "Änne");
        d.Settle(450);
        var lines = ExportView(d);
        Check("a filter with umlauts matches an ANSI file", lines.Length == 2, string.Join(" | ", lines));
        d.Key(VK_ESCAPE);
        d.Settle(100);
        d.SetText(FilterBox(d, host), "");
        d.Settle(450);
        lines = ExportView(d);
        Check("export converts to the new tab's encoding", lines.Length == 4 && lines[1].StartsWith("Änne", StringComparison.Ordinal), lines[1]);

        Pick(d, GridOf(d, host), d.Scale(50), RowY(d, 2), "Show only this value");
        lines = ExportView(d);
        Check("filter by value works on ANSI text", lines.Length == 2 && lines[1].StartsWith("Ömer", StringComparison.Ordinal), string.Join(" | ", lines));
        d.Key('L', ctrl: true, shift: true);
        d.Settle(150);
        d.Click(GridOf(d, host), d.Scale(50), HeaderHeight(d) + RowHeight(d) / 2);
        d.Settle(100);
        var edit = d.Children(GridOf(d, host), "Edit").FirstOrDefault(NppDriver.Visible);
        Check("the filter editor shows it readably", edit != IntPtr.Zero && d.ControlText(edit) == "==Ömer", edit == IntPtr.Zero ? "none" : d.ControlText(edit));
        Check("closes cleanly", d.Close());
    }

    // Without -nosession, because with it Notepad++ deliberately keeps its saved docking layout.
    static void PanelRestore(NppDriver d) {
        string configFile = Path.Combine(Path.GetDirectoryName(exe), "config.xml");
        string withSession = "-multiInst \"" + Sample("plain.txt") + "\"";
        d.Start(exe, withSession);
        d.Settle(500);
        d.Command(d.PluginMenu("xsvViewer")["Table Panel"], 400);
        d.Settle(200);
        Check("panel is open", VisibleHost(d) != IntPtr.Zero);
        Check("first run closes cleanly", d.Close());
        var saved = File.ReadAllLines(configFile).Where(l => l.Contains("PluginDlg") || l.Contains("ActiveTabs")).Select(l => l.Trim()).ToArray();
        Console.WriteLine("  " + string.Join("\n  ", saved));
        Check("Notepad++ recorded the open panel", saved.Any(l => l.Contains("xsvViewer.dll") && l.Contains("isVisible=\"yes\"")));

        d.Start(exe, withSession);
        d.Settle(700);
        Check("open panel comes back after a restart", VisibleHost(d) != IntPtr.Zero);
        d.Screenshot(d.Main, Shot("panel_restored"));
        d.Command(d.PluginMenu("xsvViewer")["Table Panel"], 300);
        d.Settle(200);
        Check("the restored panel can be closed", VisibleHost(d) == IntPtr.Zero);
        Check("second run closes cleanly", d.Close());
        saved = File.ReadAllLines(configFile).Where(l => l.Contains("PluginDlg")).Select(l => l.Trim()).ToArray();
        Check("a closed panel stays closed next time", saved.All(l => !l.Contains("isVisible=\"yes\"")), string.Join(" ", saved));
    }

    static void Panel(NppDriver d) {
        d.Start(exe, Open("plain.txt"));
        d.Settle(500);
        var menu = d.PluginMenu("xsvViewer");
        d.Command(menu["Table Panel"], 400);
        d.Settle(200);
        var panel = VisibleHost(d);
        Check("panel opens", panel != IntPtr.Zero);
        Check("panel leaves the text visible and editable", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 0 && NppDriver.Visible(d.Sci0));
        Check("menu item is checked", d.MenuChecked(menu["Table Panel"]));
        d.Screenshot(d.Main, Shot("panel"));

        // Typing updates the panel.
        Console.WriteLine("  focus after opening the panel: " + NppDriver.ClassOf(d.Focus()));
        d.Send(d.Sci0, NppDriver.SCI_GOTOPOS, 0);
        foreach (char c in "typed,") d.Char(c);
        Thread.Sleep(600);
        d.Settle(100);
        var lines = ExportView(d);
        Check("typing reaches the table", lines[0].Contains("typed") || lines.Any(l => l.Contains("typed")), lines[0]);
        d.Send(d.ActiveSci(), SCI_SETSAVEPOINT);

        d.Command(menu["Table Panel"], 300);
        d.Settle(200);
        Check("panel closes", VisibleHost(d) == IntPtr.Zero);
        d.Command(menu["Table Panel"], 300);
        d.Settle(200);
        Check("panel reopens", VisibleHost(d) != IntPtr.Zero);
        Check("closes cleanly", d.Close());
    }

    static void Reload(NppDriver d) {
        string file = Sample("reload.tsv");
        File.WriteAllText(file, "a\tb\n1\t2\n", new UTF8Encoding(false));
        d.Start(exe, Open("reload.tsv"));
        d.Settle(500);
        Check("two columns at first", ExportView(d)[0] == "a\tb");

        File.WriteAllText(file, "a\tb\tc\n1\t2\t3\n4\t5\t6\n", new UTF8Encoding(false));
        d.Post(d.Main, WM_COMMAND, IDM_FILE_RELOAD);
        var prompt = d.WaitDialog();
        if (prompt != IntPtr.Zero) d.Post(prompt, WM_COMMAND, IDYES);
        Thread.Sleep(900);
        d.Settle(200);
        var lines = ExportView(d);
        Check("table follows a reload from disk", lines[0] == "a\tb\tc" && lines.Length == 3, string.Join("|", lines));
        Check("text is locked again after the reload", d.Send(d.Sci0, NppDriver.SCI_GETREADONLY) == 1);
        Check("closes cleanly", d.Close());
    }

    static void Big(NppDriver d) {
        string file = Sample("big.tsv");
        if (!File.Exists(file)) {
            using var w = new StreamWriter(file, false, new UTF8Encoding(false), 1 << 20);
            w.Write("id\tgroup\tv1\tv2\tv3\tv4\tv5\tv6\n");
            var random = new Random(1);
            for (int i = 0; i < 1000000; ++i)
                w.Write($"row{i}\tg{i % 7}\t{random.NextDouble():F6}\t{random.NextDouble():F6}\t{random.NextDouble():F6}\t{random.NextDouble():F6}\t{random.NextDouble():F6}\t{random.NextDouble():F6}\n");
        }
        var clock = Stopwatch.StartNew();
        d.Start(exe, Open("big.tsv"));
        for (int i = 0; i < 300 && VisibleHost(d) == IntPtr.Zero; ++i) Thread.Sleep(100);
        d.Settle(200);
        Console.WriteLine($"  1M rows ({new FileInfo(file).Length / 1048576} MB): table up after {clock.ElapsedMilliseconds} ms");
        var host = VisibleHost(d);
        Check("large file opens as a table", host != IntPtr.Zero);
        var grid = GridOf(d, host);

        clock.Restart();
        d.Click(grid, d.Scale(100), d.Scale(10));
        d.Settle(100);
        Console.WriteLine($"  sort: {clock.ElapsedMilliseconds} ms");

        clock.Restart();
        d.SetText(FilterBox(d, host), "g3");
        d.Settle(400);
        Console.WriteLine($"  filter: {clock.ElapsedMilliseconds} ms");

        clock.Restart();
        d.Key(VK_END, ctrl: true);
        for (int i = 0; i < 30; ++i) d.Wheel(grid, 120);
        Console.WriteLine($"  scrolling: {clock.ElapsedMilliseconds} ms");
        d.Screenshot(d.Main, Shot("big"));
        Check("large file stays responsive", !d.Process.HasExited);
        Check("closes cleanly", d.Close(20000));
    }
}
