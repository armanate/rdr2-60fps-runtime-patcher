# RDR2 60fps — PS5 runtime patcher

Unlocks **60 FPS in Red Dead Redemption 2** (PS4, v1.32) on a jailbroken PS5,
and on a 1080p TV renders the game's own **PS4 Pro 4K checkerboard**, shown at
1080p (supersampled). It patches the game in memory each time you play: no
modded PKG, nothing written to the game. Close the game and it's back to stock.

Built on [KurohaXR/rdr2-60fps-autopatch](https://github.com/KurohaXR/rdr2-60fps-autopatch),
rewritten with the improvements below.

## What's new

- **One payload:** no ps5debug-NG needed. It reads game memory with the
  console debug calls, and writes the patch either that way or, on firmware
  that refuses it (>= 8.40, e.g. 13.60), through the kernel direct map, using
  code ported from ps5debug-NG
- **More regions:** `CUSA08519` and `CUSA08568` as well as `CUSA03041`
- **Version check:** only patches v01.32
- **Bug fix:** an already-patched game is detected instead of reported as "not found"
- **On-screen notifications** for every step and failure
- **Retries:** retries while the game loads and retries the write
- **Log file**, cleared on every launch: `/data/rdr2-60fps/rdr2-60fps.log`
- **Safety:** warns if the game was already open, and blocks a second copy
- **4K checkerboard on 1080p TVs (v2.0):** see [Resolution](#resolution)
- **Docker build** for macOS, Linux and Windows, and automated tests

## Requirements

- A jailbroken PS5 with an ELF loader (elfldr)
- RDR2 `CUSA08519` / `CUSA03041` / `CUSA08568`, **clean v1.32** update

## Usage

1. Load `RedDead2 60FPS_v2.0.elf`.
2. When *"ready. Start Red Dead Redemption 2"* appears, start the game.
3. *"60FPS IS ON, Enjoy."* appears within seconds, with the resolution the
   game picked, e.g. *"Resolution: 1920x2160 checkerboard, 3840x2160 frame"*.
   Play.

Load the patcher **before** starting the game. If you don't start the game
within 10 minutes, the patcher exits and you need to load it again.

If a check fails, the notification ends with *"Nothing changed"*: the game was
left untouched. The log file says why.

## Resolution

The first run creates `/data/rdr2-60fps/scale.txt`:

```
fake4k=1
2160p=0
```

- **`fake4k=1`**: on a 1080p TV, the game believes the output is 4K and takes
  its own PS4 Pro 4K checkerboard path (1920×2160 rebuilt to 3840×2160). The
  PS5 scales those frames to 1080p. Measured on a PS5, the renderer's
  constants moved from 1920×1080 / 960×540 to 1920×2160 / 960×1080.
  On a 4K TV nothing changes (that's already its mode). `fake4k=0` turns it
  off.
- **`2160p=`**: a lighter render on top of 4K checkerboard, if 60fps dips:
  `3` = 3/4 (2880×1620), `2` = 2/3 (2560×1440), `1` = 1/2. `0` = stock. Ratios
  above 1 aren't accepted: they corrupt the image.

This part has to land before the game applies its settings, about a second
after it starts, so load the payload **before** starting RDR2. If the game is
already open, or anything about the resolution patch doesn't check out, the
payload says so and applies the 60fps patch alone. It changes three places,
each verified against the clean 1.32 code first. Two of them jump to new
code it writes in unused space at the end of the game's code:

| Where | Change |
|---|---|
| `0x7865e8` (where the game copies its screen size into its video mode) | a jump to new code: 1920×1080 → 3840×2160, checkerboard on |
| `0xa2512e` (snap to the TV's own modes) | `je` → `jmp`: keep 3840×2160 |
| `0x786846` (copy of the Resolution Scale setting, only with `2160p=` set) | a jump to new code that sets it in the live settings |

4K checkerboard is about twice the GPU work of stock 1080p. If busy areas
like Saint Denis dip, try `2160p=2`.

## Safety

The patcher writes 3 bytes (`0f 44 f0` → `31 f6 90`) into the RDR2 process
only. It writes only after the title ID, the version and 61 surrounding bytes
all match, then reads the bytes back. On firmware that refuses the debug-write
call, it uses the kernel's read/write access to write that one game page
through the direct map (see [src/ptwalk.c](src/ptwalk.c)); it still patches no
kernel code, touches no other process, and changes no file except its log and
`scale.txt`. It exits when done. The resolution patch is described under
[Resolution](#resolution).

## Build and test

```bash
./build.sh                         # → src/*.elf (needs Docker)
python3 test/run_tests.py          # RDR2 logic tests against a fake console
python3 test/run_tests_combined.py # 60fps payload with the resolution patch
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
  — `src/ptwalk.c` is ported from its `proc_ptwalk.c`, and it showed where the
  game version lives and how to reach game memory
- **John Törnblom**: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and elfldr

## License

**GPL-3.0-or-later** (see [LICENSE](LICENSE)). `src/ptwalk.c` is ported from
ps5debug-NG, which is GPL-3.0, so the combined payload is GPL-3.0; the rest is a
rewrite of KurohaXR's MIT code (MIT is GPL-compatible). Share the source with
any compiled `.elf`.

The [LICENSE](LICENSE) file carries the notices but not the full GPL text; add
it once with:

```bash
curl -L https://www.gnu.org/licenses/gpl-3.0.txt >> LICENSE
```

Experimental software, use it at your own risk. Not affiliated with Rockstar
Games or Sony.
