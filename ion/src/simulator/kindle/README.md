# Upsilon for Kindle (kindlehf)

A port of Upsilon to Kindle e-readers running the hard-float ("kindlehf")
firmware, i.e. **firmware 5.16.3 and later**. It was written for the
**Paperwhite 4** (PW4, 10th gen) and should also work on the other i.MX-based
models that get that firmware (Kindle 10th gen, Oasis 2/3).
MediaTek-based models (Paperwhite 5, Kindle 11th gen, Scribe...) use a different
e-ink interface and are not supported yet.

The whole calculator runs: Calculation, Functions, RPN, Python, Statistics,
Probability, Solver, Atomic, Sequences, Regression, Reader, Settings.

## Installing

You need a jailbroken Kindle. Unzip `upsilon.zip` at the root of the Kindle's
storage (over USB). It contains:

| Folder | What it is |
| --- | --- |
| `upsilon/` | The calculator itself, its settings (`upsilon.conf`), saved state and log. **Always needed.** |
| `documents/Upsilon.sh` | A [Scriptlet](https://kindlemodding.org/kindle-dev/scriptlets.html): Upsilon appears in the Kindle library, tap it to start. Needs the Universal Hotfix (installed with WinterBreak and later jailbreaks). |
| `koreader/plugins/upsilon.koplugin/` | [KOReader](https://koreader.rocks) plugin: *Tools → More tools → Upsilon calculator*. It can also be bound to a gesture. |
| `extensions/upsilon/` | [KUAL](https://www.mobileread.com/forums/showthread.php?t=203326) menu entry. |

Keep `upsilon/` and whichever launchers you use; the others are harmless.

To quit, **hold the "exit" key** (top left of the keypad) for about half a
second. You get back to wherever you started from: the home screen, the
library, or KOReader.

## Using it

* The screen is split between the calculator display (zoomed 3x on a PW4) and
  a touch keypad laid out like the real NumWorks keyboard. Small labels show
  the shift (top left) and alpha (top right) function of each key.
* Keys react on touch down, like physical keys. Holding an arrow or **del**
  auto-repeats.
* E-ink keeps faint traces of previous images. A full flashing refresh happens
  regularly and when the whole screen changes; tap **clean screen** to force one.
* Everything in the calculator's memory (Python scripts, functions,
  sequences, variables) and your settings are saved to
  `upsilon/upsilon.state` every few seconds and on exit, whichever launcher
  you use.
  After updating Upsilon, scripts, functions and sequences are kept; variables
  and settings saved by the previous version are reset.
* `upsilon/upsilon.log` has diagnostic output.

Options live in `upsilon/upsilon.conf` (zoom level, refresh
frequency, contrast, touch orientation fixes, startup language).

When started from the library or KUAL, the Kindle interface is paused while
Upsilon runs. From KOReader, KOReader hands over the screen and touch panel
and takes them back afterwards. If you press the power
button the Kindle goes to sleep as usual; after waking it up, tap
**clean screen** to repaint the calculator.

## Building

```
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf zip \
  libfreetype6-dev libpng-dev libjpeg-dev pkg-config imagemagick
pip install lz4 pypng stringcase
make PLATFORM=simulator TARGET=kindle -j4 upsilon.zip
# -> output/release/simulator/kindle/upsilon.zip (layout above)
```

The binary is fully static, so it doesn't depend on the firmware's glibc. You
can also use the koxtoolchain `kindlehf` toolchain:
`make ... KINDLE_CROSS=arm-kindlehf-linux-gnueabihf-`.

### Testing without a Kindle

The binary can render into memory and read taps from a script, which works
under `qemu-arm`:

```
cat > script.txt <<EOF
wait 1000
key ok
key 1
key +
key 2
key EXE
wait 1000
shot result.pgm
hold exit 800
EOF
UPSILON_FAKE_FB=1072x1448 UPSILON_FAKE_INPUT=script.txt \
  qemu-arm output/release/simulator/kindle/epsilon.bin --state /tmp/test.state
```

Commands: `key NAME`, `hold NAME MS`, `tap X Y`, `wait MS`, `shot FILE.pgm`,
`quit` (see `input.cpp`).

## How it works

| File | Role |
| --- | --- |
| `eink.cpp` | Maps `/dev/fb0`, converts an 8-bit gray canvas to the fb format, sends `MXCFB_SEND_UPDATE` (Rex, Zelda or legacy layout, probed at first update) |
| `screen.cpp` | Ion's 320×240 framebuffer; on each keyboard poll, converts to gray, diffs with what's on the panel and sends one partial update for the changed area |
| `touch.cpp` | Finds the touchscreen in `/dev/input`, decodes multi-touch protocol A/B, scales to the fb |
| `keypad.cpp` | Keypad layout and drawing (with Kandinsky's fonts), hit testing, pressed-key feedback |
| `input.cpp` | Turns taps into `Ion::Keyboard::State`s so that short taps and taps during busy periods are not lost, and direct keyboard polls (Python interrupt, `ion.keydown`) behave like on the hardware |
| `persistence.cpp`, `preferences.cpp` | State file |
| `package/` | Launchers: `upsilon.sh` (pauses the Kindle UI (`cvm`), status bar and screensaver, then restores them; `--scriptlet` and `--koreader` modes), the Scriptlet, the KOReader plugin and the KUAL entry |
