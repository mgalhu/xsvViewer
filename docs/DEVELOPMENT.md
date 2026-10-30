# Development

## Build

Needs the Visual Studio 2022 Build Tools with the C++ x64 workload. CMake and Ninja come with them.

```powershell
.\build.ps1             # build, run the unit tests, copy the DLL to dist\xsvViewer\
.\build.ps1 -Config Debug
.\install.ps1           # copy into the installed Notepad++ (asks for administrator rights)
```

The DLL links the C runtime statically, so it needs no redistributable.

## Layout

```
src/core     parsing, filters, sorting, export, settings. No Win32, all unit tested.
src/plugin   Notepad++ integration, grid, toolbar, dialogs.
src/npp      Notepad++ and Scintilla API headers, from tag v8.9.8.1.
tests        unit tests (test_core.cpp), GUI tests (gui/).
docs         user guide, this file, screenshots.
```

## How it works

**Table** (`src/core/Table.h`) keeps the text and one offset per row, nothing else. Rows are parsed when
they are needed, by `scanRecord` in `Scanner.h`. Dialect detection uses the same scanner, so the two
can't disagree about where a record ends. This is why a table with millions of rows costs little memory
beyond the text.

**Document** (`src/plugin/Document.h`) is one open file as a table: the Table, its layout, and the current
view, meaning which rows in which order. There is one for each Notepad++ buffer that needs it. It tells
its observers when something changes.

**Grid** (`src/plugin/Grid.h`) is an owner-drawn window that paints only what is on screen, double
buffered. **TableHost** wraps it with the toolbar and status bar.

**In the tab.** A TableHost is a sibling of each Scintilla view, placed exactly over it. Scintilla is
subclassed to follow its position, forward focus to the grid, and suppress its painting while covered,
because with DirectWrite it would otherwise draw over the table.

**The text is locked** with `SCI_SETREADONLY` while covered, since Notepad++'s accelerators act on the
hidden editor regardless of focus. The previous state is restored on leaving.

**Keys.** Accelerators run before a window sees a key, so a `WH_GETMESSAGE` hook gives the grid and the
text boxes first refusal. It must stay out of the way while a menu is open: menus read the same queue,
with focus still on the grid, and intercepted keys would never reach them.

**Menus run their own message loop**, so timers fire while one is open, and the table can change
underneath it. `Grid::headerMenu` and `cellMenu` hold on to the source row and file column, and check them
again afterwards.

## Tests

```powershell
.\build\Release\xsvtests.exe          # unit tests for src\core
.\build\Release\xsvtests.exe --perf   # timings on a 2-million-row table
.\tests\gui\run.ps1 -Npp <portable Notepad++ folder> -Work <scratch folder> [-Scenario name]
```

The GUI tests drive a real Notepad++ on a hidden desktop, so they never take over the screen or the
clipboard. Use a portable copy kept for this purpose: they replace its plugin settings, and add a test
plugin (`tests\gui\TestShim.cpp`) that picks menu items by label and captures clipboard writes. It is built
as `xsvTestShim.dll` and must never ship. The `realMenu` scenario switches the menu part off to run real
menus.

Things that will catch you out when reading results:

- **Capturing text.** Notepad++ draws with DirectWrite by default, which a hidden desktop can't capture.
  Set `writeTechnologyEngine="0"` in its `config.xml` before trusting a screenshot of the text. Stale
  pixels otherwise look exactly like a repaint bug.
- **`-nosession`** makes Notepad++ keep its saved docking layout, so the `panelRestore` scenario runs
  without it.
- **`Ctrl+Tab`** opens the document switcher, which only commits when Ctrl is really released.
- **Dialogs** need a moment to paint on a hidden desktop.
- **The leak checks in `resources`** did not fail against a build with the bitmap and font handling
  deliberately broken, so they only guard against other leaks.

To regenerate the pictures in `docs\images`, set `writeTechnologyEngine="0"`, run the `docs` scenario, and
copy them from `<scratch folder>\shots\docs`. They are cropped so that the title bar, which shows the full
file path, stays out.

## Making a release

1. Set the version in `src/plugin/xsvViewer.rc` (`FILEVERSION`, `PRODUCTVERSION` and the strings) and the
   About text in `PluginMain.cpp`, and add it to `CHANGELOG.md`.
2. Run the GUI tests, ideally against a build from a clean clone.
3. Run `.\release.ps1`. It builds and runs the unit tests, then writes to `dist\`:
   - `xsvViewer-<version>-x64.zip`, with the DLL at the root and the guide in `doc\`, which Plugins Admin
     installs into `plugins\doc\xsvViewer`
   - `pl-entry.json`, the entry for the plugin list, with the zip's SHA-256
4. Create a GitHub release with that zip, tagged `v<version>`.
5. [Test it locally](https://npp-user-manual.org/docs/plugins/#test-your-plugins-locally) in Plugins Admin
   with a debug build of Notepad++ and an edited `nppPluginList.json`: install, use, remove.
6. Add or update the entry in `src/pl.x64.json` of
   [nppPluginList](https://github.com/notepad-plus-plus/nppPluginList) in a pull request. Change nothing
   but that file. The version must match the DLL exactly, and the
   [rules](https://npp-user-manual.org/docs/plugins/#rules-for-adding-your-plugins-into-list) cover the
   rest.
7. Announce new features in the
   [plugin development forum](https://community.notepad-plus-plus.org/category/5/plugin-development).
