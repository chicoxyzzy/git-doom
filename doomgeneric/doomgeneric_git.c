// doomgeneric_git.c — render DOOM to a grid of truecolor terminal pixels and store every
// frame as a git commit. The commit log IS the framebuffer / recording.
//
// Producer side of git-doom: this binary plays DOOM, draws each frame to the
// terminal, and (on a background thread) appends it as a commit on the main
// branch in a data repo. A separate `git-doom watch` / `replay` reads it back.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"   // players, consoleplayer, gamestate, savegamedir
#include "g_game.h"     // G_SaveGame, G_LoadGame, G_DoLoadGame
#include "p_saveg.h"    // P_SaveGameFile

// We drive DOOM's own save/load menu (F2/F3) and sync each slot to a git tag.
// The menu sets these; the tic loop that would normally consume them doesn't run
// reliably here, so we poll and drive G_DoSaveGame/G_DoLoadGame ourselves.
extern void G_DoSaveGame(void);          // not in g_game.h
extern boolean sendsave;                 // a save was requested via the menu
extern gameaction_t gameaction;          // == ga_loadgame when a load was selected
extern int saveStringEnter;              // menu is editing a save name (type raw)
extern int saveSlot;                     // which slot the menu is saving to
#define MAX_SLOTS 8

// ---- display geometry -------------------------------------------------
// The grid is runtime-sized (GITDOOM_COLS/ROWS) so it can fit any terminal and
// not wrap. Keep cols ≈ 3.2 × rows: at a terminal's ~2:1 cell aspect that yields
// DOOM's 1.6:1. Default 160x50. Every row is exactly `g_cols` chars + '\n'.
#define MAX_COLS 320
#define MAX_ROWS 120
static int g_cols = 160;
static int g_rows = 50;

// Render mode: truecolor pixels via terminal blocks.
//   g_half = 0 -> solid block '█' per cell (1 px/cell, fg+bg = colour, gapless)
//   g_half = 1 -> half-block '▀' (2 px/cell: top=fg, bottom=bg) for 2x detail
static int g_half = 0;

#define CLAMPI(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
// Worst case is color mode: per cell up to two "\e[38;2;r;g;bm" escapes + a
// 3-byte glyph; plus a reset per row.
#define FRAME_CHARS (MAX_ROWS * (MAX_COLS * 42 + 8))

// ---- options (taken from the environment so DOOM keeps its own argv) --------
static char repo_path[1024] = "game-data";
static char branch[256]     = "main";
static int  opt_render      = 1;   // draw frames to the terminal
static int  opt_commit      = 1;   // append frames as commits
static long opt_max_frames  = 0;   // >0: exit after N frames (for testing)

// ---- timing -----------------------------------------------------------------
static uint64_t start_ms;
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- input: raw terminal + timeout-based key-up -----------------------------
// A terminal only delivers key-down bytes (with auto-repeat while held); there
// is no key-up. We synthesize releases: a key is "down" until it hasn't been
// seen for KEY_TIMEOUT_MS, then DG_GetKey emits the up event. Auto-repeat keeps
// held movement keys alive, which is enough to walk and turn.
#define KEY_TIMEOUT_MS 150

static struct termios old_tio;
static int  tty_raw = 0;
static int  tty_out = 0;   // stdout is a terminal -> use alt-screen + clears
static volatile sig_atomic_t want_quit = 0;
static volatile sig_atomic_t got_winch = 0;   // terminal was resized
static int  g_autoscale = 0;                   // size tracks the terminal live
static int  render_cleared = 0;                // has the alt screen been cleared

typedef struct { int pressed; unsigned char key; } kev_t;
#define KQ 256
static kev_t kq[KQ];
static int   kq_head, kq_tail;
static pthread_mutex_t kq_m = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pressed_at[256];   // 0 = up, else last-seen ms

static void kq_push(int pressed, unsigned char key) {
    pthread_mutex_lock(&kq_m);
    int n = (kq_tail + 1) % KQ;
    if (n != kq_head) { kq[kq_tail].pressed = pressed; kq[kq_tail].key = key; kq_tail = n; }
    pthread_mutex_unlock(&kq_m);
}
static int kq_pop(kev_t *e) {
    pthread_mutex_lock(&kq_m);
    int r = 0;
    if (kq_head != kq_tail) { *e = kq[kq_head]; kq_head = (kq_head + 1) % KQ; r = 1; }
    pthread_mutex_unlock(&kq_m);
    return r;
}

static int map_key(int c, unsigned char *out) {
    switch (c) {
        case 'w': case 'W': *out = KEY_UPARROW;    return 1;  // forward
        case 's': case 'S': *out = KEY_DOWNARROW;  return 1;  // back
        case 'a': case 'A': *out = KEY_LEFTARROW;  return 1;  // turn left
        case 'd': case 'D': *out = KEY_RIGHTARROW; return 1;  // turn right
        case ',': case 'q': case 'Q': *out = KEY_STRAFE_L; return 1;
        case '.': case 'e': case 'E': *out = KEY_STRAFE_R; return 1;
        case ' ':           *out = KEY_FIRE;       return 1;
        case 'f': case 'F': *out = KEY_USE;        return 1;  // open doors / use
        case '\r': case '\n': *out = KEY_ENTER;    return 1;
        case '\t':          *out = KEY_TAB;        return 1;  // automap
        case 'y': case 'Y': *out = 'y';            return 1;
        case 'n': case 'N': *out = 'n';            return 1;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7':              *out = (unsigned char)c; return 1;
        default: return 0;
    }
}

static void press(unsigned char k) {
    if (pressed_at[k] == 0) kq_push(1, k);   // new press -> key-down event
    pressed_at[k] = now_ms();                // refresh; DG_GetKey releases on timeout
}

// Read one byte if it arrives within `ms`; 0 on timeout. Used to disambiguate a
// standalone Esc from an arrow-key escape sequence (ESC [ A, ESC O A, ...).
static int poll_byte(unsigned char *c, int ms) {
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&p, 1, ms) > 0) return read(STDIN_FILENO, c, 1) == 1;
    return 0;
}

static void *input_thread(void *arg) {
    (void)arg;
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 3) { want_quit = 1; break; }                   // Ctrl-C
        if (saveStringEnter) {                  // editing a save name: pass raw chars
            if (c == '\r' || c == '\n')      press(KEY_ENTER);
            else if (c == 127 || c == 8)     press(KEY_BACKSPACE);
            else if (c == 27)                press(KEY_ESCAPE);
            else if (c >= 32 && c < 127)     press(c);          // typed character
            continue;
        }
        if (c == 27) {  // Esc: an arrow-key sequence, or the standalone menu key
            unsigned char c2, c3;
            if (poll_byte(&c2, 40) && (c2 == '[' || c2 == 'O') && poll_byte(&c3, 40)) {
                switch (c3) {
                    case 'A': press(KEY_UPARROW);    continue;   // forward
                    case 'B': press(KEY_DOWNARROW);  continue;   // back
                    case 'C': press(KEY_RIGHTARROW); continue;   // turn right
                    case 'D': press(KEY_LEFTARROW);  continue;   // turn left
                    default: break;
                }
                // Swallow any longer CSI sequence (e.g. F-keys ESC[12~) so its
                // trailing bytes don't leak as keypresses. No save/load hotkeys —
                // use the Esc menu (Save Game / Load Game).
                if (c3 >= '0' && c3 <= '9') {
                    unsigned char cn;
                    while (poll_byte(&cn, 40) && cn >= '0' && cn <= '9') { }
                }
                continue;
            }
            press(KEY_ESCAPE);                                    // standalone Esc -> menu
            continue;
        }
        unsigned char k;
        if (map_key(c, &k)) press(k);
    }
    return NULL;
}

// ---- frame -> git commit pipeline (background thread) -----------------------
#define CQ 64   // small: in block mode the queue stays near-full, so keep exit-drain short
static char *cq[CQ];
static int   cq_head, cq_tail;
static pthread_mutex_t cq_m  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cq_cv = PTHREAD_COND_INITIALIZER;   // items available
static pthread_cond_t  cq_space = PTHREAD_COND_INITIALIZER; // space available
static int   stop_commit = 0;
static long  frame_no = 0;
static long  dropped = 0;
static int   opt_block = 1;   // 1: wait for git (never drop); 0: drop oldest when full
static char  empty_tree[64] = "";
static char  parent[64] = "";       // sha of previous frame; "" = orphan root
static pthread_t th_input, th_commit;

static int read_health(void) {
    if (gamestate == GS_LEVEL) return players[consoleplayer].health;
    return -1;
}

static void enqueue_commit(const char *frame, int len) {
    char subj[160];
    int hp = read_health();
    if (hp >= 0)
        snprintf(subj, sizeof subj, "frame %ld | hp %d | %llus",
                 frame_no, hp, (unsigned long long)((now_ms() - start_ms) / 1000));
    else
        snprintf(subj, sizeof subj, "frame %ld | %llus",
                 frame_no, (unsigned long long)((now_ms() - start_ms) / 1000));

    int slen = (int)strlen(subj);
    char *m = malloc(slen + 2 + len + 1);   // "subj\n\n" + frame + NUL
    int o = sprintf(m, "%s\n\n", subj);
    memcpy(m + o, frame, len);
    m[o + len] = '\0';

    pthread_mutex_lock(&cq_m);
    int n = (cq_tail + 1) % CQ;
    if (opt_block) {
        // Never drop: wait for the committer to free a slot. Gameplay throttles to
        // git's commit speed when it can't keep up (heavy color frames), but the
        // recording stays complete.
        while (n == cq_head && !stop_commit) pthread_cond_wait(&cq_space, &cq_m);
        if (stop_commit) { pthread_mutex_unlock(&cq_m); free(m); return; }
    } else if (n == cq_head) {
        free(cq[cq_head]); cq_head = (cq_head + 1) % CQ; dropped++;   // drop oldest
    }
    cq[cq_tail] = m;
    cq_tail = n;
    pthread_cond_signal(&cq_cv);
    pthread_mutex_unlock(&cq_m);
}

// One frame -> one commit, via plumbing (no working tree, no index touched).
// The message file lives at the repo root; since every git call uses `-C repo`,
// the -F argument must be the basename (git resolves it relative to -C).
#define MSG_BASE ".doom-frame-msg"
static char fork_parent[64];
static volatile int fork_pending;
static void commit_one(const char *msg) {
    if (fork_pending) { strncpy(parent, fork_parent, sizeof parent - 1); fork_pending = 0; }
    char msgfile[1100];
    snprintf(msgfile, sizeof msgfile, "%s/%s", repo_path, MSG_BASE);
    FILE *f = fopen(msgfile, "wb");
    if (!f) return;
    fwrite(msg, 1, strlen(msg), f);
    fclose(f);

    char cmd[2400];
    if (parent[0])
        snprintf(cmd, sizeof cmd,
                 "git -C '%s' commit-tree %s -p %s -F '%s' 2>/dev/null",
                 repo_path, empty_tree, parent, MSG_BASE);
    else
        snprintf(cmd, sizeof cmd,
                 "git -C '%s' commit-tree %s -F '%s' 2>/dev/null",
                 repo_path, empty_tree, MSG_BASE);

    FILE *p = popen(cmd, "r");
    if (!p) return;
    char sha[64] = "";
    if (!fgets(sha, sizeof sha, p)) { pclose(p); return; }
    pclose(p);
    sha[strcspn(sha, "\r\n")] = '\0';
    if (strlen(sha) < 7) return;

    strncpy(parent, sha, sizeof parent - 1);
    snprintf(cmd, sizeof cmd, "git -C '%s' update-ref refs/heads/%s %s",
             repo_path, branch, sha);
    int rc = system(cmd);
    (void)rc;
}

static void *commit_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&cq_m);
        while (cq_head == cq_tail && !stop_commit) pthread_cond_wait(&cq_cv, &cq_m);
        if (cq_head == cq_tail && stop_commit) { pthread_mutex_unlock(&cq_m); break; }
        char *m = cq[cq_head];
        cq_head = (cq_head + 1) % CQ;
        pthread_cond_signal(&cq_space);   // a slot opened up
        pthread_mutex_unlock(&cq_m);
        commit_one(m);
        free(m);
    }
    return NULL;
}

// ---- save / load: DOOM engine state <-> git -------------------------------
// A save is a tagged commit whose tree holds the real savegame blob (save.dsg)
// plus a small thumbnail of the moment. Load extracts the blob back into the
// slot file and DOOM applies it.
static int render_color(char *frame, int cols, int rows);   // defined below
#define THUMB_COLS 48   // small preview, not the full frame (keeps save tags light)
#define THUMB_ROWS 15
// fork_parent / fork_pending are declared above (used by the commit thread).

static char *popen_line(const char *cmd, char *buf, size_t n) {
    FILE *p = popen(cmd, "r");
    if (!p) { buf[0] = '\0'; return NULL; }
    char *r = fgets(buf, (int)n, p);
    pclose(p);
    if (!r) { buf[0] = '\0'; return NULL; }
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf;
}

static int write_repo_file(const char *base, const void *data, size_t len) {
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", repo_path, base);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

// Store a saved slot's .dsg (+ a small thumbnail) as a commit tagged save/slotN.
static void git_sync_slot(int slot) {
    char dsg_abs[1200];
    if (!realpath(P_SaveGameFile(slot), dsg_abs)) return;   // git -C needs an absolute path
    char cmd[2600], blobsha[64], thumbsha[64] = "", treesha[64], commitsha[64];

    snprintf(cmd, sizeof cmd, "git -C '%s' hash-object -w '%s' 2>/dev/null", repo_path, dsg_abs);
    if (!popen_line(cmd, blobsha, sizeof blobsha) || strlen(blobsha) < 7) return;

    // small preview of the current frame (not the full-res one — keeps tags light)
    static char thumb[THUMB_ROWS * (THUMB_COLS * 42 + 8) + 1];
    int thumb_len = render_color(thumb, THUMB_COLS, THUMB_ROWS);

    if (write_repo_file(".doom-thumb", thumb, thumb_len)) {
        snprintf(cmd, sizeof cmd, "git -C '%s' hash-object -w .doom-thumb 2>/dev/null", repo_path);
        popen_line(cmd, thumbsha, sizeof thumbsha);
    }

    char tree[256];
    int tn = snprintf(tree, sizeof tree, "100644 blob %s\tsave.dsg\n", blobsha);
    if (strlen(thumbsha) >= 7)
        snprintf(tree + tn, sizeof tree - tn, "100644 blob %s\tthumb.txt\n", thumbsha);
    write_repo_file(".doom-tree", tree, strlen(tree));
    snprintf(cmd, sizeof cmd, "git -C '%s' mktree < '%s/.doom-tree' 2>/dev/null", repo_path, repo_path);
    if (!popen_line(cmd, treesha, sizeof treesha) || strlen(treesha) < 7) return;

    char msg[256];
    int mn = snprintf(msg, sizeof msg, "save slot%d | frame %ld\n\n", slot, frame_no);
    char *body = malloc(mn + thumb_len + 1);
    memcpy(body, msg, mn);
    memcpy(body + mn, thumb, thumb_len);
    write_repo_file(".doom-savemsg", body, mn + thumb_len);
    free(body);
    if (parent[0])
        snprintf(cmd, sizeof cmd, "git -C '%s' commit-tree %s -p %s -F .doom-savemsg 2>/dev/null", repo_path, treesha, parent);
    else
        snprintf(cmd, sizeof cmd, "git -C '%s' commit-tree %s -F .doom-savemsg 2>/dev/null", repo_path, treesha);
    if (!popen_line(cmd, commitsha, sizeof commitsha) || strlen(commitsha) < 7) return;

    snprintf(cmd, sizeof cmd, "git -C '%s' tag -f 'save/slot%d' %s >/dev/null 2>&1", repo_path, slot, commitsha);
    if (system(cmd) == 0)
        fprintf(stderr, "git-doom: saved save/slot%d @ frame %ld\n", slot, frame_no);
}

// At startup, materialise any save/slotN tags into the savegame dir so DOOM's
// load menu lists them (across sessions). Call once savegamedir is set.
static void restore_saves_from_git(void) {
    if (!opt_commit) return;
    char cmd[2600];
    struct stat st;
    for (int s = 0; s < MAX_SLOTS; s++) {
        char *dsg = P_SaveGameFile(s);
        if (stat(dsg, &st) == 0 && st.st_size > 0) continue;   // already present this session
        snprintf(cmd, sizeof cmd, "git -C '%s' rev-parse -q --verify 'refs/tags/save/slot%d' >/dev/null 2>&1", repo_path, s);
        if (system(cmd) != 0) continue;                        // no such save
        snprintf(cmd, sizeof cmd,
                 "git -C '%s' cat-file blob 'save/slot%d^{tree}:save.dsg' > '%s' 2>/dev/null",
                 repo_path, s, dsg);
        int rc = system(cmd); (void)rc;
    }
}

static void git_init_repo(void) {
    char cmd[1300];
    struct stat st;
    char gitdir[1100];
    snprintf(gitdir, sizeof gitdir, "%s/.git", repo_path);
    if (stat(gitdir, &st) != 0) {
        snprintf(cmd, sizeof cmd, "git init -q '%s'", repo_path);
        if (system(cmd) != 0) { opt_commit = 0; return; }
        // Point HEAD at the main branch so a plain `git log` in the data repo
        // shows the frames (instead of the empty default branch).
        snprintf(cmd, sizeof cmd,
                 "git -C '%s' config user.email doom@git.local && "
                 "git -C '%s' config user.name git-doom && "
                 "git -C '%s' symbolic-ref HEAD 'refs/heads/%s'",
                 repo_path, repo_path, repo_path, branch);
        int rc = system(cmd); (void)rc;
    }
    // Materialise the empty tree object so commit-tree can reference it.
    snprintf(cmd, sizeof cmd, "git -C '%s' hash-object -w -t tree /dev/null", repo_path);
    FILE *p = popen(cmd, "r");
    if (p) { if (fgets(empty_tree, sizeof empty_tree, p)) empty_tree[strcspn(empty_tree, "\r\n")] = '\0'; pclose(p); }
    // Fresh match = fresh orphan main: parent starts empty, so the first commit
    // is a root and update-ref overwrites the main branch.
    parent[0] = '\0';
}

// Size the grid to the terminal: fill the width, cap rows to the height, keep
// cols ~= 3.2*rows (DOOM's aspect at a ~2:1 cell). Used at startup and on resize.
static void fit_to_terminal(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) return;
    int c = ws.ws_col;
    int r = (int)(c / 3.2);
    if (r > ws.ws_row - 1) r = ws.ws_row - 1;
    g_cols = CLAMPI(c, 16, MAX_COLS);
    g_rows = CLAMPI(r, 8, MAX_ROWS);
}

static void on_winch(int sig) { (void)sig; got_winch = 1; }

// ---- env / cleanup ----------------------------------------------------------
static void parse_env(void) {
    const char *v;
    int pinned = 0;
    if ((v = getenv("GITDOOM_REPO")))   { strncpy(repo_path, v, sizeof repo_path - 1); }
    if ((v = getenv("GITDOOM_BRANCH"))) { strncpy(branch, v, sizeof branch - 1); }
    if ((v = getenv("GITDOOM_RENDER"))) { opt_render = atoi(v); }
    if ((v = getenv("GITDOOM_COMMIT"))) { opt_commit = atoi(v); }
    if ((v = getenv("GITDOOM_MAXFRAMES"))) { opt_max_frames = atol(v); }
    if ((v = getenv("GITDOOM_COLS")))     { g_cols = CLAMPI(atoi(v), 16, MAX_COLS); pinned = 1; }
    if ((v = getenv("GITDOOM_ROWS")))     { g_rows = CLAMPI(atoi(v), 8,  MAX_ROWS); pinned = 1; }
    g_autoscale = !pinned;   // no explicit size -> track the terminal live
    if ((v = getenv("GITDOOM_AUTOSCALE"))) { g_autoscale = atoi(v); }
    if ((v = getenv("GITDOOM_BLOCK")))     { opt_block = atoi(v); }
    if ((v = getenv("GITDOOM_CHARSET"))) {
        g_half = (!strcmp(v, "half") || !strcmp(v, "halfblock"));   // else solid "color"
    }
}

static void restore_term(void) {
    if (tty_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &old_tio); tty_raw = 0; }
    if (opt_render && tty_out) {
        fputs("\x1b[?25h\x1b[?7h\x1b[?1049l", stdout);   // show cursor, restore autowrap, leave alt screen
        fflush(stdout);
        tty_out = 0;
    }
}

static void shutdown_and_exit(int code) {
    // Drain pending frames so the recording is complete.
    if (opt_commit) {
        pthread_mutex_lock(&cq_m);
        int queued = (cq_tail - cq_head + CQ) % CQ;
        stop_commit = 1;
        pthread_cond_signal(&cq_cv);
        pthread_cond_broadcast(&cq_space);   // wake a blocked producer, if any
        pthread_mutex_unlock(&cq_m);
        if (queued > 4) fprintf(stderr, "git-doom: flushing %d queued frame(s)…\n", queued);
        pthread_join(th_commit, NULL);
    }
    restore_term();
    if (dropped) fprintf(stderr, "git-doom: dropped %ld frame(s) (git couldn't keep up)\n", dropped);
    exit(code);
}

static void on_signal(int sig) { (void)sig; want_quit = 1; }

// ---- doomgeneric platform callbacks -----------------------------------------
void DG_Init(void) {
    start_ms = now_ms();
    parse_env();

    if (isatty(STDIN_FILENO)) {
        tcgetattr(STDIN_FILENO, &old_tio);
        struct termios raw = old_tio;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        tty_raw = 1;
    }
    tty_out = isatty(STDOUT_FILENO);
    if (g_autoscale && tty_out) {
        fit_to_terminal();                 // size to the window now
        signal(SIGWINCH, on_winch);        // and track resizes live
    }
    if (opt_render && tty_out) {
        fputs("\x1b[?1049h\x1b[?7l\x1b[2J\x1b[H\x1b[?25l", stdout);   // alt screen, no-autowrap, clear, hide cursor
        fflush(stdout);
    }

    if (opt_commit) {
        git_init_repo();
        pthread_create(&th_commit, NULL, commit_thread, NULL);
    }
    pthread_create(&th_input, NULL, input_thread, NULL);

    atexit(restore_term);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}

// Average framebuffer RGB over [x0,x1) x [y0,y1).
static void avg_rgb(int x0, int x1, int y0, int y1, int *pr, int *pg, int *pb) {
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    unsigned long sr = 0, sg = 0, sb = 0; int n = 0;
    for (int y = y0; y < y1; y++) {
        pixel_t *row = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;
        for (int x = x0; x < x1; x++) {
            uint32_t px = row[x];
            sr += (px >> 16) & 0xFF; sg += (px >> 8) & 0xFF; sb += px & 0xFF; n++;
        }
    }
    if (n < 1) n = 1;
    *pr = (int)(sr / n); *pg = (int)(sg / n); *pb = (int)(sb / n);
}

// Truecolor pixel render.
//   solid (default): one █ per cell, colour set as BOTH fg and bg, so the whole
//     cell fills regardless of the terminal's background/line-spacing handling —
//     no gaps. 1 px/cell.
//   half: '▀' with top pixel = fg, bottom pixel = bg -> 2 px/cell (double the
//     vertical detail), but relies on the terminal tiling half-blocks cleanly.
// Colour escapes are delta-encoded (emitted only on change) to stay compact.
static int render_color(char *frame, int COLS, int ROWS) {
    int idx = 0;
    if (g_half) {
        int PH = 2 * ROWS;
        for (int cy = 0; cy < ROWS; cy++) {
            int pfr = -1, pfg = -1, pfb = -1, pbr = -1, pbg = -1, pbb = -1;
            for (int cx = 0; cx < COLS; cx++) {
                int x0 = cx * DOOMGENERIC_RESX / COLS, x1 = (cx + 1) * DOOMGENERIC_RESX / COLS;
                int tr, tg, tb, br, bg, bb;
                avg_rgb(x0, x1, (2*cy)   * DOOMGENERIC_RESY / PH, (2*cy+1) * DOOMGENERIC_RESY / PH, &tr, &tg, &tb);
                avg_rgb(x0, x1, (2*cy+1) * DOOMGENERIC_RESY / PH, (2*cy+2) * DOOMGENERIC_RESY / PH, &br, &bg, &bb);
                if (tr != pfr || tg != pfg || tb != pfb) { idx += sprintf(frame + idx, "\x1b[38;2;%d;%d;%dm", tr, tg, tb); pfr = tr; pfg = tg; pfb = tb; }
                if (br != pbr || bg != pbg || bb != pbb) { idx += sprintf(frame + idx, "\x1b[48;2;%d;%d;%dm", br, bg, bb); pbr = br; pbg = bg; pbb = bb; }
                frame[idx++] = (char)0xE2; frame[idx++] = (char)0x96; frame[idx++] = (char)0x80; // ▀ U+2580
            }
            idx += sprintf(frame + idx, "\x1b[0m\n");
        }
    } else {
        for (int cy = 0; cy < ROWS; cy++) {
            int pr = -1, pg = -1, pb = -1;
            int y0 = cy * DOOMGENERIC_RESY / ROWS, y1 = (cy + 1) * DOOMGENERIC_RESY / ROWS;
            for (int cx = 0; cx < COLS; cx++) {
                int x0 = cx * DOOMGENERIC_RESX / COLS, x1 = (cx + 1) * DOOMGENERIC_RESX / COLS;
                int r, g, b;
                avg_rgb(x0, x1, y0, y1, &r, &g, &b);
                if (r != pr || g != pg || b != pb) {
                    idx += sprintf(frame + idx, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm", r, g, b, r, g, b);
                    pr = r; pg = g; pb = b;
                }
                frame[idx++] = (char)0xE2; frame[idx++] = (char)0x96; frame[idx++] = (char)0x88; // █ U+2588
            }
            idx += sprintf(frame + idx, "\x1b[0m\n");
        }
    }
    frame[idx] = '\0';
    return idx;
}

void DG_DrawFrame(void) {
    static char frame[FRAME_CHARS + 1];
    if (got_winch) {                       // terminal resized: re-fit and repaint
        got_winch = 0;
        if (g_autoscale) fit_to_terminal();
        render_cleared = 0;
    }
    const int COLS = g_cols, ROWS = g_rows;
    int idx = render_color(frame, COLS, ROWS);

    if (opt_render) {
        if (tty_out) {
            // Place each row with an absolute cursor move (ESC[r;1H) rather than
            // trusting newlines. In raw mode a row whose width equals the terminal
            // width triggers right-margin autowrap, and the following bare '\n'
            // then advances again — inserting a blank line between every row (the
            // "gaps"). Absolute positioning is immune to that in any terminal.
            if (!render_cleared) { fputs("\x1b[2J", stdout); render_cleared = 1; }
            const char *p = frame, *end = frame + idx;
            int r = 1;
            while (p < end) {
                const char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
                fprintf(stdout, "\x1b[%d;1H", r++);
                fwrite(p, 1, len, stdout);
                if (!nl) break;
                p = nl + 1;
            }
        } else {
            fwrite(frame, 1, idx, stdout);   // non-tty: plain dump
        }
        fflush(stdout);
    }
    if (opt_commit) enqueue_commit(frame, idx);

    // --- native save/load menu, synced to git (driven from the game thread) ---
    if (opt_commit) {
        // The menu requested a save but the tic loop didn't write it: do it now.
        if (sendsave && gamestate == GS_LEVEL) { sendsave = false; G_DoSaveGame(); }
        // Sync any slot whose .dsg was just (re)written — catches saves from
        // either path. (Loads don't change the file, so they aren't re-synced.)
        static long slot_seen[MAX_SLOTS];
        static int  seeded = 0;
        for (int s = 0; s < MAX_SLOTS; s++) {
            struct stat st;
            long m = (stat(P_SaveGameFile(s), &st) == 0) ? (long)st.st_mtime * 1000000 + st.st_size : 0;
            if (!seeded) { slot_seen[s] = m; }
            else if (m && m != slot_seen[s]) { slot_seen[s] = m; git_sync_slot(s); }
        }
        seeded = 1;
        // A menu load was selected: apply it now (tic loop is unreliable).
        if (gameaction == ga_loadgame) G_DoLoadGame();
    }

    frame_no++;
    if (want_quit) shutdown_and_exit(0);
    if (opt_max_frames && frame_no >= opt_max_frames) shutdown_and_exit(0);
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)(now_ms() - start_ms);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    // Emit synthetic key-up for anything that timed out (auto-release).
    uint64_t t = now_ms();
    for (int k = 0; k < 256; k++) {
        if (pressed_at[k] && t - pressed_at[k] > KEY_TIMEOUT_MS) {
            pressed_at[k] = 0;
            *pressed = 0;
            *key = (unsigned char)k;
            return 1;
        }
    }
    kev_t e;
    if (kq_pop(&e)) { *pressed = e.pressed; *key = e.key; return 1; }
    return 0;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    restore_saves_from_git();   // savegamedir is set now; populate the load menu
    for (;;) doomgeneric_Tick();
    return 0;
}
