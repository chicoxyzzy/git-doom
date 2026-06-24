---
title: "DOOM, but every frame is a git commit"
---

# DOOM, but every frame is a git commit

There is no window. No video file. No save file on disk. Just a git repository — and
DOOM is playing inside its commit log.

The branch history *is* the recording: replay it, `git log` it, check out any frame
you like. Saves are git tags carrying the engine's real state,
so you can `git push` a save and someone else fetches it and picks up exactly where you
died. It is a deeply silly use of version control, and it works. Here's how.

(Just want to play? The [README](https://github.com/chicoxyzzy/git-doom).)

![DOOM E1M1, frame by frame, in git](demo.gif)

## The seam

You don't fork DOOM to do this. You lie to it about what a screen is.

DOOM's renderer, simulation, and WAD handling are portable C.
[doomgeneric](https://github.com/ozkl/doomgeneric) pulls the platform-specific parts
out into six functions a port has to fill in:

- `DG_Init` — set up.
- `DG_DrawFrame` — here's a finished 640×400 frame; put it somewhere.
- `DG_GetKey` — give me keyboard events.
- `DG_GetTicksMs` / `DG_SleepMs` — what time is it.
- `DG_SetWindowTitle` — optional.

A sane port wires these to SDL or X11. git-doom wires them to a terminal and a git
repo, and "put the frame somewhere" becomes "render it as text and commit it."
Everything above the seam — the actual DOOM — never finds out.

## A frame becomes text

`DG_ScreenBuffer` hands over 640×400 pixels of `0x00RRGGBB`. A terminal has nowhere
near that many "pixels," so we squint: box-downsample to a character grid — 160×50 by
default, [autoscaled](#autoscaling) to your window — each cell the average color of the
pixels beneath it.

Then we paint with colored blocks:

- **`color` (default):** one cell, one pixel — a solid `█` with its color set as
  *both* foreground and background. Why both? So the cell fills edge to edge no matter
  how your terminal draws the glyph or spaces its lines. No gaps, ever.
- **`half`:** one cell, two stacked pixels — `▀` with the top pixel as foreground and
  the bottom as background. Twice the vertical resolution, if your terminal tiles
  half-blocks cleanly (iTerm2, kitty, WezTerm do).

Three details keep it correct and cheap:

**Aspect.** A terminal cell is about twice as tall as it is wide. Make the grid
`cols ≈ 3.2 × rows` and it displays at ~1.6:1 — exactly DOOM's 320×200. That's why the
default is a wide-looking 160×50 and not something square.

**Delta-encoded color.** Two escapes per cell (`\e[38;2;r;g;bm` and `\e[48;2;r;g;bm`)
times 8000 cells is a wall of bytes, so we emit a color escape only when the color
*changes* from the previous cell. A flat wall costs one escape and then just glyphs.

**Absolute positioning.** Each row is placed with `\e[<row>;1H`, never a newline.
Here's the trap: in raw mode a row exactly as wide as the terminal trips the
right-margin auto-wrap, and a trailing newline then advances a *second* time — a blank
line between every row, on every redraw. Addressing rows by number sidesteps the whole
mess.

### Autoscaling

No size pinned? git-doom asks the terminal how big it is (`ioctl(TIOCGWINSZ)`), takes
`cols = width` and `rows = cols / 3.2`, and installs a `SIGWINCH` handler so dragging
the window or bumping the font re-fits on the next frame. It all runs on the alternate
screen buffer (`\e[?1049h`), so DOOM owns the terminal while it plays and your
scrollback comes back untouched on exit.

## Text becomes a commit

Here's the load-bearing trick: **the frame is the commit message, and the commit has
no files.**

git is content-addressed — blobs, trees, and commits keyed by hash, with branches as
movable name tags. Normally a commit snapshots a tree of files. We don't want files; we
want a message. So every frame commits against the same **empty tree**, on a background
thread. The plumbing version is two commands:

```
git commit-tree <empty-tree> -p <parent> -F <frame>   # write a commit object
git update-ref refs/heads/main <new-sha>              # move the branch forward
```

`commit-tree` writes a commit object straight into the object store — no working tree,
no index, no checkout; nothing touches disk but `.git`. Parent each frame on the last
and the branch becomes a single chain: one commit per frame, in order. The subject line
is a tiny HUD (`frame 312 | hp 87 | 12s`); the body is the frame. Point `HEAD` at
`main` and a plain `git log` scrolls the movie as text.

Every session starts a fresh orphan `main` — last run's recording is gone — and because
all those commits share one empty tree, the repo grows only by commit objects. No
blobs. A whole playthrough is cheap.

### Keeping up with the game

Committing is slower than rendering. A truecolor frame is big — a few hundred KB of
escape codes (≈240 KB at 160×50) — and forking `git commit-tree` plus `update-ref`
*twice per frame* can't keep up at large grids.

So the default doesn't fork per frame: it streams every frame as a commit into one
long-lived **`git fast-import`** process. Same one-commit-per-frame chain, but the
object writes batch into a packfile and there's no per-frame process spawn — roughly
2–7× faster, enough to hold above DOOM's 35 Hz. fast-import only publishes the branch
on a `checkpoint`, so git-doom checkpoints every dozen frames (`GITDOOM_CHECKPOINT`):
the dial between live-`watch` latency and raw throughput. `GITDOOM_FASTIMPORT=0` drops
back to the plain `commit-tree` path above.

Either way, frames reach the commit thread through a bounded queue, and the default is
stubborn — **never drop a frame.** When the queue fills the game thread blocks, so if
git still can't keep up the *game* slows to a lower but complete framerate, flushed to
the last frame on exit. Two switches change that:

- `GITDOOM_BLOCK=0` — drop the oldest queued frame instead of waiting. Smooth to play,
  lossy to watch.
- `GITDOOM_COMMIT=0` — don't record at all. Full speed, no history.

## Reading it back

The viewer is the punchline: it's git and a terminal, and it has never heard of DOOM.

- `git-doom watch` polls the branch tip and reprints the newest commit's message
  (`git show -s --format=%B`) whenever it moves — a live mirror of a game running in
  another terminal.
- `git-doom replay` walks the branch oldest-first (`git rev-list --reverse`) and prints
  each message on a timer.

That's the whole consumer. It renders commit messages; the messages happen to be DOOM.
(Both pass `--no-pager`, or git would open a pager on every single frame.)

## Input from a terminal

Terminals have a cruel asymmetry: they tell you when a key goes *down*, never when it
comes back *up*. DOOM needs the up — you hold W to keep walking. So git-doom fakes it: a
key counts as held from the byte that announced it, and `DG_GetKey` releases it once
150 ms pass with no repeat. Key auto-repeat — the same thing that spams a letter when
you hold it in a text box — is what keeps you moving.

Arrow keys arrive as escape sequences (`ESC [ A`, `ESC O A`, …), so on an `ESC`
git-doom peeks ahead briefly to tell a real arrow from a bare `Esc` (which opens the
menu). The rest is a keymap: `wasd` and the arrows move and turn, `,`/`.` strafe, space
fires, `f` uses, `1`–`7` switch weapons, `tab` is the map, `esc` the menu. Ctrl, Shift,
and Alt can't travel through a terminal as standalone keypresses, so DOOM's
modifier-bound defaults (Ctrl to fire, Shift to run) move to plain keys.

## Saves and loads

This is where it stops being a gimmick. git-doom doesn't fake saving — it drives DOOM's
**real** Save/Load menu and teaches it to write into git instead of a save folder, with
no working tree to hold the file.

**Typing a name.** `esc` → Save Game → pick a slot → type a name. But `wasd` are
remapped to arrows, which would make them useless as letters. DOOM exposes a flag,
`saveStringEnter`, true exactly while you're editing a name — so git-doom watches it and
passes raw characters straight through whenever it's set. The menu reads the typed
character and ignores the synthetic key-*release* (it only acts on presses), so names
come out clean, no doubled letters.

**Writing the save.** DOOM's menu doesn't save on the spot; it raises a `sendsave` flag
for the network/tic loop to act on a tic later. That loop doesn't run reliably in this
stripped-down single-player setup, so git-doom does it by hand: on `sendsave`, it calls
`G_DoSaveGame` directly. That writes a genuine DOOM `.dsg` — the whole world: every
monster, item, your position, health, ammo.

**Into git.** git-doom watches each slot's `.dsg` for a change (mtime and size). The
moment one lands, it becomes a tag:

```
git hash-object -w slotN.dsg          # the real savegame → a blob
git mktree                            # a tree: save.dsg = that blob,
                                      #         thumb.txt = a little screenshot
git commit-tree <tree>                # wrap it in a commit
git tag -f save/slotN <commit>        # one tag per menu slot
```

Note the inversion: frames are empty-tree commits, but a save is a real tree with files
in it — the savegame blob plus a small color thumbnail of the moment you hit save.

**Loading.** Pick a slot in the Load menu and DOOM sets `gameaction = ga_loadgame`;
git-doom spots it and calls `G_DoLoadGame` directly — the same hand-driven trick as
saving.

**Across sessions and machines.** On startup git-doom unpacks every `save/slotN` tag
back into the savegame folder
(`git cat-file blob save/slotN^{tree}:save.dsg`), so a fresh clone already has your
saves listed in the Load menu. And because a save is just a tag, `git push` it — someone
else fetches, loads, and is standing exactly where you were, health and all.

## Why git, and what it costs

git is a content-addressed store of immutable objects with friendly names bolted on
top. That is also — if you squint — a framebuffer and a save system, which is the whole
joke, and it holds up. The cost is throughput: every frame is an object write, and
truecolor frames are big, so at large grids recording can still throttle play — even
streamed through fast-import. Completeness over framerate, with `GITDOOM_BLOCK` and
`GITDOOM_COMMIT` to trade one for the other.

## Map of the code

One platform file, `doomgeneric/doomgeneric_git.c`:

- `DG_DrawFrame` — downsample, render to blocks, queue the commit, drive save/load.
- `render_color` — the block and half-block rendering.
- the commit thread — streams frames into `git fast-import` (`fastimport_*`), or
  `commit_one` (`commit-tree` + `update-ref`) when `GITDOOM_FASTIMPORT=0`.
- `git_sync_slot` / `restore_saves_from_git` — the save-tag machinery.
- `input_thread` / `DG_GetKey` — terminal input and the synthetic key-up.
- `fit_to_terminal` and the `SIGWINCH` handler — autoscaling.

The `git-doom` script wraps it all: `build`, `play`, `watch`, `replay`, `saves`,
`thumb`, `clean`, and `gc`.
