# RDR2 60fps — PS5 runtime patcher

Unlocks **60 FPS in Red Dead Redemption 2** (PS4, v1.32) on a jailbroken PS5.
It patches the game in memory each time you play: no modded PKG, nothing
written to disk. Close the game and it's back to stock.

Built on [KurohaXR/rdr2-60fps-autopatch](https://github.com/KurohaXR/rdr2-60fps-autopatch),
rewritten with the improvements below.

## What's new

- **More regions:** `CUSA08519` and `CUSA08568` as well as `CUSA03041`
- **Version check:** only patches v01.32
- **Bug fix:** an already-patched game is detected instead of reported as "not found"
- **On-screen notifications** for every step and failure, even without ps5debug-NG
- **Retries:** waits for ps5debug-NG, retries while the game loads, retries the
  write, and reconnects if the connection drops
- **Log file**, cleared on every launch: `/data/rdr2-60fps/rdr2-60fps.log`
- **Safety:** warns if the game was already open, and blocks a second copy
- **Docker build** for macOS, Linux and Windows, and **32 automated tests**

## Requirements

- A jailbroken PS5 with an ELF loader
- [ps5debug-NG](https://github.com/OpenSourcereR-dev/ps5debug-NG) v1.3.0+ (FW 13.60 supported)
- RDR2 `CUSA08519` / `CUSA03041` / `CUSA08568`, **clean v1.32** update

## Usage

1. Load `ps5debug-NG.elf`, then `rdr2-60fps.elf`.
2. When *"ready. Start Red Dead Redemption 2"* appears, start the game.
3. *"60fps ON"* appears within seconds. Play.

Load the patcher **before** starting the game. If you don't start the game
within 10 minutes, the patcher exits and you need to load it again. Don't run
`klogsrv` alongside ps5debug-NG (both use port 3232).

If a check fails, the notification ends with *"Nothing changed"*: the game was
left untouched. The log file says why.

## Safety

The patcher writes 3 bytes (`0f 44 f0` → `31 f6 90`) into the RDR2 process
only. It writes only after the title ID, the version and 61 surrounding bytes
all match, then reads the bytes back. It patches no kernel code, touches no
other process, and changes no file except its log. It exits when done.

## Build and test

```bash
./build.sh                  # → src/rdr2-60fps.elf (needs Docker)
python3 test/run_tests.py   # logic tests against a fake ps5debug-NG
```

On Linux you can also build without Docker: install clang-18, lld-18 and
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) v0.43, set
`PS5_PAYLOAD_SDK`, then run `make -C src`.

## Credits

- **[KurohaXR](https://github.com/KurohaXR)**: the original tool, the crash
  research and the runtime-patch approach
- **Jao**: the 1.32 *Unlock FPS* patch
- **illusion**: [PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)
- **OpenSourcereR**: [ps5debug-NG](https://github.com/OpenSourcereR-dev/ps5debug-NG)
- **John Törnblom**: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and elfldr

## License

MIT (see [LICENSE](LICENSE)), derived from KurohaXR's MIT code. The payload
links against the GPL-3.0 ps5-payload-sdk, so share the source along with any
compiled `.elf`.

Experimental software, use it at your own risk. Not affiliated with Rockstar
Games or Sony.
