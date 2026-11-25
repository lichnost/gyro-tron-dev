# Gyro-Tron OS, packages and the manager app

**Status:** reference — as-built, with the design history kept where it explains a decision.
Shipped: the container (§2), the loader (§6), the USB link (§7) and the manager app (§9).
Not yet: uninstalling on the watch (§10), the repartition of §5. The authorities for the
contracts described here are the code:
`components/tat_api/include/tat/tat_api.h` (the game ABI, API 1.8),
`components/link/include/link/link.h` (the link protocol), `components/loader/loader.c`
(the container and the loading). Where this document and the code disagree, the code wins.
Checked against the code 2026-11-04.

The console splits into two things that are built, shipped and versioned separately.

**The OS** is the firmware: drivers, engine, canvas, launcher, settings, Wi-Fi, updates, the
USB link and the game loader. It lives in the two OTA slots and rolls back if a build won't
boot. It is the only thing that has to be protected, because it is the only thing that can't
be reinstalled from a file.

**Packages** are single files you put on the watch and take off again. One kind exists today
— games — and the container has room for more, so there is one thing to build, one thing to
send, and one thing to share. A package is self-contained: it can be emailed, posted or
handed to a friend, and it works.

Everything below is the contract between them.

---

## 1. What ships in the OS

Built in, always present:

- **Pocket Watch** — it's a watch first; telling the time can't depend on an installed file,
  and it needs the wall clock, the time zone and the network.

**No game is compiled into the firmware.** Every game is a package. The firmware build packs
the games in `games/` into `.tat` files and carries a factory copy of each
(`components/factory`), which it installs to storage on first start - or whenever it finds
the games folder empty. A factory game uninstalls like any other and can be put back.
The launcher builds the home screen by scanning the games folder on storage
(`components/loader/loader.c`), not from a registry.

---

## 2. The package — one file, like an APK

`.tat` for games. Little-endian throughout.

```
offset  size  field
0       8     magic        "TATPKG\0" + format version byte (currently 1)
8       4     header_crc32 CRC-32 of bytes 12..end of the section table
12      4     total_size   whole file, bytes
16      1     kind         1 GAME; values 2+ are unassigned
17      1     reserved
18      2     api_major    the game API the package was built against
20      2     api_minor
22      2     version      the package's own version, for "you have v1, this is v2"
24      16    id           "skyjump" — lowercase [a-z0-9_]; a game's save namespace
40      24    name         "SKY JUMP" — what the launcher and the app show
64      24    author       shown in the app; packages get shared, so credit travels with them
88      3     accent       red, green, blue - plain bytes
91      1     reserved
92      2     section_count
94      2     reserved
96      ...   sections[], 16 bytes each: type u32, offset u32, size u32, crc32 u32
```

**Every game has an `ICON` section.** `tools/mktat.py` will not pack a game without an
`icon.png` beside its `game.json`. It was optional once, and the result was a home screen
and a library full of identical blank cartridges. The icon is a PNG, 210 x 210 drawn in a
circle (232 x 232 is the most the launcher will decode), and at most 4096 bytes - the watch
sends it to the manager app in a single link frame. `tools/make_icons.py` draws the
firmware's own on a 52 x 52 grid scaled up 4x, which is a good way to stay under the limit.

**No game is part of the firmware.** The ones a new watch comes with live in `games/<id>/`
like anyone else's, are built by the same `tools/mktat.py`, and are installed and removed
the same way. The firmware build packs them (`components/factory`) and carries a factory
copy of each, which it puts in place the first time it starts, or when it finds the games
folder empty - storage can be wiped, and a watch with nothing on it is a poor thing to
hand someone. A game its owner removed stays removed. Only the clock and Settings are
compiled in: the clock is the watch itself, and needs the real-time clock, time zones and
network time, none of which a game is given.

One exception to the "a game directory holds source" rule: a **GB ROM-cartridge descriptor**
(`games/gb_*`, `"type": "gb"` in `game.json`) carries no code at all — its `CODE` section is
the template game's (the emulator in `kits/gbboy/`) and its `ASST` is the cartridge. `tools/mktat.py` recognises the
type and routes to `kits/gbboy/tools/gbmake.py`, which fetches the ROM (cached in `build/roms/`,
gitignored — a ROM we have no right to publish never lands in the repository). The package
that comes out is an ordinary `.tat` in every other respect. Descriptor format:
`games/gbboy/README.md`.

| Section | Used by | Meaning |
|---|---|---|
| `CODE` | GAME | The executable image. Format decided by §6. |
| `ICON` | GAME | PNG with alpha, the carousel icon. 210×210 is what it is drawn at; the loader decodes at most 232×232, and the limit is 4096 bytes. |
| `ASST` | GAME | `name[16]` then bytes. Sprite sheets, backgrounds, ROM dumps (`games/gbboy/README.md`). |
| `SIG ` | GAME | Optional, always last: 1 key-id byte + a 64-byte Ed25519 signature over everything before it. See below. |

Any other section type is skipped by the loader (the header CRC still covers it), so a
future section type can arrive without breaking an installed game.

**The `SIG ` section.** Signed means the official channel of the game catalog: built and
signed by the console developers (`tools/mktat.py --sign <keyfile>`, or the catalog
pipeline signing on deploy - `docs/reference/CATALOG.md`). The signature covers the byte
range from offset 0 to the start of the section itself, so it authenticates the header,
the table and every other section; the table entry's own crc32 is 0 by format rule (a CRC
over the signature bytes would be circular), and every CRC pass skips the section for
that reason. Community packages ship unsigned on purpose - their risk is bounded by the
loader sandbox, not by a signature, and an unsigned package loads exactly as it always
has. Verification runs at install time - the manager checks before sending
(`TatPackage.cs`), the watch's store checks what it downloads
(`loader_verify_signature` in `components/loader/sig.c`) - and never at launch, so boot
stays as fast as it is today. The trusted public keys sit in `components/loader/keys.c`,
`backend/tools/tatpkg.py` and `TatPackage.cs`; `tests/test_tat_format.py` fails if the three
copies drift apart.

**Where the checks run.** Sending a `.tat` is an ordinary `FS_PUT` (§7): the link writes
the file without looking inside it. The package checks run before sending - the manager
checks the header CRC, the section CRCs and the API version client-side
(`app/GyroTron.Manager/Link/TatPackage.cs`) - and again on the watch at load time
(`components/loader/loader.c` re-checks the whole package before relocating anything). A
truncated file therefore *installs*, and fails honestly, with a message, when you tap its
icon.

**Kinds.** Only `KIND_GAME` (1) exists: the manager refuses anything else, the loader only
loads kind GAME, and the packer only writes it. Themes ship as plain folders sent over the
link, not as packages; the kind byte has room for more kinds if a second one ever earns its
keep.

---

## 4. The game ABI

C, not C++. Fixed-layout POD structs, explicit widths, no inline functions, no vtables across
the boundary. This is what lets a game built today keep running on next year's OS.

### 4.1 What a game exports

```c
typedef struct {
    uint32_t    magic;          /* 'TATG' */
    uint16_t    api_major, api_minor;

    const char *id;             /* "pindrop" - lowercase; also the save namespace */
    const char *name;           /* "PIN DROP" - what the carousel shows */
    uint8_t     accent_r, accent_g, accent_b;  /* the launcher's colour, plain bytes */
    const tat_asset_t *assets;  /* may be NULL */
    int         asset_count;

    void (*begin)(const tat_api_t *api);  /* once, after loading. Store `api`. */
    void (*enter)(void);                  /* becoming the active app; screen is black */
    void (*update)(float dt);             /* once per frame */
    void (*draw)(void);                   /* once per frame, after update */
    void (*leave)(void);                  /* another app took over, or the watch is sleeping */
    void (*unload)(void);                 /* being removed from memory; release anything */
    void (*redraw)(void);                 /* optional: the console needs a frame anyway */
    bool (*keep_awake)(void);             /* optional, may be NULL */
    int  (*state_save)(void *buf, size_t max);      /* optional, API 1.4: see sleep snapshots */
    bool (*state_load)(const void *buf, size_t len); /* optional, API 1.4 */
} tat_game_t;

extern const tat_game_t tat_game;
/* A game with files of its own also declares this and points the descriptor at it. */
extern const tat_asset_t tat_assets[];
```

`begin` is the only place the game receives the API table. It stores the pointer; the OS
guarantees it stays valid until `unload`.

### 4.2 What the OS provides

**The header is real now:** `components/tat_api/include/tat/tat_api.h`, with the console's
side in `tat_host.cpp`. PIN DROP and SKY JUMP are written against it and nothing else.
Every change below was found by writing one of them, not by design review — which is the
argument for porting real games early rather than freezing the API on paper.

From PIN DROP (API 1.0):

- **`rgb(r, g, b)` is an API call, not a macro.** The panel stores RGB565 byte-swapped, and
  a macro baked that into the game - every colour came out wrong. How a pixel is packed is
  the console's business, so a game asks for a colour rather than building one.
- **The pause menu is exposed whole** (`menu_open`, `menu_update`, `menu_draw`, ...) rather
  than as row and button primitives. Primitives would have every game reinventing the pause
  screen, which is what was just removed from every one of them.
- **`redraw` was added to the game descriptor**, for games that only draw when something
  changed. Without it a screenshot over Wi-Fi catches a black frame.

From SKY JUMP (API 1.1):

- **`canvas_banner()` was added.** The panel a game drops over the play area to say GAME
  OVER lived inside the games component, out of a package's reach, so every packaged game
  would have rolled its own. It now sits beside the pause menu in the console, for the same
  reason the pause menu does.
- **A descriptor's accent colour is three plain bytes**, not a `tat_color_t`. A descriptor
  exists before the game has an api pointer to call `rgb()` with, so the old field asked
  games to hand-pack the panel's byte order — the one thing `rgb()` exists to prevent.
- **`tat_asset_t` holds an end pointer, not a length.** The difference of two linker symbols
  is not a constant expression in C, so with a length a built-in game could not write its
  own asset table down. (The C++ version got away with it; C does not.)
- **Assets come from a table the build supplies**, declared as `extern const tat_asset_t
  tat_assets[]`. Linked into the firmware for a built-in game, supplied by the loader for an
  installed one; the game asks `api->asset()` by name and never learns which.

From ECHO (API 1.4) — sleep snapshots:

- **`state_save` / `state_load` were added to the game descriptor.** Deep sleep keeps no
  memory, so before it the console asks the running game to write its logical state — a
  level, a score, a board, up to 32 KB — into a buffer, and hands those bytes back after
  `begin()` when the watch comes on. Keep pointers and anything derived out of it: rebuild
  those in `begin()`. Version the blob inside the game, and return `false` from
  `state_load` rather than guess at a layout that has changed. A game that keeps nothing
  leaves both NULL and simply starts fresh after deep sleep.
- **A snapshot only ever wakes the build that wrote it.** The file on storage carries the
  firmware's ELF hash and the game package's fingerprint; a firmware update or a re-sent
  `.tat` invalidates it, and the wake lands on the home screen instead of into stale state.

From the SENSITIVITY setting (API 1.5) — `float tilt_sensitivity(void)`:

- **The console hands the factor over; the game applies it.** Settings has a SENSITIVITY
  row (X1, X1.25 ... X2) for players who have to tip the watch too far. The console does
  not scale the tilt it delivers — a game's input may be an angle, a threshold or raw gravity,
  and each of those wants the gain in a different place. Instead divide by this
  whatever you treat as "full tilt": a threshold (`0.24f / T->tilt_sensitivity()`) is
  reached sooner, a range of turn (`FULL_LOCK / T->tilt_sensitivity()`) shrinks, and raw
  gravity (`GRAVITY * T->tilt_sensitivity()`) pulls harder. A game that ignores it simply
  keeps its old feel at every level, which is right for direction-only play where turning
  further is the whole point.

Streamed PCM (API 1.6) — `audio_start` / `audio_push` / `audio_space` / `audio_stop`:

- **`tone()` is for one-shot effects; a synth of your own streams.** An emulator's APU or a
  tracker renders mono int16 samples and pushes them, and the mixer blends the stream with
  tones under the console's volume setting. `start()` returns the rate the console plays at —
  pass 0 to generate at that same rate, which is the one rate nothing is resampled at — and 0
  when there is no audio path, in which case stay silent. One stream per game; `start()` again
  drops what was queued.
- **The buffer is small, so treat it as a mailbox.** About a tenth of a second sits between
  the game and the speaker. A game running ahead gets 0 back from `push()` and drops its render
  rather than blocking — ask `space()` first to skip the work of rendering samples nobody will
  hear (an emulator runs a frame with only its frame sequencer going when the queue is nearly
  full, and the music keeps its place). `stop()` is how a pause menu goes silent at once instead of
  playing out the tail.

The header is the truth for signatures; what follows is the map of it. Every change is
appended to the end of the struct, so a member's "since" is also where it sits.

| Group | Members | Since |
|---|---|---|
| console | `rgb` (always: a game never packs a colour by hand), `log`, `now_us`, `random` | 1.0 |
| input | `input()` - buttons (BOOT never reaches a game held), touch, tilt in screen-aligned g and deg/s - and `gestures()` (`tap`, `swipe_left`, `swipe_right`) | 1.0 |
| frame rotation | tilt also carries rx/ry/rz (deg) + rdt (s) - the gyro integrated over the whole frame at the IMU's own rate, corrected like gx. An orientation filter must turn by this, not by gx*dt: the wrist speeds up and slows inside a frame, one instantaneous sample stretched over dt misses most of a sweep, and heading has no reference to correct the miss back (paid for on hardware) | 1.8 |
| memory | `alloc`, `free` - everything a game takes is given back at `unload`, so a game that forgets cannot leak across launches | 1.0 |
| canvas | `canvas_create` (scale 2 -> 233x233, scale 3 -> 155x155), `canvas_destroy`, `canvas_width`, `canvas_pixels` | 1.0 |
| palette | `canvas_color`, `canvas_reserve`, `canvas_set_color`; `canvas_set_palette` installs a whole palette so a game owns its indices (call it before `sheet_load`) | 1.0 / 1.3 |
| drawing | `canvas_clear`, `canvas_pixel`, `canvas_fill_rect`, `canvas_rect`, `canvas_line`, `canvas_fill_circle`, `canvas_text`, `canvas_text_centered` | 1.0 |
| sprites | `sheet_load` (PNG, frames side by side), `canvas_sprite`, `canvas_sprite_scaled`; `sheet_pixels` + `sheet_info` hand the sheet's own indices over for games that blit (e.g. repaint one car into several liveries) | 1.0 / 1.2 |
| present | `canvas_present`, `canvas_present_rotated` (keeps the world upright while the watch turns like a wheel) | 1.0 |
| pause menu | `menu_open`, `menu_close`, `menu_is_open`, `menu_invalidate`, `menu_update`, `menu_draw`, `menu_sound_row`, `menu_toggle_sound`, `ui_color` | 1.0 |
| banner | `canvas_banner` - the shared LEVEL 3 / GAME OVER panel over the play area | 1.1 |
| polar | `polar_angles`, `polar_radii` - per-screen-pixel angle/radius tables (~870 KB; NULL if they could not be built, and the game must cope) | 1.2 |
| text metrics | `text_width` | 1.2 |
| sensitivity | `tilt_sensitivity` - the Settings factor; divide whatever you treat as "full tilt" by it | 1.5 |
| sound | `tone` (one-shot effects), `volume` (0..3, the console setting) | 1.0 |
| streamed PCM | `audio_start` / `audio_push` / `audio_space` / `audio_stop` - one stream, a ~0.1 s mailbox; a game running ahead gets 0 back and drops its render | 1.6 |
| saves | `save_get(key, &value, limit)` / `save_set(key, value)` - ints, in this game's own namespace, erased with the game; `get` leaves the value alone when nothing is stored | 1.0 |
| assets | `asset(name, &len)` - the package's `ASST` sections, in place; the game never learns whether it came from the firmware image or the `.tat`. Watch it: the console materializes the whole file in PSRAM for that pointer | 1.0 |
| asset streaming | `asset_size(name)` / `asset_read(name, offset, dst, n)` - ranges read straight from the package, nothing held beyond the bytes the game keeps. For a game that carries one big bundle, materializing it as a pointer can cost half the free PSRAM; reads past the end are clamped, `size` is 0 for an unknown name | 1.9 |

Notes that matter:

- **Tilt aim has two pinned signs - do not re-derive them.** The pipeline delivers honest
  physics (gravity in screen axes, the gyro chain over `rx/ry/rz`); what a game adds is
  convention, and two signs are hardware facts pinned by `games/aimdiag/`, golden shots
  and kit SPEC §7.1 (full story there): an orientation filter's heading *falls* on a
  rightward turn, so the view azimuth is `wrap360(psi_offset - psi)`; and the aim pitch
  is the sight line THROUGH the device - the way the glass faces - so
  `theta = asinf(-vy)`: top edge away = glass to the sky = theta positive, top edge
  toward you = glass to the ground = theta negative. An `asinf(+vy)` variant described
  as the normal's opposite inverts the vertical on the watch; the golden shot
  (`games/duckhunt/tests/golden/aimdown.png` - script pitch + shows the ground) fails
  the build-style regression run if the sign flips.
- **Saves are ints, not buffers.** A game's settings, scores and (via a little packing) its
  states are 32-bit values under string keys. Anything bigger travels by sleep snapshots
  (§4.1's `state_save`) or the game's own asset tables.
- **The shared look is the whole pause menu and the banner.** There is no `ui_row`/`ui_button`
  layout kit: games that need their own screens build them from canvas calls, and the console's
  menus stay consistent because they are the console's, not the games'.
- **No file system.** Games can't read storage. Saves go through `save_*`, content comes from
  the package. That is what makes uninstall complete, and what makes a shared game safe to
  run.

### 4.3 Porting a game to the API

What the first two conversions came down to, so the rest come out the same shape. A ported
game is one `.c` file plus, if it ships files, a `_builtin.c` naming them.

| C++ it used | What it becomes |
|---|---|
| `wc::Canvas canvas; canvas.init(s)` | `T->canvas_create(s, 0)`, kept in the game's own struct |
| `c.fillCircle(...)`, `c.textCentered(...)` | the matching `T->canvas_*` call, canvas first |
| `Gfx::W`, a hardcoded canvas width | `T->canvas_width(cv)` — ask, don't assume |
| `wc::Store s("id"); s.get(k, v)` | `T->save_get(k, &v, limit)`; the namespace is the descriptor's `id` |
| `wc::audio::play({...})` | `tat_tone_t` + `T->tone(&t)`, one call per tone |
| `console::ui::PauseMenu menu` | `T->menu_open/is_open/update/draw/...` |
| `games::ui::banner(...)` | `tat_banner_t` + `T->canvas_banner(...)` |
| `Gestures ges; ges.update(...)` | `T->gestures()` — the host already did it |
| `esp_random()`, `esp_timer_get_time()` | `T->random()`, `T->now_us()` |
| `ESP_LOGI(TAG, ...)` | `T->log(...)` — the tag is the game's `id` |
| `e.presenter()`, `e.input()` | `T->canvas_present(cv)`, `T->input()` — a game exits through the console's pause menu, not by going home itself |
| `_binary_x_png_start` in the game | a `tat_assets[]` table in `_builtin.c`; the game calls `T->asset("x.png", &len)` |
| `std::vector<Thing> things` | `Thing things[MAX]; int n_things;` and swap-with-last removal |
| a big array as a plain member | `T->alloc()` in `begin`, `T->free()` in `unload` |

Four things that bite every time:

- **Large state is not free because it is static.** A track of 20 KB of
  plain members lands in internal RAM — the scarce kind, and the kind Wi-Fi needs.
  The free internal heap went from 48 KB to 23 KB and the watch stopped answering. Anything
  measured in kilobytes belongs in `alloc()`, which is what a package would have to use.

- **Removing from a fixed array.** `a[i] = a[--n]` moves the last element into the hole, so
  the loop must **not** advance `i` afterwards — or must `break` immediately. Both ports had
  one of each.
- **Filling up.** Decide what happens when the array is full *before* it is, and prefer
  stopping cleanly to dropping the newest thing on the floor.
- **Anything `const` and file-scope must be a constant expression in C.** C++ will fold
  things C will not — the difference of two linker symbols being the one that caught us.
- **Check the old save namespace before choosing an `id`.** The descriptor's `id` *is* the
  save namespace. Sleepy Star's was `sleepystar`, not `star`; calling it the obvious thing
  would have put every player quietly back on level 1.

Build it in by adding the source to `components/games/CMakeLists.txt` and renaming the
symbols a package would export, so several games can share one firmware:

```cmake
set_source_files_properties("mygame/mygame.c" "mygame/mygame_builtin.c"
    PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_mygame;tat_assets=tat_assets_mygame")
```

then declare `extern "C" const tat_game_t tat_game_mygame;` in `main/main.cpp` and register
`static tat::HostedGame mygame(tat_game_mygame);` in the carousel.

### 4.4 Versioning

`major` changes when something is removed or changes meaning. `minor` changes when functions
are **appended to the end** of the struct and nowhere else.

The one exception so far: API 1.7 removed the never-called `go_home` while the console was
still pre-production, shifting every member after it — a one-time break, and every package
was rebuilt against the new layout. The rule holds from there on.

The OS runs a game when `game.api_major == os.api_major && game.api_minor <= os.api_minor`.
Otherwise the launcher still shows the icon, but tapping it opens a full-screen page -
"NEEDS A NEWER GYRO-TRON" (or "BUILT FOR AN OLDER GYRO-TRON"), with TAP TO GO BACK
(`components/loader/packaged_game.cpp`) - and nothing is loaded. Uninstalling an
unsatisfiable game is the manager app's job (§9; hold-to-uninstall on the watch - §10, planned).

---

## 5. Flash layout

The map in `partitions.csv` (32 MB flash):

| Partition | Size | Holds |
|---|---|---|
| `nvs` | 24 K | settings, saves, the calibration record |
| `otadata` | 8 K | which OS slot to boot |
| `phy_init` | 4 K | RF calibration data |
| `app0` / `app1` | 4 MB each | the OS, A/B for rollback |
| `storage` | 16 MB | FAT mounted at `/data`: game packages, themes. Never exposed over USB (§7) |
| `coredump` | 64 K | crash dumps |

There is no games partition: installed games are `.tat` files in the FAT's games folder.
The two 4 MB app slots are sized for the OS image alone - which is that big mostly because
it embeds a factory copy of every game. An OS without the embedded games would be ~1.2 MB,
so the slots could shrink and storage grow:

| Partition | Size | Holds |
|---|---|---|
| `app0` / `app1` | 1.5 MB each | the OS, A/B for rollback |
| `storage` | 18.5 MB | FAT: game packages, themes, watch faces |

Changing the partition table needs one USB reflash — it can't be done safely over the air,
so it waits until there is something else worth one.

**No registry.** The design once put packages in raw flash slots with a free list, a
compactor and an NVS registry; the shipped answer is plainer - a package is a
self-contained file, the FAT is the free list, and the launcher scans the games folder
(`components/loader/loader.c`). No game code is loaded until you tap its icon.

---

## 6. Loading game code — the open question

Everything above is ordinary engineering. This is the risk, and it's honest to say so: the
ESP32-S3 has no dynamic linker, and the Xtensa toolchain doesn't produce position-independent
code the way a desktop does.

**A. Relocatable ELF, fixed up at install time.** The game is built as an ELF with
relocations; at install the addresses are resolved for the slot it lands in and the fixed
image is written. At launch the code runs straight from flash through the instruction cache,
exactly like the firmware — full speed, no RAM spent on code. Costs a relocation pass, and a
rewrite when the region is compacted.

**B. Load into PSRAM and execute there.** Copy the code into PSRAM and map those pages
executable. Simpler slot handling, but it spends PSRAM on every running game and depends on
instruction fetch from PSRAM.

**A is preferred.** Either way the game source doesn't change: the same code builds as a
package or compiled into the OS.

### 6.1 Answered: A's relocations, B's execution - and much smaller than feared

The loader ships A's relocation scheme with B's execution model: the fixed-up image is copied into PSRAM and executed from there (`components/loader/loader.c` allocates from `MALLOC_CAP_SPIRAM` and maps it executable), not run from flash. Measured on the real games rather than argued. Compile a game to an object and it has
thousands of relocations, which is where the fear came from:

```
R_XTENSA_SLOT0_OP  4116      (patches a field inside an instruction - the hard kind)
R_XTENSA_32        3445
R_XTENSA_ASM_EXPAND 777
```

But that is the state *before* a linker has run. Link the game properly — one contiguous
image at base 0, `--no-relax`, `-q` to keep the relocations, unresolved symbols left
unresolved — and almost all of it is already done:

```
total 594, and every single one of these is a section symbol:
  386  R_XTENSA_SLOT0_OP    .text / .rodata / .bss     <- intra-image, already correct
   91  R_XTENSA_32          .text / .rodata / .bss     <- add the load address
   23  R_XTENSA_ASM_EXPAND  section                    <- relaxation hint, nothing to do
  ...  R_XTENSA_32          sinf, cosf, memset, ...    <- 50, resolve by name
```

Two facts make the loader small:

- **Nothing outside the image is reached by a PC-relative instruction.** `-mlongcalls`
  routes every external call through the literal pool, so it lands as a plain 32-bit word.
  Not one `SLOT0_OP` relocation names an external symbol. Load the image contiguously and
  every branch, call and `l32r` inside it is correct without being touched.
- **So the loader handles exactly one relocation type, `R_XTENSA_32`.** Section-relative
  ones get the load address added; named ones are looked up.
  `R_XTENSA_NONE` (the assembler's empty entry - a link with relaxation off ships hundreds
  of them), `R_XTENSA_SLOT0_OP` and
  `R_XTENSA_ASM_EXPAND` are skipped without a look; anything else is a rejection.

What a game still needs by name is the C library in `components/loader/exports.c`: compiler
helpers (`__divsf3` and friends), the float maths, and the mem*/str*/printf core — every one
a standard symbol. The list is deliberately wider than what today's games import: it is the
ordinary C library a small game can reasonably expect, whether or not anything here uses it
yet, and it is append-only — adding a name costs a few table bytes and breaks nothing,
taking one away breaks every package that used it. It is not a console API, which is the
point: these names mean the same thing in ten years, and everything about the console itself
still arrives through the api table.

That leaves ordinary work: pack the linked image into the `CODE` section, copy it to a
64 KB-aligned block, zero the bss, walk every `SHT_RELA` section whose target is
`SHF_ALLOC` (`.rela.text` and `.rela.rodata` - the descriptor lives in the latter), map it
executable, and find `tat_game` in the symbol table.

### 6.2 Done: an installed game runs

PIN DROP, packaged with `tools/mktat.py`, sent to `Games/` over the USB link, picked up by
the launcher and played from the icon. `components/loader` is the whole of it.

Three things about `--emit-relocs` cost a day between them, and all three are in the packer
with the reason written beside them:

- **An addend is relative to the INPUT section.** An output section therefore only
  relocates correctly when it begins with the input section its addends are counted from.
  The literal pool has to sit in front of the code (`l32r` only reaches backwards), so with
  both in one `.text` every function pointer was short by the size of the pool and landed
  in the middle of another function. The pool gets its own output section now.
- **A section per function makes that offset the whole answer**, so `-ffunction-sections`
  turns every function pointer into `.text + 0`. Packages don't use it.
- **Merged string sections get addends that are not section-relative at all** - they came
  out negative. `-fno-merge-constants`, and the strings arrive intact.

Two addresses, not one. The image is mapped twice: ordinarily, where it can be read and
written a byte at a time, and again on the instruction bus where it executes. The
instruction bus only does aligned 32-bit loads, so a pointer to a function carries the
executable address and a pointer to a string the ordinary one - which is the symbol's
section, not a guess. Literal pools are the exception that needs no thought: they live in
`.text` and `l32r` is an aligned 32-bit load.

And one that was not a bug but a design mistake. The first version loaded packages during
start-up, and one bad file took the watch through three crashed boots into safe mode, where
it could not even be asked what had happened. A package comes from a stranger: nothing that
loads one may sit between the console and being able to boot. `tat::PackagedGame` opens the
package in `begin()`, which the engine calls the first time an app is shown - so a package
that will not load costs a message on screen and a tap to go back.

**Themes and watch faces don't depend on any of this**, so the app and the sharing story can
ship without waiting for it.

---

## 7. The USB link

Built and working. It runs over the built-in USB-Serial/JTAG port — the same one that carries
the log and does the flashing — so nothing has to be switched on the watch and no drive is
ever mounted. Frames are found by a sync word and checked with a CRC, so log text and
protocol traffic share the wire safely.

```
A5 5A | len u16 | seq u8 | cmd u8 | payload[len] | crc16      (CRC over seq, cmd, payload)
```

A reply carries the same `seq`, with `cmd | 0x80` for success, or `0xFF` whose payload is a
readable reason for the refusal.

| Command | Payload | Reply |
|---|---|---|
| `HELLO` 0x01 | — | protocol version, API major/minor, firmware version, board - and the Wi-Fi key (a cable is physical access; this is how `update.ps1` bootstraps unattended) |
| `INFO` 0x02 | — | storage total u32, free u32, installed game count u8 |
| `LIST` 0x03 | — | count, then id/name/accent/flags/size per game |
| `ICON` 0x04 | id[16] | that game's icon PNG |
| `FS_FREE` 0x10 | — | storage total, free |
| `FS_LIST` 0x11 | path | entries: is_dir, size, name |
| `FS_PUT` 0x12 | size u32, crc32 u32, path | OK, then `FS_DATA` chunks, then `FS_END` |
| `FS_DATA` 0x13 | offset u32, chunk | OK. The offset lets a resent chunk be recognised rather than counted twice |
| `FS_END` 0x14 | — | OK once the CRC matches; a bad file is deleted, not kept |
| `FS_GET` 0x15 | offset u32, path | up to 4 KB of the file from that offset |
| `FS_DELETE` 0x16 | path | OK (a file, or a folder and everything under it) |
| `FS_MKDIR` 0x17 | path | OK |
| `FS_FORMAT` 0x18 | the exact text `ERASE EVERYTHING` | wipe storage, reply, then restart (so everything seeds fresh); anything else is refused |
| `SHOT` 0x20 | — | width u16, height u16, total u32: a fresh screenshot into the link's cache (docs/reference/remote_debug.md) |
| `SHOT_DATA` 0x21 | offset u32 | up to 4 KB of the cached frame from that offset, RGB565 little-endian |
| `INPUT` 0x22 | query string | runs it like `GET /input`: `tap=233,233`, `app=2`, `tilt=off`, ... |
| `APPS` 0x23 | — | count, then index u8 / id[16] / name[24] per carousel entry, in `app=N` order |

The design once reserved `PKG_BEGIN` / `PKG_DATA` / `PKG_END` / `PKG_REMOVE` package
commands and a `COMPACT`. The shipped answer is plainer: a game installs by `FS_PUT`-ing
its `.tat` into the games folder and uninstalls with `FS_DELETE`, and there is no
compaction because packages are files on a FAT (§5).

Paths are relative to the storage root; `..`, absolute paths and backslashes are refused. The
watch stays awake while a session is open, and the session lapses 30 s after the last frame.

**Opening the port must leave DTR and RTS alone** — esptool uses those lines to reset the
chip, and asserting them would reboot the watch the moment the app connects. `tools/tatlink.py`
is the reference client and shows exactly this.

---

## 8. Retiring the USB drive

Once the app can send files, the watch never needs to appear as a USB drive again. That
removes, in one go:

- the FAT corruption risk that cost a set of themes, and every guard written afterwards
  (reboot postponement, eject-before-update, "don't format unless blank")
- the "CONNECTED TO COMPUTER" screen, the eject dance, and Settings > USB DRIVE
- TinyUSB MSC and its buffers, which is internal RAM back

Plugging in then means exactly one thing — charging, and talking to the app. This shipped:
the watch never exposes its FAT as a USB drive.

---

## 9. The manager app — shipped

**Native, C# + Avalonia**, in `app/`. One codebase, published as a single self-contained file
per platform: `gyrotron-manager.exe` on Windows, and Mac and Linux binaries from the same
source built in GitHub Actions and attached to a release. Nothing for anyone to install.

What it does (see [app/README.md](../../app/README.md) for the details and the CLI):

- Finds the watch by trying `HELLO` on each serial port, and shows firmware, board and the
  game API version.
- **The watch is on the left, your library is on the right** - a plain folder of `.tat` files
  in your documents. Drag a game from one to the other, or drop `.tat` files in from
  anywhere. Taking a game off the watch *moves* it to the library, and the copy is read back
  from disk before the watch's file is deleted.
- **Remove**, with the same confirmation the watch shows.
- Sends themes to the watch's `Theme` folder as well as games (§8).
- The same binary is a command-line tool (`list`, `library`, `add`, `install`, `save`,
  `uninstall`, `send`, `remove`, `bench`, `verify`), which is how all of this is tested against real
  hardware.

`tools/tatlink.py` remains the reference client for the protocol, and what CI and development
use.

---

## 10. Uninstalling on the watch (planned)

Hold an icon on the home carousel:

1. After ~600 ms it lifts and wobbles.
2. A panel: the name, the space it frees, and a plain warning that saves go too.
3. **DELETE** / **CANCEL**. Nothing else on the panel.
4. The carousel closes the gap; the space is free immediately.

Holding a built-in game's icon says it's part of the watch and offers Settings > GAMES
instead. Settings > GAMES keeps working as it does now for hiding without deleting.

---

## 11. Safety

- **A crash loop can't brick the watch.** The console counts boots with an RTC marker
  (`noteBootStarted` / `noteBootStable`, `components/wc_console/console.cpp`); three boots
  in a row that never got stable drop the watch into **safe mode** - themes, sound and
  storage skipped, Wi-Fi forced on, the update screen waiting. Every boot stage leaves a
  crumb ("boot", "audio", "storage", ...), a crash surfaces as a red SOMETHING BROKE screen
  plus `last_crash.txt` for `update.ps1 -Crash` to turn into source lines. A game that
  crashes on launch is therefore not quarantined by name - it just makes the next tap of
  its icon fail honestly, and uninstall is the manager app's job.
- **The OS never loads a game it can't satisfy** (§4.4).
- **Install is checked end to end** (§2), and the registry entry is written last.
- **Memory is accounted per game.** `alloc`/`free` and every canvas and sheet it created are
  released at `unload`, so leaks can't build up across launches.
- **Themes can't execute anything.** They are images and folders the console renders — the
  safest kind of thing to accept from a stranger.
- **The sandbox stops accidents, not malice — say so out loud.** A game runs as ordinary
  machine code in the same privileged address space as the OS (the ESP32-S3 has no user
  mode). What keeps a buggy game contained is the loader: unknown imports fail to load,
  every package is CRC-checked, a gone game's handles are refused, saves are namespaced by
  the id the loader pinned at load, and a crash means quarantine plus safe mode. None of
  that binds a *deliberately* hostile package: it can write any memory or peripheral no
  matter what it imports. The practical defence is distribution — packages arrive over the
  key-checked link — and the rule that follows: install only games whose source you trust.
- **No signing.** Worth revisiting when packages start arriving over the network rather than
  by USB, but a shared file shouldn't *require* a signature — that would kill the trading.
  The format-version byte in the magic is the room a future scheme needs, whichever way it
  lands.

---

## 12. What shipped, and what's next

Each phase left a working watch.

Shipped:

- **The USB link (§7)** - framing, `HELLO`/`INFO`/`LIST`/`ICON`, then the `FS_*` file
  commands. Proven on hardware, and the only install path there is: a game installs by
  writing its `.tat` into the games folder.
- **USB mass storage retired (§8)** - the watch never exposes its FAT; the only way in is
  the link.
- **The loader (§6)** - a relocatable ELF linked at address 0, fixed up into PSRAM at load
  time, executing from there. Handles exactly one relocation type, `R_XTENSA_32`.
- **The game ABI (§4)** - every factory game runs through the api table on API 1.8. Pocket
  Watch stays a firmware app on purpose (§1).
- **The manager app (§9)** - Avalonia GUI + CLI, attached to GitHub releases.

Next, in no fixed order:

- Hold-to-uninstall on the watch (§10) - today removal is the manager app's job.
- The repartition of §5 - one USB reflash, so it waits for something else worth one.

Doom is on the `doom-port` branch - a good test of a package with a large `CODE` section.

---

## 13. Still open

- Packages arriving over the network rather than by USB: worth revisiting signing then, and
  whether the OTA channel should carry packages at all.
- Do packages need to declare the hardware they use, so a future board without an IMU can
  warn rather than misbehave?

Answered since the first draft:

- *Should the app be able to pull a package back off the watch?* Yes - `FS_GET` (§7), and
  the manager's library is built on it.
- *Where does a game's high score go when it's uninstalled and reinstalled?* It is kept:
  saves live in the console's settings store under the game's id, so a game that goes to
  the library and comes back finds them where it left them.
