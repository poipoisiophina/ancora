# ancora

**Stops thumbstick drift in SteamVR.**

Is your character walking or turning on its own in VRChat (or another
SteamVR game) when you are not touching the stick? That is stick drift.
ancora fixes it: when your thumb is **off** the stick, games get "stick
centered". When your thumb is **on** the stick, everything works as normal.

- Works with Quest controllers (Touch style sticks with a thumb sensor).
- Does not touch the game or its files. It runs inside SteamVR only, where it
  hooks the calls controller drivers use to report the stick.
- Free, small, and you can turn it off or remove it at any time.

---

## What you need

- A Windows 10 or 11 PC (64-bit).
- SteamVR installed and started at least once.
- Your Quest connected to SteamVR. Tested with Steam Link. Virtual Desktop,
  ALVR and Quest Link also hand the controllers to SteamVR through a driver, so
  they should work the same way, but they are not tested yet. If ancora does
  not see a stick, its card on the Status page says **Not connected**.

Nothing else. No extra runtimes, no admin rights.

---

## Install

1. **Close SteamVR** completely.
2. Download the newest **`ancora-setup-<version>.exe`** from
   [Releases](https://github.com/poipoisiophina/ancora/releases) (a
   pre-release is fine) and run it.
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
- **Thumb gate**: the main fix. Keep it **on**. If the thumb sensor misses
  your thumb (a worn sensor, a silicone cap, pushing with the side of your
  thumb), holding the stick at its edge for a quarter second still works:
  ancora stops trusting the sensor until it sees your thumb again. The card's
  **Thumb** box then says **sensor ignored**.
- **Deadzone**: only needed if your stick drifts even while your thumb rests
  on it. Leave it **off** unless you need it. Higher numbers mean you must push
  the stick further before you move. SteamVR's own controller bindings can
  also set a deadzone; ancora's is per stick and the Tune page measures it for
  you, but you only need one of the two.
- **Smoothing**: steadies a stick that shakes while your thumb is on it. It does
  not fix drift, and higher numbers add lag to your movement. Leave it **off**
  unless your stick shakes.

**Anchoring on** (top right) turns ancora on or off for everything.

Changes save by themselves and work immediately. No restart.

### Tune page

Not sure about the deadzone? Let ancora measure your stick:

1. Pick the controller, press **Start measuring**.
2. **Let go of the stick** for 5 seconds.
3. **Push the stick all the way, then let go.** Do this 6 times. Wait about a
   second each time after letting go.
4. Read the result and press **Apply** if you want the suggested deadzone.

### Stick does not reach full in some directions?

A worn stick can stop short on one side: for example, you push it all the way
to the top left, but the game only gets about two thirds. ancora can stretch
those directions back to full:

1. On the Tune page, pick the controller, press **Measure edge**.
2. **Push the stick all the way and circle slowly** along its edge. Dots turn
   yellow where the edge is reached and stay red where it needs another pass;
   it finishes by itself once every direction is yellow.
3. The plot shows how far the stick reaches in each direction. Press
   **Stretch ... stick** to apply it.

A worn stick does not stop at the same place every time, so the measurement
can come out short in places. While you play, ancora keeps watching: when the
stick is held further than the measured edge in some direction, that direction
is raised to match, and saved a few seconds later. A stick also keeps wearing:
when many full pushes in a row toward one direction all stop clearly short of
its edge, that direction is lowered part of the way, but never more than 0.10
below the furthest the stick has reached there (measure again on the Tune page
if it has worn more than that). Only that one direction
changes, so a worn spot stays stretched even when the stick goes further right
beside it, and readings shorter than a fiftieth of a second are ignored. Games
get full a little (4 %) before the furthest edge seen, so an ordinary full
push still counts as full.

The stretched directions get a little more sensitive near the edge. Your
deadzone keeps working as before. The Status page draws the outline in yellow
while a stretch is on. To undo it, press **Remove ... stretch** on the Tune page.

### Diagnostics page

Only needed when something is wrong. **Copy diagnostics** copies a report you
can send to whoever helps you.

### Keyboard

Tab or arrow keys move around, Enter selects, Left/Right change a slider,
Esc goes back.

### Language

ancora speaks your Windows language when it has it. To pick another one,
click the **globe** at the bottom left of the panel. It has VRChat's
languages, English, Deutsch, Español, Français, Italiano, 日本語, 한국어,
עברית, Português (Brasil), Polski, 简体中文, 繁體中文, Русский and
toki pona, plus Українська, العربية and فارسی, and every official EU
language: Български, Čeština, Dansk, Eesti, Ελληνικά, Gaeilge, Hrvatski,
Latviešu, Lietuvių, Magyar, Malti, Nederlands, Português (Portugal), Română,
Slovenčina, Slovenščina, Suomi and Svenska. Hebrew, Arabic and Persian turn
the whole panel right to left.

The translations were drafted by machine. If yours reads oddly, see
"Translate ancora" below.

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

This removes everything ancora added, including its SteamVR entries. Before
it changes a SteamVR settings file, ancora saves a copy to
`%TEMP%\ancora-backup`, so you can put a file back if something went wrong.
Windows clears that folder with its other temporary files.

---

## Something is wrong?

| Problem | Try this |
|---|---|
| No anchor icon in the tray | Start SteamVR, ancora starts with it. Or run `%LOCALAPPDATA%\ancora\ancora.exe`. The icon can also be hidden behind the small up arrow in the tray. |
| Installer says "SteamVR is running" | Close SteamVR fully (also from the tray), then press OK. |
| Still drifting | Open the panel. Is the card **Drift anchored** when your thumb is off? If it says **Not corrected**, switch it on. If it drifts with your thumb **on** the stick, use **Tune** and apply a deadzone. |
| A controller shows **Not connected** | Wake the controller or check its battery. If it stays that way while the controller works in games, restart SteamVR with the controller already on: ancora only sees sticks a controller driver sets up after ancora has started. |
| I cannot walk slowly any more | Your deadzone is too high. Lower it, or turn it off. |
| My controller is a Valve Index | Not supported yet. ancora does nothing on Index controllers. |
| Game does not use SteamVR | ancora only works for games that run through SteamVR. |

Still stuck? Open the panel, **Diagnostics > Copy diagnostics**, and send that
text along with your question.

---

## Questions

**Can this get me banned in VRChat?**
ancora never touches VRChat or its files. It is a SteamVR driver: inside
SteamVR's own process it intercepts the stick values your controller driver
reports and changes them before any game sees them. A game only ever sees
ordinary stick values. Nobody can promise anything for VRChat's rules, but
ancora does not modify the game.

**Does it slow anything down?**
No. The driver runs inside SteamVR whenever SteamVR runs and does a few
calculations per stick update. The panel uses a few MB of memory and draws
nothing while it is hidden in the tray.

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
| `build_driver.bat all` | Builds everything: driver, panel (`ancora.exe`), diagnostic tools. Fetches the OpenVR SDK (pinned release, checked by commit) on first run. |
| `install.ps1` | Builds and installs from the repo (SteamVR closed). |
| `uninstall.ps1` | Removes the install (SteamVR closed). |
| `package.bat` | Builds and creates `dist\ancora-setup-<version>.exe` with Inno Setup (fetched automatically, nothing installed). |

Needs Visual Studio 2022 Build Tools with C++ (x64), git, and Windows 10 or 11.

Tests run on every build, and a failure stops it, so nothing untested is
deployed into a running SteamVR or packed into an installer:

- `test/ancora_test.cpp`, driver side: edge stretch, edge measuring, edge
  learning (raising and lowering), whole x/y pairs, saving learned reach,
  smoothing, the output order, the thumb gate and its fallback, ancora.ini
  reading and key edits, and that the shipped `ancora.ini` is the default
  file. Runs before any dll is built, also in the quick logic only build.
- `ancora.exe --selftest`, panel side: ini files, the Tune page flow, every
  language file, the SteamVR settings edits. Runs in `build_driver.bat all`,
  so also in `package.bat`.

GitHub Actions runs `package.bat` on pushes to `main` and on pull requests,
skipping changes that only touch `.md` files (`.github/workflows/build.yml`),
and keeps the installer and test logs for 7 days.

Releasing: raise `AppVersion` in `packaging/ancora.iss` and push to `main`.
When that version has no tag yet and the build passes, the workflow tags the
commit `v<version>` and publishes the tested installer as the pre-release
`ancora <version>` with generated notes. A version that already has a tag is
never released again. Mark a pre-release as a full release by hand on GitHub.

Layout:

- `driver/src/driver_ancora.cpp` fixed loader: hooks SteamVR's driver input calls.
- `driver/src/ancora_logic.cpp` the behaviour, hot reloadable.
- `driver/src/ancora_edge.h`, `ancora_learn.h`, `ancora_average.h`,
  `ancora_output.h` the stick math and the thumb gate, shared by the logic dll,
  the panel and the tests.
- `driver/src/ancora_config.h`, `ancora_ini_io.h` ancora.ini: defaults,
  reading, and locked, atomic key writes, shared by the driver and the panel.
- `test/ancora_test.cpp` driver side tests.
- `gui/src/ancora_panel.cpp` the panel.
- `gui/src/ancora_lang.h` panel text loading; `gui/lang/*.xml` one file per language.
- `gui/src/ancora_steamvr.h` SteamVR registration (`ancora.exe --check`,
  `--register`, `--unregister`), used by the scripts and the installer, so the
  installer runs no PowerShell.
- `packaging/ancora.iss` installer script; `packaging/messages.iss` its translated messages.

### Translate ancora

1. Copy `gui/lang/en.xml` to `<code>.xml`, where code is a language tag
   such as `nl`, `pt-PT` or `ar`.
2. Set `code`, `name` (the language's own name), `dir` (`ltr`, or `rtl` for
   Arabic, Hebrew, Persian...) and `font` on the first line.
3. Translate the text inside each element. Keep the element names and every
   `{placeholder}`; you may move a placeholder within the sentence.
4. To try it without building: put the file in a `lang` folder next to
   `ancora.exe` and pick it from the globe menu. A file there replaces a
   built-in language with the same code.
5. To check it: `ancora.exe --screenshot shot.png --fake --page tune --lang <code>`
   renders a page and lists any text that does not fit. `ancora.exe --selftest`
   checks that every built-in language has every string with the right
   placeholders. `--lang qps-ploc` (40 % longer text) and `--lang qps-plocm`
   (the same, right to left) are pseudo languages for layout testing.

`build_driver.bat all` builds every `gui/lang/*.xml` into `ancora.exe`.
A string missing from a language shows in English, and Diagnostics says how many.

## License

ancora is released under the BSD 3-Clause License, the same license as the OpenVR
SDK it builds against: see [LICENSE](LICENSE). The installer ships OpenVR's
`openvr_api.dll`; its notice is in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

ancora is not affiliated with, endorsed by, or sponsored by Valve or Meta.
SteamVR, Steam, Meta and Quest are trademarks of their respective owners, used
here only to say what ancora works with.
