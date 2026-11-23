# ECHO

Watch the pads light up, then touch them in the same order. One more each round,
for as long as you can remember. The one game here that does not use tilt - and the
reference for writing your own game (`echo.c` is a whole game in ~530 lines).

## Controls

- Four pads. The watch plays a pattern - WATCH - and then it is YOUR GO: **touch the pads**
  in the same order. A pad stays lit for as long as your finger is on it, like a real button
- Get it right and the pads cheer, then the pattern grows by one. It gets quicker every four
  rounds, and there is no last round: it goes on for as long as you can remember it
- Touch a wrong pad, or take longer than five beats of the pattern over one, and you lose;
  the round you reached is your score
- That is all of it, on purpose: the one game here that does not use tilt. A first version
  pressed pads by tipping the watch toward them, and it was clever and no fun
- **Swipe left** menu (sound, restart game, best) · **BOOT** home. Best round is saved

---

Part of **Gyro-Tron** — [the console](../../README.md) ·
Pack: `python tools/mktat.py games/echo` ·
Game API: [docs/reference/GAME_API.md](../../docs/reference/GAME_API.md)
