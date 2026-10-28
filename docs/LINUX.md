# Linux

OmniStats runs on Linux through Proton in second-monitor mode. Linux support is experimental, and the in-game overlay doesn't work yet: it shows a black screen instead of a transparent overlay.

Tested on Bazzite 44 (KDE Wayland) with Proton-CachyOS 11.0, DXVK 3.1.1, and Rocket League through Heroic.

When the runtime does not implement composition swap chains, OmniStats uses an opaque window for setup and the dashboard.

## Install

Install OmniStats in its own Wine prefix rather than Rocket League's. Adjust `PROTON` and `DXVK` if your versions differ.

```bash
PROTON="$HOME/.var/app/com.heroicgameslauncher.hgl/config/heroic/tools/proton/Proton-CachyOS-latest"
DXVK="$HOME/.var/app/com.heroicgameslauncher.hgl/config/heroic/tools/dxvk/dxvk-3.1.1/x64"
export WINEPREFIX="$HOME/Games/OmniStats/prefix"
export WINEDLLOVERRIDES='d3d11,dxgi=n'
mkdir -p "$HOME/Games/OmniStats"
curl -fL https://omnistats.org/OmniStatsInstaller.msi -o "$HOME/Games/OmniStats/OmniStatsInstaller.msi"
MSI=$("$PROTON/files/bin/wine" winepath -w "$HOME/Games/OmniStats/OmniStatsInstaller.msi")
"$PROTON/files/bin/wine" msiexec /i "$MSI" /qn
cp "$DXVK/d3d11.dll" "$DXVK/dxgi.dll" "$PROTON/files/lib/vkd3d/x86_64-windows/"*.dll \
  "$WINEPREFIX/drive_c/windows/system32/"
```

## Enable the Stats API

Close Rocket League. In `TAStatsAPI.ini` (or `DefaultStatsAPI.ini` if that file doesn't exist), set:

```ini
[TAGame.MatchStatsExporter_TA]
PacketSendRate=30
Port=49123
```

With Heroic, `TAStatsAPI.ini` is inside the Rocket League prefix under `Documents/My Games/Rocket League/TAGame/Config`.

## Launch

```bash
PROTON="$HOME/.var/app/com.heroicgameslauncher.hgl/config/heroic/tools/proton/Proton-CachyOS-latest"
export WINEPREFIX="$HOME/Games/OmniStats/prefix"
export WINEDLLOVERRIDES='d3d11,dxgi=n'
"$PROTON/files/bin/wine" 'C:\users\steamuser\AppData\Local\OmniStats\OmniStats.exe'
```

During setup, choose **Second-monitor dashboard**. Then open Settings > General and turn off **Only show while Rocket League is active**. OmniStats can't see the game window from a separate prefix, so it would otherwise stay hidden.

If OmniStats can't find your Stats API file, set `rocket_league_stats_api_config_path` in `~/Games/OmniStats/prefix/drive_c/users/steamuser/AppData/Roaming/omnistats/config.json`. Use a `Z:` path, for example `Z:\\home\\you\\Games\\Heroic\\RocketLeague\\TAGame\\Config\\DefaultStatsAPI.ini`.

## Troubleshooting

- **OmniStats closes right away:** check `drive_c/users/<wine-user>/AppData/Roaming/omnistats/omnistats_log.txt` inside the prefix used to launch it. A `[D3D11] Failed to create composition swap chain: 0x80004001` error identifies a build without opaque-window presentation support. Use a build containing that fix. If no new application log appears, capture Wine's terminal output and check the executable path, prefix, and DXVK DLL overrides.
- **Black screen:** you're in overlay mode. Switch back to the second-monitor dashboard.
- **No game connection:** check that `Port=49123` and `PacketSendRate=30` are set, then restart Rocket League.

Rank lookups, shortcuts, Discord, replay uploads, and auto-updates haven't been tested on Linux.
