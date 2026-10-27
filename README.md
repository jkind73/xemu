# xemu Chihiro Fork

A specialized fork of [xemu](https://xemu.app) dedicated to emulating the **Sega Chihiro** arcade platform (built on custom original Xbox hardware). It boots authentic arcade firmware, speaks native Chihiro I/O protocols (JVS, AN2131/8051, drive boards, card readers), and brings authentic arcade experiences to modern PC setups.

---

## Supported Games

| Game | Status | Notes |
|---|---|---|
| **Crazy Taxi High Roller** | ✅ | Force feedback & auto-centering supported |
| **Ghost Squad** | ✅ | Full light gun & Tamura HW210 card reader support |
| **Mobile Suit Gundam: Battle Operating Simulator** | ✅ | Network firmware required; Master Billboard dual-display supported |
| **Mobile Suit Gundam 0079: Card Builder** | ✅ | Network firmware 18.07 bypass integrated; Master Billboard supported |
| **Ollie King** | ✅ | Multi-cabinet LAN linking supported |
| **OutRun 2** | ✅ | Force feedback & multi-cabinet linking supported |
| **OutRun 2 SP** | ✅ | Network firmware required; Force feedback & LAN linking |
| **The House of the Dead III** | ✅ | Light gun calibration supported |
| **Virtua Cop 3** | ✅ | Dual light gun & pedal support |
| **Wangan Midnight: Maximum Tune** | ✅ | Namco V257 drive board & Sanwa card reader support |
| **Wangan Midnight: Maximum Tune 2** | ✅ | Namco V257 drive board & Sanwa card reader support |

---

## What This Fork Brings to the Table

### 1. Gundam Card Builder Network Firmware 18.07 Bypass
* *Mobile Suit Gundam 0079: Card Builder* (`G_XB.xbe`) normally refuses to boot without arcade network firmware version **18.07** or higher.
* Because only firmware version 13.05 (`ver1305.bin`) is publicly dumped, this fork integrates an automated in-memory firmware check bypass for `G_XB.xbe`, `G_TEST.xbe`, and `G_XB_df.xbe`.
* **Result**: Gundam Card Builder boots and plays seamlessly using standard `ver1305.bin` firmware.

### 2. Master Billboard / Secondary Spectator Display
Chihiro Gundam cabinets featured an auxiliary widescreen spectator billboard mounted above or beside the cabinet. This fork brings a dedicated secondary display window replicating authentic arcade behavior:

* **Synchronized Audio & Video**: Streams high-definition 16:9 panoramic video cutscenes with full stereo AAC audio decoded in real time.
* **Authentic Arcade Flow & State Progression**:
  * **Attract Mode (Idle)**: Loops `E02.mp4` (CGI intro & launch animatic) continuously while no one is playing.
  * **Coin / IC Card Insertion**: Instantly cuts to `E03.mp4` (Mission Briefing), flows into `E04.mp4` (Cockpit OS Boot), then `E05.mp4` (Catapult Launch / Sortie).
  * **Active Combat**: Transitions to an animated **tactical spectator radar HUD** (with rotating sweep beam, concentric range rings, and status telemetry).
  * **Dynamic Ace Alerts**: Dynamic encounters (`E06`–`E15`) interrupt with sirens, radar blips, and pilot communications (Char, Amuro, etc.), then automatically return to the tactical radar HUD.
  * **Mission Clear & Game Over**: Plays `E16.mp4` (Evacuation / Clear) followed by `E17.mp4` (Score & Rank Assessment), then returns to the Attract mode loop.
* **Built-in UDP Port 30004 Listener**: Background socket listener compatible with `chihiro-billboard.py`, TeknoParrot, or external game scripts for instant real-time event triggers (`E02`..`E17`, `ATTRACT`, `SORTIE`, `STANDBY`).
* **Hotkeys**: Press keys on the billboard window for instant testing:
  * `F1`: Attract (`E02`)
  * `F2`: Coin / Briefing (`E03`)
  * `F3`: OS Boot (`E04`)
  * `F4`: Catapult Sortie (`E05`)
  * `F5`: Ace Pilot Alert (`E07`)
  * `F6`: Mission Clear (`E16`)
  * `F7`: Game Over (`E17`)
  * `F8` / `Space`: Tactical Radar HUD
  * `F11`: Fullscreen toggle

### 3. Light Guns & Pointer Devices
* Raw Input (Windows) and evdev (Linux) supporting up to 32 independent pointing devices.
* Customizable per-player crosshairs.
* Sinden lightgun border support with configurable thickness.
* `F3` shortcut toggles Light Gun Mode (hides desktop cursor and locks out menus during play).

### 4. Steering Wheels & Real Force Feedback
* Native protocol-level emulation for Namco V257 and Sega 838-13683 drive boards.
* True force feedback on DirectInput / SDL haptic wheels; gamepad rumble fallback.
* Auto-centering emulation for titles like *Crazy Taxi*.

### 5. IC Card Readers (Save Your Progress)
* Fully emulated **Tamura (SAXA) HW210** (Ghost Squad, Gundam) and **Sanwa CRP-1231LR-10NAB** (Maximum Tune).
* Insert cards with a single hotkey; player stats, rank, and licenses persist from session to session.

---

## How to Prepare Game Data from MAME CHDs

Chihiro games require **NetBoot `.bin`** memory images, and titles with secondary billboard cutscenes require extracted `.mp4` video files.

### Step 1: Converting Game CHDs to NetBoot `.bin`
To convert a game GD-ROM / DVD CHD (e.g. `gdx-0013.chd`, `gdx-0017.chd`) to a bootable NetBoot `.bin` image:

1. Download **[Chihiro-netboot](https://github.com/Tovarichtch/chihiro-netboot)**.
2. Run the tool to unpack and decrypt the partition:
   ```bash
   python chihiro-netboot.py --extract <path_to_game.chd> -o <output_folder>
   ```
   *(Or launch the GUI to select your CHD).*
3. The resulting `.bin` file (e.g., `gundamos.bin`, `gundcb79.bin`) can be loaded directly in xemu via **Machine → Load Media Board Image** or via command line:
   ```bash
   xemu.exe -machine xbox,chihiro=on -bios chihiro_xbox_bios.bin -m 128
   ```

### Step 2: Extracting Billboard Cutscenes from DVD CHDs (`CDV-10010`)
*Gundam 0079: Card Builder* cutscenes are stored as dual-stream left/right Sofdec (`.sfd`) video pairs on the `cdv-10010.chd` disc.

To extract and stitch them into modern panoramic `.mp4` files:
1. Ensure **FFmpeg** and **chdman** (from MAME tools) are installed and available in your `PATH`.
2. Use `chihiro-billboard.py`:
   ```bash
   python chihiro-billboard.py --extract path/to/cdv-10010.chd -o movies/
   ```
3. The tool will automatically:
   - Extract `E02L_max.sfd` + `E02R_max.sfd` through `E17L_max.sfd` + `E17R_max.sfd`.
   - Stitch the Left and Right halves together using FFmpeg `hstack`.
   - Encode standard widescreen **`E02.mp4` through `E17.mp4`** files with high-quality AAC stereo audio.
4. Store the generated `.mp4` files in any folder of your choice:
   - In xemu, open the menu and go to **Chihiro → Billboard** to select your movies folder using the file picker (**Movies Directory**).
   - Alternatively, place them in a `movies/` folder beside `xemu.exe` (detected automatically).
   - Or set `movie_dir = "path/to/movies"` in your `xemu.toml`.

---

## Configuration & Usage

In `xemu.toml` (or via the **Chihiro** settings menu in xemu):

```toml
[chihiro]
system = true

[chihiro.billboard]
enable = true
monitor = 1            # Target monitor index (0 = primary, 1 = secondary)
fullscreen = false     # Start billboard in borderless fullscreen
movie_dir = "movies"   # Directory containing E02.mp4..E17.mp4
```

---

## License

GPL-2.0, matching upstream xemu.

## Acknowledgements

* Upstream [xemu](https://xemu.app) project by Matt Borgerson and contributors.
* Sega Chihiro emulation research by Tovarichtch, Réda Chérif-Touil, and the arcade preservation community.
* Special thanks to the arcade testing community for feedback and validation.

