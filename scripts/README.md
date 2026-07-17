# Scripts

Double-click each `.command`, or run it from a terminal. Each one is independent
and safe to re-run; run all three in order for a game that is downloaded,
installed, and patched in one go. Everything lands under `dist/`.

| Script | Does | Needs |
|---|---|---|
| `download.command` | Fetches the game archive to `dist/Deadpool.zip` | `curl` |
| `install.command` | Unzips it to `dist/Deadpool/` | `unzip`, the archive |
| `gen-fix.command` | Builds the proxy from `src/` and installs it into `dist/Deadpool/Binaries/` | `python3`, `mingw-w64` |

Run in that order: `download` → `install` → `gen-fix`. The paths line up, so
`gen-fix` drops the built `fmodex.dll` straight into the game `install` produced.

`gen-fix` also handles the swap the fix needs: the first time it runs it moves the
game's stock `fmodex.dll` aside to `fmodex_og.dll` (what the proxy forwards to),
then writes the proxy as `fmodex.dll`. Re-running only rebuilds the proxy and
leaves the stock DLL alone. It works even before the game is installed, creating
the `Binaries/` path so the build has somewhere to go.
