# ancora

**Stops thumbstick drift in SteamVR.**

Is your character walking or turning on its own in VRChat (or another
SteamVR game) when you are not touching the stick? That is stick drift.
ancora fixes it: when your thumb is **off** the stick, games get "stick
centered". When your thumb is **on** the stick, everything works as normal.

- Works with Quest controllers (Touch style sticks with a thumb sensor).
- Does not touch the game or its files. It runs inside SteamVR only.
- Free, small, and you can turn it off or remove it at any time.

---

## What you need

- A Windows 10 or 11 PC (64-bit).
- SteamVR installed and started at least once.
- Your Quest connected to SteamVR (for example with Steam Link).

Nothing else. No extra runtimes, no admin rights.

---

## Install

1. **Close SteamVR** completely.
2. Run **`ancora-setup-0.1.0.exe`**.
   - If Windows says "Windows protected your PC", click **More info**, then
     **Run anyway**. (This happens because the installer is not signed with a
     paid certificate.)
3. Click through the installer. It does not ask you anything difficult.
4. **Start SteamVR.** That is it. ancora is now active.

You will see a small **anchor icon** in the Windows tray (bottom right, near
the clock). That is the ancora panel.

---

## Use it

**You do not have to do anything.** Drift is fixed as soon as SteamVR runs.

If you want to look or change something, **click the anchor icon** in the tray.

### Status page

One card per controller:

| You see | It means |
|---|---|
| **Drift anchored** (green) | Your thumb is off the stick, games get "centered". Drift is blocked. |
| **Following thumb** (yellow) | Your thumb is on the stick, it works normally. |
| **In deadzone** (green) | Your thumb is on, but the stick is only pushed a little, so games get "centered". |
| **Not corrected** | ancora is switched off for this controller. |
| **Not connected** | The controller is off, asleep, or its battery is out. |

Each card has:

- **Switch at the top**: ancora on or off for this controller.
- **Thumb gate**: the main fix. Keep it **on**.
- **Deadzone**: only needed if your stick drifts even while your thumb rests
  on it. Leave it **off** unless you need it. Higher numbers mean you must push
  the stick further before you move.

**Anchoring on** (top right) turns ancora on or off for everything.

Changes save by themselves and work immediately. No restart.

### Tune page

Not sure about the deadzone? Let ancora measure your stick:

1. Pick the controller, press **Start measuring**.
2. **Let go of the stick** for 5 seconds.
3. **Push the stick all the way, then let go.** Do this 6 times. Wait about a
   second each time after letting go.
4. Read the result and press **Apply** if you want the suggested deadzone.

### Diagnostics page

Only needed when something is wrong. **Copy diagnostics** copies a report you
can send to whoever helps you.

### Keyboard

Tab or arrow keys move around, Enter selects, Left/Right change a slider,
Esc goes back.

---

## Turn it off

- **For a moment:** click the anchor icon, switch **Anchoring** off.
- **For one controller:** switch that controller's card off.
- **Completely:** uninstall (below).

---

## Uninstall

1. **Close SteamVR.**
2. Windows **Settings > Apps > Installed apps**, find **ancora**, click
   **Uninstall**.

This removes everything ancora added, including its SteamVR entries. Nothing
is left behind.

---

## Something is wrong?

| Problem | Try this |
|---|---|
| No anchor icon in the tray | Start SteamVR, ancora starts with it. Or run `%LOCALAPPDATA%\ancora\ancora.exe`. The icon can also be hidden behind the small up arrow in the tray. |
| Installer says "SteamVR is running" | Close SteamVR fully (also from the tray), then press OK. |
| Still drifting | Open the panel. Is the card **Drift anchored** when your thumb is off? If it says **Not corrected**, switch it on. If it drifts with your thumb **on** the stick, use **Tune** and apply a deadzone. |
| A controller shows **Not connected** | Wake the controller or check its battery. |
| I cannot walk slowly any more | Your deadzone is too high. Lower it, or turn it off. |
| My controller is a Valve Index | Not supported yet. ancora does nothing on Index controllers. |
| Game does not use SteamVR | ancora only works for games that run through SteamVR. |

Still stuck? Open the panel, **Diagnostics > Copy diagnostics**, and send that
text along with your question.

---

## Questions

**Can this get me banned in VRChat?**
ancora never touches VRChat or its files. It sits inside SteamVR and only
changes what your own stick reports, like a SteamVR controller setting.
Nobody can promise anything for VRChat's rules, but ancora does not modify
the game.

**Does it slow anything down?**
No. It uses a few MB of memory and almost no CPU, and nothing at all while
the panel is closed.

**Does it change my buttons or other controller settings?**
No. Only the thumbstick, and only when needed. Buttons, triggers, grip,
haptics and your SteamVR bindings are untouched.

**Where are my settings?**
In `%LOCALAPPDATA%\ancora\ancora.ini`. The panel edits it for you; you
never need to open it.

---

## For developers

Everything is rebuilt from source; the repo holds no binaries.

| Command | Does |
|---|---|
| `build_driver.bat` | Builds the logic dll only. While SteamVR runs, the installed driver swaps it in live. |
| `build_driver.bat all` | Builds everything: driver, panel (`ancora.exe`), diagnostic tools. Fetches the OpenVR SDK on first run. |
| `install.ps1` | Builds and installs from the repo (SteamVR closed). |
| `uninstall.ps1` | Removes the install (SteamVR closed). |
| `package.bat` | Builds and creates `dist\ancora-setup-<version>.exe` with Inno Setup (fetched automatically, nothing installed). |

Needs Visual Studio 2022 Build Tools with C++ (x64), git, and Windows 10 or 11.

Layout:

- `driver/src/driver_ancora.cpp` fixed loader: hooks SteamVR's driver input calls.
- `driver/src/ancora_logic.cpp` the behaviour, hot reloadable.
- `gui/src/ancora_panel.cpp` the panel.
- `steamvr.ps1` SteamVR registration, used by the scripts and the installer.
- `packaging/ancora.iss` installer script.
