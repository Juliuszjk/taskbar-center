# TaskbarCenter

Keeps Windows 10 taskbar app icons centered on the screen. Start, search and
the tray stay at the edges. When you open or close an app, the icons move back
to the middle of the monitor. Works on every taskbar when several monitors are
connected.

- No installer, no admin rights, no tray icon, no window.
- Runs only when Windows reports a change. A light check every 2 seconds is a
  safety net in case an event is missed.
- Stopping it puts the taskbar back exactly as it was.

## Usage

Put `TaskbarCenter.exe` in a folder it can stay in, then run it:

| Command | What it does |
| --- | --- |
| `TaskbarCenter.exe` | Start centering now (until sign-out) |
| `TaskbarCenter.exe --install` | Start now and at every sign-in |
| `TaskbarCenter.exe --uninstall` | Stop and remove from sign-in |
| `TaskbarCenter.exe --stop` | Stop and restore the taskbar |
| `TaskbarCenter.exe --debug` | Start and write `TaskbarCenter.log` |
| `TaskbarCenter.exe --help` | Show the list of options |

To pass an option, use a shortcut with the option at the end of *Target*, or
run it from `cmd` / PowerShell, or press Win+R and type the full path.

If something looks wrong, run `--stop`, then start it with `--debug`, repeat
what went wrong and send `TaskbarCenter.log`. The log is written next to the
exe, or to `%TEMP%` if that folder is read-only.

## Building

On Linux with `mingw-w64-gcc`:

```sh
./build.sh   # produces build/TaskbarCenter.exe
```

## How it works

The taskbar buttons live in a window of class `MSTaskListWClass`
(`Shell_TrayWnd` → `ReBarWindow32` → `MSTaskSwWClass` → `MSTaskListWClass` on
the main taskbar, `Shell_SecondaryTrayWnd` → `WorkerW` → `MSTaskListWClass` on
the others). The program measures the buttons with UI Automation, works out
where they must start to be centered on the monitor, keeps them inside the
button area so they never cover Start, search or the tray, and moves
`MSTaskListWClass` with `SetWindowPos`.

Triggers: app windows shown or hidden, layout events from Explorer's taskbar
windows, Explorer restarts (`TaskbarCreated`), display and setting changes.
Each trigger is followed by checks at 60, 310 and 1010 ms so button grow and
shrink animations are caught.

## Test checklist

1. Run `TaskbarCenter.exe --debug`. The icons jump to the middle of the screen.
2. Open and close a few apps. The icons stay centered.
3. Open several windows of one app, pin and unpin an icon, drag an icon.
4. Restart Explorer (Task Manager → Windows Explorer → Restart). The icons are
   centered again within a few seconds.
5. Change the resolution and the scaling (Settings → Display).
6. Turn the search box on and off (right-click the taskbar → Search).
7. Check Task Manager: a few MB of memory, 0% CPU at rest.
8. Run `--stop`. The icons return to the left.
9. Run `--install`, sign out and back in. The icons are centered. Run
   `--uninstall` afterwards.
10. Several monitors: repeat steps 2 and 5, and connect or disconnect a
    monitor while it runs.
