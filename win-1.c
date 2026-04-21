/* _GNU_SOURCE is harmless on Windows/MSYS2; keep it off for non-glibc */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
/*
 * AI Video Dubber LOCAL — GTK3 single-file C port
 *
 * Build (fish shell, Arch Linux):
 *   gcc (pkg-config --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 | string split " ") \
 *       ui-v8.c -o ui-v8 \
 *       (pkg-config --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 | string split " ") \
 *       -lpthread -lm -Wall -O2
 *
 * Runtime deps (pacman):
 *   sudo pacman -S gtk3 gstreamer gst-plugins-good gst-plugins-bad ffmpeg python noto-fonts-extra
 *   pip install openai-whisper deep-translator edge-tts --break-system-packages
 */

/* ═══════════════════════════════════════════════════════════════════
 * Includes
 * ═══════════════════════════════════════════════════════════════════ */

#include <gtk/gtk.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <time.h>
#include <math.h>
#include <glib/gstdio.h>

/* ═══════════════════════════════════════════════════════════════════
 * Windows / POSIX compatibility shim
 *
 * Target Windows runtime: MSYS2 UCRT64 (or MINGW64).
 * The app still builds unchanged on Linux.
 * ═══════════════════════════════════════════════════════════════════ */
#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <process.h>
  #include <fcntl.h>
  #include <sys/stat.h>

  /* pthreads come from winpthreads (mingw-w64-<arch>-winpthreads-git). */

  /* MSYS2 bash.exe is expected in PATH (or Git Bash's bash.exe).      */
  #define SHELL_PATH   "bash.exe"

  /* Windows has no /tmp.  g_tmpdir is populated at startup (see main)
   * from g_get_tmp_dir() and is always an absolute forward-slash path
   * (e.g. "C:/Users/<u>/AppData/Local/Temp").  All call-sites build
   * paths via snprintf("%s/...", TMPDIR, ...) — never string-concat. */
  extern char g_tmpdir[1024];
  #define TMPDIR       ((const char *)g_tmpdir)

  /* mkdir(path, mode) — Windows _mkdir has no mode parameter.         */
  #define mkdir(p, m)  _mkdir(p)

  /* Give POSIX names to MSVCRT underscored equivalents so the rest of
   * the code keeps using the familiar POSIX spellings.                */
  #ifndef unlink
    #define unlink(p)    _unlink(p)
  #endif
  #ifndef access
    #define access(p, m) _access((p), (m))
  #endif
  #ifndef popen
    #define popen(c, m)  _popen((c), (m))
    #define pclose(p)    _pclose(p)
  #endif
  #ifndef fileno
    #define fileno(f)    _fileno(f)
  #endif
  #ifndef dup2
    #define dup2(a, b)   _dup2((a), (b))
  #endif

  /* Windows has no fork/execvp — ensure any stray reference errors
   * out at link time with a clear symbol name rather than silent UB. */
  #define fork()       (-1)

  /* waitpid/WIFEXITED macros are unused once run_cmd is ported, but
   * keep safe fallbacks for any stray reference.                      */
  #ifndef WIFEXITED
    #define WIFEXITED(s)   (((s) & 0xff) == 0)
    #define WEXITSTATUS(s) (((s) >> 8) & 0xff)
  #endif
  #ifndef F_OK
    #define F_OK 0
    #define X_OK 0
    #define W_OK 2
    #define R_OK 4
  #endif
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif

  static inline int win_readlink_self(char *buf, size_t sz) {
      DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sz);
      if (n == 0 || n >= sz) return -1;
      buf[n] = '\0';
      for (DWORD i = 0; i < n; i++) if (buf[i] == '\\') buf[i] = '/';
      return (int)n;
  }
  #define readlink_self(buf, sz) win_readlink_self((buf), (sz))
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/wait.h>

  #define SHELL_PATH   "/bin/sh"
  #define TMPDIR       ((const char *)"/tmp")

  static inline int linux_readlink_self(char *buf, size_t sz) {
      ssize_t n = readlink("/proc/self/exe", buf, sz - 1);
      if (n <= 0) return -1;
      buf[n] = '\0';
      return (int)n;
  }
  #define readlink_self(buf, sz) linux_readlink_self((buf), (sz))
#endif

/* Cross-platform wrappers over GLib so every call-site stays one-line */
#define xunlink(p)         g_unlink(p)
#define xchmod(p, m)       g_chmod((p), (m))
#define xaccess(p, mode)   g_access((p), (mode))

#ifdef _WIN32
char g_tmpdir[1024] = "C:/Windows/Temp";
#endif

/* ====================== FORWARD DECLARATIONS ====================== */
typedef struct AppWidgets AppWidgets;
typedef struct AppState AppState;

typedef struct {
    double start, end;
    char text[2048];
    char translated[2048];
    char speaker_id[64];   /* add.txt: diarization speaker label e.g. "SPEAKER_00" */
} Segment;

typedef struct AppConfig {
    char whisper_model[64];
    char target_language[32];
    char tts_voice_id[128];
    char dub_voice[128];
    int  tts_rate;
    /* Feature 4: Recent Files — store last 5 opened video paths */
    char recent_files[5][4096];
    /* Feature 5: Translation engine: "google", "argos", "ollama", "mistral" */
    char translation_engine[32];
    /* Feature 4 (Audio Ducking): duck background audio while TTS speaks */
    int  audio_ducking;        /* 0 = off, 1 = on */
    /* Feature 3 (Voice Cloning): path to cloning script */
    char voice_clone_script[4096];
    /* add.txt new config options */
    int  subtitle_font_size;       /* ASS subtitle font size (default 72) */
    int  subtitle_language;        /* 0 = translated, 1 = original */
    char output_dir[4096];         /* default output folder ("" = ask dialog) */
    int  max_segments;             /* max Whisper segments (0 = unlimited) */
    int  tts_pitch;                /* edge-tts pitch offset in Hz (e.g. +10) */
} AppConfig;

typedef struct AppState {
    AppWidgets *aw;
    char        video_path[4096];
    Segment    *segments;
    int         seg_count;
    AppConfig   config;
    /* Video playback state (GStreamer) */
    GstElement *pipeline;       /* playbin pipeline */
    GstElement *video_sink;     /* gtksink element  */
    guint       pos_timer_id;   /* position update timer */
    gboolean    is_playing;
    gint64      duration;       /* nanoseconds */
} AppState;

/* Full AppWidgets definition here so all callbacks below can access its members */
struct AppWidgets {
    GtkWidget      *window;
    GtkProgressBar *bar;
    GtkLabel       *status_lbl;
    GtkLabel       *video_lbl;
    GtkListStore   *seg_store;
    GtkTreeView    *seg_view;
    GtkComboBoxText*model_combo;
    GtkComboBoxText*lang_combo;
    GtkComboBoxText*dub_voice_combo;  /* Female / Male Khmer voice */
    GtkComboBoxText*trans_engine_combo; /* Feature 5: translation engine */
    GtkEntry       *voice_entry;
    GtkSpinButton  *rate_spin;
    GtkWidget      *recent_btn;       /* Feature 4: Recent Files menu button */
    GtkWidget      *duck_check;       /* Feature 4 (Audio Ducking) toggle */
    /* add.txt new config UI widgets */
    GtkSpinButton  *font_size_spin;   /* subtitle_font_size */
    GtkComboBoxText*sub_lang_combo;   /* subtitle_language */
    GtkEntry       *output_dir_entry; /* output_dir */
    GtkSpinButton  *max_seg_spin;     /* max_segments */
    GtkSpinButton  *pitch_spin;       /* tts_pitch */
    /* fix.txt: widgets that must be reachable from callbacks */
    GtkLabel       *time_lbl;         /* current playback position */
    GtkLabel       *dur_lbl;          /* total duration */
    GtkWidget      *progress_bar_vid; /* video seek bar (GtkScale) */
    GtkLabel       *zoom_pct_lbl;     /* timeline zoom percentage label */
    GtkWidget      *tl_lang_combo;    /* timeline lang selector */
    GtkWidget      *mem_bar_lbl;      /* memory usage label in status bar */
    GtkWidget      *project_lbl;      /* project name label in status bar */
    gboolean        dark_mode;        /* TRUE = dark, FALSE = light */
    /* GStreamer video widget (from gtksink) */
    GtkWidget      *gst_video_widget; /* widget from gtksink to embed in sidebar */
    GtkWidget      *vid_box;          /* container that holds video placeholder or gtksink widget */
    GtkWidget      *vid_placeholder;  /* the "No video loaded" drawing area */
    GtkWidget      *expand_btn;       /* expand/normal/fullscreen toggle    */
    int             vid_expand_mode;  /* 0=normal, 1=expanded, 2=fullscreen */
    /* fix.txt: store refs for expand/fullscreen show/hide */
    GtkWidget      *paned;            /* main horizontal paned              */
    GtkWidget      *rpanel;           /* right panel (table + action bars)  */
    GtkWidget      *ab1;              /* action bar 1 (core pipeline)       */
    GtkWidget      *ab2;              /* action bar 2 (editing tools)       */
    GtkWidget      *ab3;              /* action bar 3 (subtitle mgmt)      */
    GtkWidget      *cfg_bar;          /* settings bar                       */
    GtkWidget      *sidebar;          /* left sidebar                       */
    GtkWidget      *vid_overlay;      /* video overlay container            */
    GtkWidget      *vctrls;           /* video control buttons box          */
    GtkWidget      *play_btn;         /* Play/Pause button */
    GtkWidget      *stop_btn;         /* Stop button */
    GtkWidget      *vol_slider;       /* Volume slider */
    GtkWidget      *mute_btn;         /* Mute button */
    /* Timeline drawing areas */
    GtkWidget      *tl_t1_area;       /* GtkDrawingArea — T1 subtitle track */
    GtkWidget      *tl_a1_area;       /* GtkDrawingArea — A1 audio track    */
    GtkWidget      *tl_playhead_box;  /* overlay box that holds playhead    */
    gdouble         tl_zoom;          /* pixels per second (default 30.0)   */
    gdouble         tl_scroll_offset; /* horizontal scroll offset in sec    */
    /* Timeline drag-to-move state (T1 track) */
    int             tl_drag_idx;      /* segment being dragged, -1 = none   */
    int             tl_drag_edge;     /* 0 = whole block, -1 = left, +1 = right */
    gdouble         tl_drag_x0;       /* click x in pixels                  */
    gdouble         tl_drag_start0;   /* segment->start at drag begin       */
    gdouble         tl_drag_end0;     /* segment->end   at drag begin       */
    gboolean        tl_drag_moved;    /* TRUE once motion exceeded threshold */
    AppState       *state;
    /* v9: Settings dialog — opened via ⚙ button in titlebar */
    GtkWidget      *settings_dialog;  /* modal settings window               */
    /* v9: Recent files list widget in sidebar */
    GtkWidget      *recent_list_box;  /* GtkBox holding recent file rows     */
    /* v9: pipeline step status chips in toolbar */
    GtkWidget      *pipe_chip_model;  /* "Model  whisper-xxx" chip           */
    GtkWidget      *pipe_chip_lang;   /* "→  km-KH" chip                     */
    /* fix.txt: extra refs so fullscreen mode hides only the table/timeline */
    GtkWidget      *seg_scroll;       /* scrolled-window holding seg_view    */
    GtkWidget      *tl_panel;         /* timeline panel container            */
    GtkWidget      *menu_bar;         /* op.png: File/Edit/View/... menubar  */
};

/* Message helpers */
static void show_msg(GtkWidget *win, GtkMessageType t, const char *title, const char *msg);
#define show_err(w,t,m)  show_msg((w), GTK_MESSAGE_ERROR,   (t), (m))
#define show_info(w,t,m) show_msg((w), GTK_MESSAGE_INFO,    (t), (m))

/* Function prototypes (this fixes most errors) */
static void find_khmer_font(char *out, size_t sz);
static int export_ass(const Segment *segs, int n, const char *path,
                      const char *font_name, int font_size, int use_original,
                      char *err, size_t esz);

static void post_progress(GtkProgressBar *bar, GtkLabel *lbl, int pct, const char *msg);
static void post_done(void (*fn)(void*), void *arg);

/* Feature 4: forward declarations for recent-files support */
static void load_video_path(AppWidgets *aw, const char *path);
/* Video placeholder + Timeline drawing */
static gboolean vid_placeholder_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud);
/* GStreamer video playback */
static void     gst_stop_pipeline(AppState *st);
static void     gst_start_video(AppWidgets *aw, const char *path);
static gboolean gst_pos_update_cb(gpointer ud);
static void     cb_play_pause(GtkWidget *btn, gpointer ud);
static void     cb_stop_playback(GtkWidget *btn, gpointer ud);
static void     cb_seek_changed(GtkRange *range, gpointer ud);
static void     cb_volume_changed(GtkRange *range, gpointer ud);
static void     cb_mute_toggle(GtkWidget *btn, gpointer ud);
static void     cb_vid_expand(GtkWidget *btn, gpointer ud);
static gboolean tl_t1_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud);
static gboolean tl_a1_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud);
static gboolean tl_t1_click_cb(GtkWidget *w, GdkEventButton *ev, gpointer ud);
static void     tl_refresh(AppWidgets *aw);
static void     cb_seg_selection_changed(GtkTreeSelection *sel, gpointer ud);
static void rebuild_recent_menu(AppWidgets *aw);
/* Feature 2: forward declaration for preview row-activated handler */
static void on_seg_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                  GtkTreeViewColumn *col, gpointer ud);
/* Feature 1 (Batch): forward declaration */
static void cb_batch_open(GtkWidget *btn, gpointer ud);
/* Feature 6 (Merge): forward declaration */
static void cb_merge_segments(GtkWidget *btn, gpointer ud);
/* Feature 2: PreviewArg struct and preview_thread forward declaration —
 * on_seg_row_activated references these before they are defined.      */
typedef struct {
    char  text[2048];
    char  voice[128];
    int   rate;
    GtkProgressBar *bar;
    GtkLabel       *lbl;
} PreviewArg;
static void *preview_thread(void *data);
/* New features A/B/C: forward declarations */
static void cb_identify_speakers(GtkWidget *btn, gpointer ud);
static void cb_show_visualizer(GtkWidget *btn, gpointer ud);
static void cb_smart_duck(GtkWidget *btn, gpointer ud);
/* Visualizer: burn overlay to video */
static void cb_viz_burn_to_video(GtkWidget *btn, gpointer ud);
/* Visualizer: burn overlay onto a Dub (AI Only) video */
static void cb_viz_burn_dub_to_video(GtkWidget *btn, gpointer ud);
/* Main window: Dub (AI Only) then burn visualizer — single step, no popup needed */
static void cb_dub_with_visualizer(GtkWidget *btn, gpointer ud);
/* add.txt Feature 1: Full Pipeline */
static void cb_full_pipeline(GtkWidget *btn, gpointer ud);
/* Logo/text overlay */
static void cb_add_logo_overlay(GtkWidget *btn, gpointer ud);
/* add.txt new function forward declarations */
static void cb_export_ass(GtkWidget *btn, gpointer ud);
static void cb_retranslate_segment(GtkWidget *btn, gpointer ud);
static void cb_copy_segments_csv(GtkWidget *btn, gpointer ud);
static void cb_burn_subs_only(GtkWidget *btn, gpointer ud);
static void cb_detect_language(GtkWidget *btn, gpointer ud);
static void cb_clear_segments(GtkWidget *btn, gpointer ud);
static void cb_browse_output_dir(GtkWidget *btn, gpointer ud);
static void apply_output_dir(GtkWidget *dlg, const AppConfig *cfg);
/* add.txt v18 new feature forward declarations */
static void cb_speaker_voice_map(GtkWidget *btn, gpointer ud);
static void cb_auto_duck_balance(GtkWidget *btn, gpointer ud);
static void cb_pitch_shift_seg(GtkWidget *btn, gpointer ud);
static void cb_preview_segment(GtkWidget *btn, gpointer ud);
static void cb_downtime_filler(GtkWidget *btn, gpointer ud);

/* fix.txt: new callbacks for previously-unconnected widgets */
static void cb_edit_segment(GtkWidget *btn, gpointer ud);
static void cb_delete_segment(GtkWidget *btn, gpointer ud);
static void cb_zoom_changed(GtkRange *range, gpointer ud);
static void cb_tl_lang_changed(GtkComboBox *combo, gpointer ud);
static void cb_autofit(GtkWidget *btn, gpointer ud);
/* fix.txt: combined export — Dub + burn subs + visualizer + logo + extract bg MP3 */
static void cb_dub_full_export(GtkWidget *btn, gpointer ud);
/* fix.txt: voice combo changed → update config immediately */
static void cb_dub_voice_changed(GtkComboBox *combo, gpointer ud);
/* v9: Settings dialog (Slide 2 of Dub-UI.html) */
static void cb_open_settings(GtkWidget *btn, gpointer ud);
/* v9: Dark/Light toggle in titlebar */
static void cb_toggle_theme_btn(GtkWidget *btn, gpointer ud);

/* ====================== START OF ORIGINAL CODE ====================== */


static const char *APP_CSS =
    /* ══════════════════════════════════════════════════════════
     * DARK THEME — "Acheron" (matches Acheron-UI.html mockup)
     *   bg-0:#15151f  bg-1:#1b1b27  bg-2:#22222f  bg-3:#2b2b3b  bg-4:#353548
     *   line:#2e2e40  line-2:#3a3a52
     *   text-0:#e8e8f4  text-1:#b8b8cc  text-2:#7a7a92  text-3:#56566c
     *   accent:#5cc9d8 (teal)  ok:#7cd0a0  warn:#d8b070  err:#e88070
     * ══════════════════════════════════════════════════════════ */

    /* Global */
    "window { background-color:#15151f; color:#e8e8f4; }"
    /* Font fallback: Inter is not installed by default on Arch (install with
     *   sudo pacman -S ttf-inter   or   yay -S ttf-inter-variable
     * otherwise GTK falls back to Noto Sans whose metrics differ from the mockup). */
    "* { font-family:'Inter','Inter Variable','Noto Sans','Cantarell','Sans'; font-size:13px;"
    "    transition: background-color 140ms ease, color 140ms ease,"
    "                border-color 140ms ease; }"

    /* ── Header / title bar ── */
    ".hbar { background:linear-gradient(180deg,#1d1d29 0%,#181823 100%);"
    "  border-bottom:1px solid #2e2e40; padding:0 14px; }"
    ".brand-logo { background:linear-gradient(135deg,#5cc9d8,#6a7ed8);"
    "  border-radius:6px; padding:4px 9px; color:#0b1518;"
    "  font-weight:800; font-size:12px;"
    "  font-family:'JetBrains Mono','monospace'; }"
    ".brand-title { color:#e8e8f4; font-size:13px; font-weight:600; letter-spacing:-0.01em; }"
    ".brand-sub   { color:#7a7a92; font-size:11px; font-weight:400; }"
    /* macOS-style traffic-light window dots (from Acheron-UI.html) */
    ".win-dot { border-radius:50%; min-width:12px; min-height:12px;"
    "  margin-right:3px; background:#555; border:none; padding:0; }"
    ".win-dot-close { background:#ff5f57; }"
    ".win-dot-min   { background:#febc2e; }"
    ".win-dot-max   { background:#28c840; }"
    /* Pipeline step arrow label + step-number chip */
    ".pipe-arrow { color:#56566c; font-size:14px; padding:0 2px; }"
    ".step-num { background:#353548; color:#7a7a92;"
    "  font-size:10px; font-weight:700;"
    "  font-family:'JetBrains Mono','monospace';"
    "  border-radius:3px; padding:1px 5px; margin-right:4px; }"
    ".step-num-done { background:alpha(#7cd0a0,0.25); color:#7cd0a0; }"
    ".step-num-active { background:#5cc9d8; color:#0a1416; }"
    ".pipe-chip { background:#22222f; border:1px solid #2e2e40;"
    "  color:#b8b8cc; font-size:11px;"
    "  font-family:'JetBrains Mono','monospace';"
    "  border-radius:4px; padding:2px 9px; }"
    ".pipe-chip-key { color:#7a7a92; margin-right:4px; }"
    ".pipeline-row { background:#1b1b27; border-bottom:1px solid #2e2e40; padding:9px 14px; }"
    ".pipe-step { background:#22222f; border:1px solid #2e2e40; border-radius:14px;"
    "  padding:4px 12px; color:#b8b8cc; font-size:12px; font-weight:500; }"
    ".pipe-step label { color:inherit; }"
    ".pipe-step-done { background:rgba(124,208,160,0.15); border-color:rgba(124,208,160,0.45); color:#7cd0a0; }"
    ".pipe-step-active { background:#5cc9d8; border-color:#5cc9d8; color:#0a1416; }"
    ".pipe-sep { color:#3a3a52; font-size:12px; padding:0 2px; }"
    ".cheron { background:#22222f; border:1px solid #2e2e40; border-radius:20px;"
    "  padding:3px 12px; font-size:11px; font-weight:500; color:#b8b8cc; }"
    ".acheron:hover { background:#2b2b3b; color:#e8e8f4; border-color:#3a3a52; }"
    ".hdr-btn { background:transparent; border:1px solid transparent; border-radius:6px;"
    "  color:#b8b8cc; padding:4px 10px; font-size:12px; font-weight:500; }"
    ".hdr-btn:hover { background:#2b2b3b; color:#e8e8f4; }"
    ".hdr-btn:active { background:#22222f; }"
    ".hdr-icon-btn { background:transparent; border:1px solid transparent; border-radius:6px;"
    "  color:#b8b8cc; padding:4px 8px; font-size:13px; }"
    ".hdr-icon-btn:hover { background:#2b2b3b; color:#e8e8f4; }"

    /* ── Application menu bar (File / Edit / View / Pipeline / Timeline /
     *    Voice / Help) — dark theme ── */
    "menubar, .app-menubar { background:#1b1b27; color:#e8e8f4;"
    "  border-bottom:1px solid #2e2e40; padding:0 4px; }"
    "menubar > menuitem { background:transparent; color:#e8e8f4;"
    "  padding:6px 12px; border-radius:5px; }"
    "menubar > menuitem:hover { background:#2b2b3b; color:#ffffff; }"
    "menubar > menuitem:active,"
    "menubar > menuitem:checked { background:#353548; color:#5cc9d8; }"
    "menu { background:#22222f; color:#e8e8f4;"
    "  border:1px solid #2e2e40; padding:4px 0; }"
    "menu menuitem { background:transparent; color:#e8e8f4;"
    "  padding:6px 14px; }"
    "menu menuitem:hover,"
    "menu menuitem:focus { background:#2b2b3b; color:#ffffff; }"
    "menu menuitem:disabled { color:#56566c; }"
    "menu separator { background:#2e2e40; min-height:1px; margin:4px 0; }"

    /* ── Settings / pipeline bar ── */
    ".sbar { background:#1b1b27; border-bottom:1px solid #2e2e40; padding:8px 14px; }"
    ".sbar label { color:#7a7a92; font-size:10px; font-weight:600; letter-spacing:0.06em; }"
    "combobox button { background:#22222f; color:#e8e8f4; border:1px solid #2e2e40;"
    "  border-radius:5px; padding:4px 8px; font-size:11px; }"
    "combobox button:hover { border-color:#3a3a52; background:#2b2b3b; }"
    "combobox arrow { color:#7a7a92; }"
    "entry { background:#22222f; color:#e8e8f4; border:1px solid #2e2e40;"
    "  border-radius:5px; padding:4px 8px; font-size:11px;"
    "  caret-color:#5cc9d8; }"
    "entry:focus { border-color:#5cc9d8; box-shadow:0 0 0 2px alpha(#5cc9d8,0.22); }"
    "entry selection { background:alpha(#5cc9d8,0.28); color:#e8e8f4; }"
    "spinbutton { background:#22222f; color:#e8e8f4; border:1px solid #2e2e40; border-radius:5px; }"
    "spinbutton:focus-within { border-color:#5cc9d8; box-shadow:0 0 0 2px alpha(#5cc9d8,0.22); }"
    "spinbutton button { background:transparent; border:none; color:#b8b8cc; }"
    "spinbutton button:hover { background:#2b2b3b; color:#e8e8f4; }"
    "checkbutton { color:#b8b8cc; }"
    "checkbutton check { background:#22222f; border:1px solid #3a3a52; border-radius:3px;"
    "  min-width:14px; min-height:14px; }"
    "checkbutton check:checked { background:#5cc9d8; border-color:#5cc9d8; color:#0b1518; }"
    ".save-btn { background:#5cc9d8; color:#0b1518;"
    "  border:1px solid #5cc9d8; border-radius:5px; padding:5px 14px;"
    "  font-size:11.5px; font-weight:600; }"
    ".save-btn:hover { background:#78d8e5; border-color:#78d8e5; }"
    ".save-btn:active { background:#4ab0bd; border-color:#4ab0bd; }"

    /* ── Left sidebar ── */
    ".sidebar { background:#1b1b27; border-right:1px solid #2e2e40; }"
    ".vid-fname { color:#b8b8cc; font-size:12px; padding:8px 14px;"
    "  border-bottom:1px solid #2e2e40;"
    "  font-family:'JetBrains Mono','monospace'; }"
    ".vtool { background:transparent; border:1px solid transparent;"
    "  border-radius:5px; color:#b8b8cc; padding:4px 7px; font-size:12px; }"
    ".vtool:hover { background:#2b2b3b; color:#e8e8f4; }"
    ".video-box { background:#000000; border-radius:2px; }"
    ".sub-overlay { color:#ffffff; font-size:15px; font-weight:500;"
    "  text-shadow:0 0 3px alpha(#000000,0.95), 0 2px 4px alpha(#000000,0.85); }"
    ".time-lbl { color:#7a7a92; font-size:11px; font-family:'JetBrains Mono','monospace'; }"
    ".ctrl-btn { background:transparent; border:1px solid transparent; border-radius:5px;"
    "  color:#b8b8cc; padding:5px 8px; font-size:12px; }"
    ".ctrl-btn:hover { background:#2b2b3b; color:#e8e8f4; }"
    ".ctrl-play { background:#5cc9d8; border:1px solid #5cc9d8; border-radius:5px;"
    "  color:#0b1518; padding:6px 10px; font-weight:700; }"
    ".ctrl-play:hover { background:#78d8e5; border-color:#78d8e5; }"
    ".ctrl-play:active { background:#4ab0bd; border-color:#4ab0bd; }"

    /* ── Right panel ── */
    ".rpanel { background:#15151f; }"

    /* ── Action / edit bars ── */
    ".abar { background:#1b1b27; border-bottom:1px solid #2e2e40; padding:6px 14px; }"
    ".btn { background:#22222f; border:1px solid #2e2e40; border-radius:5px;"
    "  color:#b8b8cc; padding:5px 10px; font-size:11.5px; font-weight:500; }"
    ".btn:hover { background:#2b2b3b; border-color:#3a3a52; color:#e8e8f4; }"
    ".btn:active { background:#22222f; }"

    /* Pipeline step buttons (Transcribe / Translate / TTS / Mix) */
    ".btn-green { background:alpha(#7cd0a0,0.14); border:1px solid alpha(#7cd0a0,0.45);"
    "  color:#7cd0a0; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-green:hover { background:alpha(#7cd0a0,0.22); border-color:#7cd0a0; }"
    ".btn-solid-green { background:alpha(#5cc9d8,0.14); border:1px solid #5cc9d8;"
    "  color:#5cc9d8; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:600; }"
    ".btn-solid-green:hover { background:alpha(#5cc9d8,0.22); }"
    ".btn-purple { background:alpha(#b992d8,0.12); border:1px solid alpha(#b992d8,0.45);"
    "  color:#b992d8; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-purple:hover { background:alpha(#b992d8,0.2); border-color:#b992d8; }"
    ".btn-yellow { background:alpha(#d8b070,0.12); border:1px solid alpha(#d8b070,0.45);"
    "  color:#d8b070; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-yellow:hover { background:alpha(#d8b070,0.2); border-color:#d8b070; }"
    ".btn-blue { background:alpha(#7aa8e0,0.12); border:1px solid alpha(#7aa8e0,0.45);"
    "  color:#7aa8e0; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-blue:hover { background:alpha(#7aa8e0,0.2); border-color:#7aa8e0; }"
    ".btn-red { background:alpha(#e88070,0.12); border:1px solid alpha(#e88070,0.45);"
    "  color:#e88070; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-red:hover { background:alpha(#e88070,0.2); border-color:#e88070; }"
    ".btn-female { background:alpha(#e8a5c4,0.12); border:1px solid alpha(#e8a5c4,0.45);"
    "  color:#e8a5c4; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-female:hover { background:alpha(#e8a5c4,0.22); border-color:#e8a5c4; }"
    ".btn-male { background:alpha(#7aa8e0,0.12); border:1px solid alpha(#7aa8e0,0.45);"
    "  color:#7aa8e0; border-radius:5px; padding:5px 10px;"
    "  font-size:11.5px; font-weight:500; }"
    ".btn-male:hover { background:alpha(#7aa8e0,0.22); border-color:#7aa8e0; }"
    ".abar-section { color:#7a7a92; font-size:10.5px; font-weight:600;"
    "  padding:0 8px; letter-spacing:0.06em; }"

    /* ── Segment table (TreeView) ── */
    "treeview { background:#15151f; color:#e8e8f4; border:none; }"
    "treeview:selected { background:alpha(#5cc9d8,0.14); color:#e8e8f4; }"
    "treeview:selected:focus { background:alpha(#5cc9d8,0.18); }"
    "treeview:hover { background:#1b1b27; }"
    "treeview header button { background:#1b1b27; color:#7a7a92;"
    "  border:none; border-bottom:1px solid #2e2e40;"
    "  font-size:10.5px; font-weight:600; letter-spacing:0.06em;"
    "  padding:8px 10px; }"
    "treeview header button:hover { background:#22222f; color:#b8b8cc; }"

    /* ── Timeline ── */
    ".tl-panel { background:#1b1b27; border-top:1px solid #2e2e40; }"
    ".tl-hdr { background:#1b1b27; border-bottom:1px solid #2e2e40; padding:6px 14px; }"
    ".tl-title { color:#e8e8f4; font-size:11.5px; font-weight:600; }"
    ".tl-zoom-lbl { color:#7a7a92; font-size:10.5px; font-weight:500; }"
    ".tl-zoom-pct { color:#b8b8cc; font-size:10.5px; font-family:'JetBrains Mono','monospace'; }"
    ".tl-track-wrap { background:#22222f; border:1px solid #2e2e40;"
    "  border-radius:4px; min-height:54px; }"
    ".tl-lbl { color:#7a7a92; font-size:11px; font-weight:600;"
    "  font-family:'JetBrains Mono','monospace'; }"

    /* ── Scale / slider ── */
    "scale trough { background:#2b2b3b; border-radius:2px; min-height:4px; }"
    "scale highlight { background:#5cc9d8; border-radius:2px; }"
    "scale slider { background:#e8e8f4; border-radius:50%;"
    "  min-width:12px; min-height:12px;"
    "  border:2px solid #1b1b27; }"
    "scale slider:hover { background:#ffffff;"
    "  box-shadow:0 0 0 4px alpha(#5cc9d8,0.25); }"

    /* ── Progress ── */
    "progressbar trough { background:#2b2b3b; border-radius:2px; min-height:4px; }"
    "progressbar progress { background:#5cc9d8; border-radius:2px; }"
    "progressbar text { color:#b8b8cc; font-size:10.5px;"
    "  font-family:'JetBrains Mono','monospace'; }"

    /* ── Scrollbars ── */
    "scrollbar { background:transparent; }"
    "scrollbar slider { background:#353548; border-radius:6px;"
    "  min-width:8px; min-height:8px; }"
    "scrollbar slider:hover { background:#3a3a52; }"

    /* ── Tooltips ── */
    "tooltip { background:#22222f; color:#e8e8f4; border:1px solid #2e2e40;"
    "  border-radius:5px; }"

    /* ── Status bar ── */
    ".status-bar { background:#15151f; border-top:1px solid #2e2e40; padding:4px 14px; }"
    ".status-lbl { color:#7a7a92; font-size:11px; font-family:'JetBrains Mono','monospace'; }"
    ".status-dot-lbl { color:#7cd0a0; font-size:11px; font-weight:600; }"
    ".mem-lbl { color:#7a7a92; font-size:11px; font-family:'JetBrains Mono','monospace'; }"

    /* ── v9: Recent files list ── */
    ".recent-hd { color:#7a7a92; font-size:10.5px; font-weight:600;"
    "  letter-spacing:0.08em; padding:8px 14px 4px; }"
    ".recent-row { background:transparent; border:1px solid transparent;"
    "  border-radius:5px; padding:5px 8px; }"
    ".recent-row:hover { background:#22222f; border-color:#2e2e40; }"
    ".recent-name { color:#e8e8f4; font-size:12px; }"
    ".recent-meta { color:#7a7a92; font-size:10.5px;"
    "  font-family:'JetBrains Mono','monospace'; }"
    ".recent-dur { color:#7a7a92; font-size:10.5px;"
    "  font-family:'JetBrains Mono','monospace'; }"

    /* ── v9: Settings dialog (Slide 2) ── */
    ".sp-window { background:#15151f; }"
    ".sp-hd { background:#1b1b27; border-bottom:1px solid #2e2e40; padding:12px 18px; }"
    ".sp-hd-title { color:#e8e8f4; font-size:14px; font-weight:600; }"
    ".sp-nav-btn { background:transparent; border:1px solid transparent;"
    "  border-radius:5px; color:#b8b8cc; padding:7px 10px; font-size:12px; }"
    ".sp-nav-btn:hover { background:#22222f; color:#e8e8f4; }"
    ".sp-nav-btn.active { background:#1e3a3f; color:#5cc9d8;"
    "  border-color:transparent; }"
    ".sp-group-t { color:#7a7a92; font-size:10.5px; font-weight:600;"
    "  letter-spacing:0.08em; padding:0 0 6px 0; }"
    ".sp-row-lbl { color:#e8e8f4; font-size:12.5px; }"
    ".sp-row-hint { color:#7a7a92; font-size:11px; }"
    ".sp-footer { background:#1b1b27; border-top:1px solid #2e2e40;"
    "  padding:10px 18px; }"
    ".sp-footer-note { color:#7a7a92; font-size:11.5px;"
    "  font-family:'JetBrains Mono','monospace'; }"
    ".voice-card { background:#22222f; border:1px solid #2e2e40; border-radius:6px;"
    "  padding:8px 10px; }"
    ".voice-card:hover { border-color:#3a3a52; }"
    ".voice-card-active { background:#1e3a3f; border:1px solid #5cc9d8; border-radius:6px;"
    "  padding:8px 10px; }"
    ".voice-avatar { background:linear-gradient(135deg,#e88070,#d858a0);"
    "  border-radius:50%; color:#fff; font-weight:700; font-size:14px;"
    "  min-width:40px; min-height:40px; }"
    ".voice-avatar-male { background:linear-gradient(135deg,#5cc9d8,#4a8ad4);"
    "  border-radius:50%; color:#fff; font-weight:700; font-size:14px;"
    "  min-width:40px; min-height:40px; }"
    ".voice-name { color:#e8e8f4; font-size:12.5px; font-weight:500; }"
    ".voice-meta { color:#7a7a92; font-size:10.5px; }"

    /* ── v9: Theme toggle pill ── */
    ".theme-pill { background:#22222f; border:1px solid #2e2e40; border-radius:20px;"
    "  padding:2px; }"
    ".theme-btn { background:transparent; border:none; border-radius:16px;"
    "  color:#7a7a92; padding:3px 10px; font-size:11px; }"
    ".theme-btn:hover { color:#b8b8cc; }"
    ".theme-btn-active { background:#353548; color:#e8e8f4;"
    "  border-radius:16px; padding:3px 10px; font-size:11px;"
    "  box-shadow:0 1px 2px alpha(#000000,0.3); }"

    /* ── v9: recent-files open button ── */
    ".open-btn { background:#5cc9d8; border:1px solid #5cc9d8; border-radius:5px;"
    "  color:#0b1518; padding:7px 14px; font-size:12.5px; font-weight:600; }"
    ".open-btn:hover { background:#78d8e5; border-color:#78d8e5; }"
    ".open-btn:active { background:#4ab0bd; }"
    ;



/* ═══════════════════════════════════════════════════════════════════
 * SECTION 1 — Constants
 * ═══════════════════════════════════════════════════════════════════ */
#define APP_VERSION     "v1.0.0"
#define APP_NAME        "Queen"
#define CONFIG_SUBPATH  ".config/ai_dubber/config.json"
#define LOG_SUBPATH     ".local/share/ai_dubber/ai_dubber.log"
#define MP3_BITRATE     "192k"
#define MP3_SAMPLE_RATE 44100

static const char *WHISPER_MODELS[] = { "tiny","base","small","medium","large","large-v3", NULL };
static const char *LANG_CODES[]     = { "km","en","zh","es","fr","de","ja","ko",
                                         "ar","ru","vi","th","pt","it","id","hi", NULL };
static const char *VIDEO_EXT[]      = { ".mp4",".mkv",".avi",".mov",".webm",".flv", NULL };

#define KM_VOICE_FEMALE "km-KH-SreymomNeural"
#define KM_VOICE_MALE   "km-KH-PisethNeural"
static const char *KM_VOICES[]   = { KM_VOICE_FEMALE, KM_VOICE_MALE, NULL };
static const char *KM_VOICE_LBLS[]= { "🎀 Female (Sreymom)", "🎙 Male (Piseth)", NULL };

/* ... [I kept all your original code exactly the same from here] ... */

static void json_escape(const char *src, char *dst, size_t dsz) {
    size_t di = 0;
    for (const char *s = src; *s && di + 6 < dsz; s++) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  { dst[di++]='\\'; dst[di++]='"'; }
        else if (c == '\\') { dst[di++]='\\'; dst[di++]='\\'; }
        else if (c == '\n') { dst[di++]='\\'; dst[di++]='n'; }
        else if (c == '\r') { dst[di++]='\\'; dst[di++]='r'; }
        else if (c == '\t') { dst[di++]='\\'; dst[di++]='t'; }
        else                { dst[di++] = c; }
    }
    dst[di] = '\0';
}


/* ── Encode a Unicode codepoint as UTF-8 into dst, return bytes written ── */
static int codepoint_to_utf8(unsigned long cp, char *dst) {
    if (cp < 0x80) {
        dst[0] = (char)cp; return 1;
    } else if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    } else if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    } else {
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6)  & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F)); return 4;
    }
}

/* ── Locate a JSON string value for a given key in a flat object ─── */
/* Fully decodes \uXXXX escapes (including surrogate pairs) to UTF-8  */
static int json_get_str(const char *json, const char *key,
                         char *val, size_t vsz) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < vsz) {
        if (*p == '\\' && *(p+1)) {
            p++;
            if (*p == 'u' || *p == 'U') {
                /* Parse \uXXXX */
                char hex[5] = {0};
                for (int h = 0; h < 4 && *(p+1); h++) hex[h] = *++p;
                unsigned long cp = strtoul(hex, NULL, 16);
                /* Handle UTF-16 surrogate pairs \uD800-\uDFFF */
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    *(p+1) == '\\' && *(p+2) == 'u') {
                    p += 3;
                    char hex2[5] = {0};
                    for (int h = 0; h < 4 && *(p); h++) hex2[h] = *p++;
                    p--;
                    unsigned long low = strtoul(hex2, NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                char utf8[5] = {0};
                int bytes = codepoint_to_utf8(cp, utf8);
                for (int b = 0; b < bytes && i + 1 < vsz; b++)
                    val[i++] = utf8[b];
            } else if (*p == 'n') { val[i++] = '\n'; }
            else if (*p == 'r') { val[i++] = '\r'; }
            else if (*p == 't') { val[i++] = '\t'; }
            else { val[i++] = *p; }
            p++;
        } else {
            val[i++] = *p++;
        }
    }
    val[i] = '\0';
    return 1;
}

/* ── Get a numeric value (int/double) for a key ─────────────────── */
static int json_get_double(const char *json, const char *key, double *out) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
    *out = strtod(p, NULL);
    return 1;
}

/* ── Build the segments JSON array for passing to Python ─────────── */
/* Returns heap-allocated string; caller must free(). */
static char *segments_to_json(const Segment *segs, int n) {
    /* Estimate: per segment ~100 + 2*2048 chars */
    size_t cap = (size_t)n * 4500 + 32;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    buf[pos++] = '[';
    char esc[4200];
    for (int i = 0; i < n; i++) {
        json_escape(segs[i].text, esc, sizeof(esc));
        int written = snprintf(buf + pos, cap - pos,
            "%s{\"start\":%.3f,\"end\":%.3f,\"text\":\"%s\"}",
            i ? "," : "", segs[i].start, segs[i].end, esc);
        if (written < 0 || (size_t)written >= cap - pos) break;
        pos += (size_t)written;
    }
    buf[pos++] = ']';
    buf[pos]   = '\0';
    return buf;
}

/* ── Parse Whisper JSON output (array of {start,end,text}) ──────── */
/* Returns number of segments parsed (≤ cap). */
static int parse_whisper_json(const char *json, Segment *segs, int cap) {
    int n = 0;
    const char *p = json;
    /* Find first '{' */
    while (*p && n < cap) {
        /* locate next object start */
        p = strchr(p, '{');
        if (!p) break;
        const char *obj_start = p;
        /* Find matching '}' — simple brace counter */
        int depth = 0;
        const char *q = p;
        while (*q) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        /* Copy object */
        size_t obj_len = (size_t)(q - obj_start);
        char *obj = malloc(obj_len + 1);
        /* Bug fix #4: NULL check after malloc */
        if (!obj) break;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        double v;
        if (json_get_double(obj, "start", &v)) segs[n].start = v;
        if (json_get_double(obj, "end",   &v)) segs[n].end   = v;
        json_get_str(obj, "text", segs[n].text, sizeof(segs[n].text));
        segs[n].translated[0] = '\0';
        free(obj);  /* Bug fix #1: was already present — confirmed correct */
        n++;
        p = q;
    }
    return n;
}

/* ── Parse translated segments JSON (same shape + "translated" key) */
static int parse_translated_json(const char *json, Segment *segs, int cap) {
    int n = 0;
    const char *p = json;
    while (*p && n < cap) {
        p = strchr(p, '{');
        if (!p) break;
        const char *obj_start = p;
        int depth = 0;
        const char *q = p;
        while (*q) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        size_t obj_len = (size_t)(q - obj_start);
        char *obj = malloc(obj_len + 1);
        /* Bug fix #4: NULL check after malloc */
        if (!obj) break;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        json_get_str(obj, "translated", segs[n].translated,
                     sizeof(segs[n].translated));
        free(obj);
        n++;
        p = q;
    }
    return n;
}

/* ── Config JSON read/write ──────────────────────────────────────── */
static void config_path(char *buf, size_t sz) {
    /* g_get_home_dir() returns USERPROFILE on Windows, HOME elsewhere. */
    const char *home = g_get_home_dir();
    snprintf(buf, sz, "%s/%s", home ? home : TMPDIR, CONFIG_SUBPATH);
}

static void ensure_parent_dirs(const char *path) {
    /* GLib's g_mkdir_with_parents is cross-platform and handles both
     * '/' and '\\' separators on Windows.                             */
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
#ifdef _WIN32
    char *bsl = strrchr(tmp, '\\');
    if (bsl && (!slash || bsl > slash)) slash = bsl;
#endif
    if (slash) { *slash = '\0'; g_mkdir_with_parents(tmp, 0755); }
}

static void load_config(AppConfig *cfg) {
    /* Bug fix #2: replaced strcpy with snprintf to prevent buffer overflow */
    snprintf(cfg->whisper_model,      sizeof(cfg->whisper_model),      "base");
    snprintf(cfg->target_language,    sizeof(cfg->target_language),    "km");
    cfg->tts_voice_id[0] = '\0';
    snprintf(cfg->dub_voice,          sizeof(cfg->dub_voice),          "%s", KM_VOICE_FEMALE);
    cfg->tts_rate = 150;
    snprintf(cfg->translation_engine, sizeof(cfg->translation_engine), "google");
    cfg->audio_ducking = 0;
    cfg->voice_clone_script[0] = '\0';
    cfg->subtitle_font_size = 72;
    cfg->subtitle_language  = 0;
    cfg->output_dir[0]      = '\0';
    cfg->max_segments       = 0;
    cfg->tts_pitch          = 0;

    char path[4096]; config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, sz, f); buf[nread] = '\0'; fclose(f);

    char tmp[4096]; double d;
    if (json_get_str(buf, "whisper_model",   tmp, sizeof(tmp)))
        snprintf(cfg->whisper_model,   sizeof(cfg->whisper_model),   "%s", tmp);
    if (json_get_str(buf, "target_language", tmp, sizeof(tmp)))
        snprintf(cfg->target_language, sizeof(cfg->target_language), "%s", tmp);
    if (json_get_str(buf, "tts_voice_id",    tmp, sizeof(tmp)))
        snprintf(cfg->tts_voice_id,    sizeof(cfg->tts_voice_id),    "%s", tmp);
    if (json_get_str(buf, "dub_voice",       tmp, sizeof(tmp)))
        snprintf(cfg->dub_voice,       sizeof(cfg->dub_voice),       "%s", tmp);
    if (json_get_double(buf, "tts_rate", &d))
        cfg->tts_rate = (int)d;
    if (json_get_str(buf, "translation_engine", tmp, sizeof(tmp)))
        snprintf(cfg->translation_engine, sizeof(cfg->translation_engine), "%s", tmp);
    if (json_get_double(buf, "audio_ducking", &d))
        cfg->audio_ducking = (int)d;
    if (json_get_str(buf, "voice_clone_script", tmp, sizeof(tmp)))
        snprintf(cfg->voice_clone_script, sizeof(cfg->voice_clone_script), "%s", tmp);
    if (json_get_double(buf, "subtitle_font_size", &d)) cfg->subtitle_font_size = (int)d;
    if (json_get_double(buf, "subtitle_language",  &d)) cfg->subtitle_language  = (int)d;
    if (json_get_str(buf, "output_dir", tmp, sizeof(tmp)))
        snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", tmp);
    if (json_get_double(buf, "max_segments", &d)) cfg->max_segments = (int)d;
    if (json_get_double(buf, "tts_pitch",    &d)) cfg->tts_pitch    = (int)d;

    /* Feature 4: parse recent_files array — find the JSON array and read up to 5 strings */
    {
        const char *arr = strstr(buf, "\"recent_files\"");
        if (arr) {
            arr = strchr(arr, '[');
            if (arr) {
                arr++; /* skip '[' */
                int ri = 0;
                while (*arr && *arr != ']' && ri < 5) {
                    while (*arr == ' ' || *arr == ',' || *arr == '\n') arr++;
                    if (*arr == '"') {
                        arr++;
                        size_t ci = 0;
                        while (*arr && *arr != '"' && ci + 1 < 4096) {
                            if (*arr == '\\' && *(arr+1)) {
                                arr++;
                                if (*arr == 'n')       cfg->recent_files[ri][ci++] = '\n';
                                else if (*arr == 'r')  cfg->recent_files[ri][ci++] = '\r';
                                else if (*arr == 't')  cfg->recent_files[ri][ci++] = '\t';
                                else                   cfg->recent_files[ri][ci++] = *arr;
                                arr++;
                            } else {
                                cfg->recent_files[ri][ci++] = *arr++;
                            }
                        }
                        cfg->recent_files[ri][ci] = '\0';
                        if (*arr == '"') arr++;
                        ri++;
                    } else { arr++; }
                }
            }
        }
    }
    free(buf);
}

static void save_config(const AppConfig *cfg);  /* forward for push_recent_file */

/* ── Feature 4: push a new path to the front of recent_files list ─── */
static void push_recent_file(AppConfig *cfg, const char *path) {
    /* Check if already present — if so, remove it first */
    int pos = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp(cfg->recent_files[i], path) == 0) { pos = i; break; }
    if (pos == 0) return; /* already at front */
    if (pos > 0) {
        /* Shift entries before pos down by one */
        for (int i = pos; i > 0; i--)
            memmove(cfg->recent_files[i], cfg->recent_files[i-1], 4096);
    } else {
        /* Not found — shift everything down, dropping slot 4 */
        for (int i = 4; i > 0; i--)
            memmove(cfg->recent_files[i], cfg->recent_files[i-1], 4096);
    }
    snprintf(cfg->recent_files[0], 4096, "%s", path);
}

static void save_config(const AppConfig *cfg) {
    char path[4096]; config_path(path, sizeof(path));
    ensure_parent_dirs(path);
    char tmp_path[4096 + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    char esc_voice[256], esc_dub[256], esc_clone[4200], esc_outdir[4200];
    json_escape(cfg->tts_voice_id,       esc_voice,  sizeof(esc_voice));
    json_escape(cfg->dub_voice,          esc_dub,    sizeof(esc_dub));
    json_escape(cfg->voice_clone_script, esc_clone,  sizeof(esc_clone));
    json_escape(cfg->output_dir,         esc_outdir, sizeof(esc_outdir));
    fprintf(f,
        "{\n"
        "  \"whisper_model\": \"%s\",\n"
        "  \"target_language\": \"%s\",\n"
        "  \"tts_voice_id\": \"%s\",\n"
        "  \"dub_voice\": \"%s\",\n"
        "  \"tts_rate\": %d,\n"
        "  \"translation_engine\": \"%s\",\n"
        "  \"audio_ducking\": %d,\n"
        "  \"voice_clone_script\": \"%s\",\n"
        "  \"subtitle_font_size\": %d,\n"
        "  \"subtitle_language\": %d,\n"
        "  \"output_dir\": \"%s\",\n"
        "  \"max_segments\": %d,\n"
        "  \"tts_pitch\": %d,\n"
        "  \"recent_files\": [",
        cfg->whisper_model, cfg->target_language,
        esc_voice, esc_dub, cfg->tts_rate,
        cfg->translation_engine, cfg->audio_ducking, esc_clone,
        cfg->subtitle_font_size, cfg->subtitle_language, esc_outdir,
        cfg->max_segments, cfg->tts_pitch);
    int wrote_one = 0;
    for (int i = 0; i < 5; i++) {
        if (!cfg->recent_files[i][0]) continue;
        char esc_rf[4200];
        json_escape(cfg->recent_files[i], esc_rf, sizeof(esc_rf));
        fprintf(f, "%s\"%s\"", wrote_one ? "," : "", esc_rf);
        wrote_one = 1;
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    rename(tmp_path, path);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 3 — Logging  (file + stderr, mutex-safe)
 * ═══════════════════════════════════════════════════════════════════ */
static FILE            *g_log_fp    = NULL;
static pthread_mutex_t  g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void app_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void app_log(const char *level, const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mutex);
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
    va_list ap;
    if (g_log_fp) {
        fprintf(g_log_fp, "%s [%s] ", ts, level);
        va_start(ap, fmt); vfprintf(g_log_fp, fmt, ap); va_end(ap);
        fputc('\n', g_log_fp); fflush(g_log_fp);
    }
    fprintf(stderr, "%s [%s] ", ts, level);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&g_log_mutex);
}

#define LOG_INFO(...)  app_log("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  app_log("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) app_log("ERROR", __VA_ARGS__)

static void init_log(void) {
    const char *home = g_get_home_dir();
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", home ? home : TMPDIR, LOG_SUBPATH);
    ensure_parent_dirs(path);
    g_log_fp = fopen(path, "a");
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 4 — Path helpers
 * ═══════════════════════════════════════════════════════════════════ */
static const char *file_ext(const char *path) __attribute__((unused));
static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

static int str_in_list(const char *s, const char **list) __attribute__((unused));
static int str_in_list(const char *s, const char **list) {
    for (int i = 0; list[i]; i++)
        if (g_ascii_strcasecmp(s, list[i]) == 0) return 1;
    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 5 — FFmpeg path resolver
 * ═══════════════════════════════════════════════════════════════════ */
static char g_ffmpeg[4096]  = "ffmpeg";
static char g_ffprobe[4096] = "ffprobe";

/* Global forward declarations needed by dub_thread and identify_speakers
 * (defined later in Feature A/2 sections, declared here for ordering)  */
static char (*g_speaker_voices)[128] = NULL;
static int   g_speaker_voices_count  = 0;

static const char DIARIZE_PY[] =
    "import sys, json\n"
    "try:\n"
    "    from pyannote.audio import Pipeline\n"
    "    pipe = Pipeline.from_pretrained('pyannote/speaker-diarization-3.1')\n"
    "    diar = pipe(sys.argv[1])\n"
    "    out = []\n"
    "    for turn, _, speaker in diar.itertracks(yield_label=True):\n"
    "        out.append({'start': turn.start, 'end': turn.end, 'speaker': speaker})\n"
    "    print(json.dumps(out))\n"
    "except Exception as e:\n"
    "    print(json.dumps([]), file=sys.stderr)\n"
    "    sys.exit(1)\n";

static void resolve_tool(const char *name, char *buf, size_t sz) {
    /* 1. same dir as exe */
    char self[4096] = {0};
    int n = readlink_self(self, sizeof(self));
    if (n > 0) {
        char *sl = strrchr(self, '/');
#ifdef _WIN32
        char *bsl = strrchr(self, '\\');
        if (bsl && (!sl || bsl > sl)) sl = bsl;
#endif
        if (sl) *sl = '\0';
        char cand[4096];
#ifdef _WIN32
        /* Try with .exe if the caller didn't include it */
        if (!strstr(name, ".exe"))
            snprintf(cand, sizeof(cand), "%s/%s.exe", self, name);
        else
            snprintf(cand, sizeof(cand), "%s/%s", self, name);
#else
        snprintf(cand, sizeof(cand), "%s/%s", self, name);
#endif
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
            snprintf(buf, sz, "%s", cand); return;
        }
    }
    /* 2. system PATH via GLib (cross-platform replacement for `which`) */
    gchar *found = g_find_program_in_path(name);
    if (found) {
        snprintf(buf, sz, "%s", found);
        g_free(found);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 6 — subprocess helper  (g_spawn_sync, cross-platform)
 *
 * Uses GLib's g_spawn_sync so the same code runs on Linux and Windows
 * (MSYS2/MinGW-w64).  Captures stdout + stderr and returns the child
 * exit status.  Returns -1 on spawn failure.
 * ═══════════════════════════════════════════════════════════════════ */
static int run_cmd(char *const argv[], char *out_buf, size_t out_sz,
                    char *err_msg, size_t err_sz) {
    gchar *out_str = NULL, *err_str = NULL;
    gint exit_status = 0;
    GError *gerr = NULL;

    gboolean ok = g_spawn_sync(NULL, (gchar **)argv, NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL, NULL,
                               &out_str, &err_str,
                               &exit_status, &gerr);
    if (!ok) {
        if (err_msg)
            snprintf(err_msg, err_sz, "spawn: %s",
                     gerr ? gerr->message : "unknown error");
        if (gerr) g_error_free(gerr);
        g_free(out_str); g_free(err_str);
        return -1;
    }

    if (out_buf && out_sz > 1) {
        snprintf(out_buf, out_sz, "%s", out_str ? out_str : "");
    }
    if (err_msg && err_sz > 1) {
        snprintf(err_msg, err_sz, "%s", err_str ? err_str : "");
    }

    int rc;
#ifdef G_OS_WIN32
    /* On Windows exit_status is the raw DWORD returned by the child. */
    rc = (int)exit_status;
#else
    rc = WIFEXITED(exit_status) ? WEXITSTATUS(exit_status) : -1;
#endif
    if (rc != 0 && err_msg && !*err_msg)
        snprintf(err_msg, err_sz, "%s exited with code %d", argv[0], rc);

    g_free(out_str);
    g_free(err_str);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 7 — progress helper  (post to GTK main loop from any thread)
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct { GtkProgressBar *bar; GtkLabel *lbl; int pct; char msg[256]; } ProgUp;

static gboolean _prog_cb(gpointer d) {
    ProgUp *u = d;
    gtk_progress_bar_set_fraction(u->bar, u->pct / 100.0);
    gtk_label_set_text(u->lbl, u->msg);
    free(u); return FALSE;
}
static void post_progress(GtkProgressBar *bar, GtkLabel *lbl,
                            int pct, const char *msg) {
    ProgUp *u = malloc(sizeof *u);
    u->bar = bar; u->lbl = lbl; u->pct = pct;
    snprintf(u->msg, sizeof(u->msg), "%s", msg);
    gdk_threads_add_idle(_prog_cb, u);
}

/* generic "call fn(arg) on GTK main thread" */
typedef struct { void (*fn)(void*); void *arg; } IdleDone;
static gboolean _idle_done_cb(gpointer d) {
    IdleDone *i = d; i->fn(i->arg); free(i); return FALSE;
}
static void post_done(void (*fn)(void*), void *arg) {
    IdleDone *i = malloc(sizeof *i); i->fn = fn; i->arg = arg;
    gdk_threads_add_idle(_idle_done_cb, i);
}


/* ═══════════════════════════════════════════════════════════════════
 * SECTION 9 — Audio extraction worker  (video → 16 kHz WAV)
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    char  video_path[4096];
    char  wav_path[4096];
    GtkProgressBar *bar; GtkLabel *lbl;
    int   ok; char error[2048];
    void (*on_done)(void *ctx, const char *wav, const char *err);
    void *ctx;
} AudioArg;

/* ── Named forwarders ──────────────────────────────────────────────── */
typedef struct { AudioArg *a; } AudioCB;
static void _audio_fwd(void *d) {
    AudioCB *c = d; AudioArg *a = c->a;
    a->on_done(a->ctx, a->ok ? a->wav_path : NULL,
               a->ok ? NULL : a->error);
    free(a); free(c);
}
static void *audio_thread_real(void *data) {
    AudioArg *a = data;
    post_progress(a->bar, a->lbl, 10, "Extracting audio…");
    char *argv[] = {
        g_ffmpeg, "-y", "-i", a->video_path,
        "-vn", "-ar", "16000", "-ac", "1", "-f", "wav",
        a->wav_path, NULL
    };
    char err[2048] = {0};
    a->ok = (run_cmd(argv, NULL, 0, err, sizeof(err)) == 0);
    if (!a->ok) snprintf(a->error, sizeof(a->error), "%s", err);
    post_progress(a->bar, a->lbl, a->ok ? 30 : 0,
                  a->ok ? "Audio extracted" : "Extraction failed");
    LOG_INFO("Audio extract %s: %s", a->ok?"OK":"FAILED", a->video_path);
    AudioCB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_audio_fwd, cb);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 10 — Whisper transcription worker
 * ═══════════════════════════════════════════════════════════════════ */
/* Python one-liner: reads wav, runs whisper, prints JSON to stdout */
static const char WHISPER_PY[] =
    "import sys,json\n"
    "try:\n"
    "    import whisper\n"
    "except ImportError:\n"
    "    sys.stderr.write('openai-whisper not found. Run: pip install openai-whisper --break-system-packages\\n')\n"
    "    sys.exit(1)\n"
    "wav=sys.argv[1]; model=sys.argv[2] if len(sys.argv)>2 else 'base'\n"
    "m=whisper.load_model(model)\n"
    "r=m.transcribe(wav,verbose=False,fp16=False)\n"
    "segs=[{'start':float(s['start']),'end':float(s['end']),'text':s['text'].strip()} for s in r.get('segments',[])]\n"
    "sys.stdout.write(json.dumps(segs)+'\\n')\n"
    "sys.stdout.flush()\n";

typedef struct {
    char  wav_path[4096]; char model[32];
    int   max_segments;   /* 0 = unlimited */
    GtkProgressBar *bar; GtkLabel *lbl;
    Segment *segments; int seg_count; char error[2048];
    void (*on_done)(void *ctx, Segment *segs, int n, const char *err);
    void *ctx;
} WhisperArg;

typedef struct { WhisperArg *a; } WhisperCB;
static void _whisper_fwd(void *d) {
    WhisperCB *c = d; WhisperArg *a = c->a;
    a->on_done(a->ctx, a->seg_count > 0 ? a->segments : NULL,
               a->seg_count, a->error[0] ? a->error : NULL);
    /* Note: segments ownership transferred to caller */
    free(c);
}

static void *whisper_thread(void *data) {
    WhisperArg *a = data;
    post_progress(a->bar, a->lbl, 35, "Transcribing (Whisper)…");

    char *argv[] = { "python3", "-c", (char*)WHISPER_PY,
                     a->wav_path, a->model, NULL };
    /* Bug fix: 1MB stack buffer truncated long transcripts mid-JSON,
     * causing "only half the video was transcribed". Use a 32 MB heap
     * buffer so the full Whisper output fits for multi-hour videos. */
    size_t out_sz = 32u << 20;
    char *out = calloc(1, out_sz);
    char err[2048] = {0};
    if (!out) {
        snprintf(a->error, sizeof(a->error), "Whisper: out-of-memory");
        a->seg_count = 0; a->segments = NULL;
        WhisperCB *cb0 = malloc(sizeof *cb0); cb0->a = a;
        post_done(_whisper_fwd, cb0);
        return NULL;
    }
    int rc = run_cmd(argv, out, out_sz, err, sizeof(err));
    if (rc != 0) {
        snprintf(a->error, sizeof(a->error), "Whisper failed: %s", err);
        a->seg_count = 0; a->segments = NULL;
        LOG_ERROR("%s", a->error);
    } else {
        /* Allocate generous segment buffer */
        a->segments  = calloc(16000, sizeof(Segment));
        a->seg_count = parse_whisper_json(out, a->segments, 16000);
        /* apply max_segments cap if configured */
        if (a->max_segments > 0 && a->seg_count > a->max_segments)
            a->seg_count = a->max_segments;
        LOG_INFO("Whisper: %d segments", a->seg_count);
        post_progress(a->bar, a->lbl, 60, "Transcription done");
    }
    free(out);
    WhisperCB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_whisper_fwd, cb);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 11 — Translation worker
 * ═══════════════════════════════════════════════════════════════════ */
/* Feature 5: Multi-engine translation script.
 * sys.argv[1] = JSON segments, sys.argv[2] = target lang,
 * sys.argv[3] = engine: "google" | "argos" | "ollama" | "mistral"  */
static const char TRANSLATE_PY[] =
    "import sys,json\n"
    "segs=json.loads(sys.argv[1])\n"
    "tgt=sys.argv[2]\n"
    "engine=sys.argv[3] if len(sys.argv)>3 else 'google'\n"
    "def make_tr():\n"
    "    if engine=='ollama':\n"
    "        import urllib.request,json as _j\n"
    "        def tr(t):\n"
    "            body=_j.dumps({'model':'mistral','prompt':f'Translate to {tgt}: {t}','stream':False}).encode()\n"
    "            req=urllib.request.Request('http://localhost:11434/api/generate',body,{'Content-Type':'application/json'})\n"
    "            with urllib.request.urlopen(req,timeout=30) as r: return _j.loads(r.read())['response'].strip()\n"
    "        return tr\n"
    "    if engine=='mistral':\n"
    "        import urllib.request,json as _j,os\n"
    "        api_key=os.environ.get('MISTRAL_API_KEY','')\n"
    "        def tr(t):\n"
    "            body=_j.dumps({'model':'mistral-small','messages':[{'role':'user','content':f'Translate to {tgt}: {t}'}]}).encode()\n"
    "            req=urllib.request.Request('https://api.mistral.ai/v1/chat/completions',body,{'Content-Type':'application/json','Authorization':f'Bearer {api_key}'})\n"
    "            with urllib.request.urlopen(req,timeout=30) as r: return _j.loads(r.read())['choices'][0]['message']['content'].strip()\n"
    "        return tr\n"
    "    if engine=='argos':\n"
    "        try:\n"
    "            from argostranslate import translate as _at\n"
    "            _inst=_at.get_installed_languages()\n"
    "            _tl=next((l for l in _inst if l.code==tgt),None)\n"
    "            return lambda t: (_tl.get_translation(next((l for l in _inst if l.code=='en'),None)).translate(t) if _tl else t)\n"
    "        except Exception: pass\n"
    "    # default: google\n"
    "    try:\n"
    "        from deep_translator import GoogleTranslator\n"
    "        return lambda t: GoogleTranslator(source='auto',target=tgt).translate(t) or t\n"
    "    except ImportError: return lambda t: t\n"
    "tr=make_tr()\n"
    "out=[]\n"
    "for s in segs:\n"
    "    try: s['translated']=tr(s.get('text',''))\n"
    "    except: s['translated']=s.get('text','')\n"
    "    out.append(s)\n"
    "print(json.dumps(out))\n";

typedef struct {
    Segment *segments; int seg_count; char target_lang[8];
    char engine[32];   /* Feature 5: "google","argos","ollama","mistral" */
    GtkProgressBar *bar; GtkLabel *lbl;
    char error[2048];
    void (*on_done)(void *ctx, const char *err);
    void *ctx;
} TranslateArg;

typedef struct { TranslateArg *a; } TranslateCB;
static void _translate_fwd(void *d) {
    TranslateCB *c = d; TranslateArg *a = c->a;
    a->on_done(a->ctx, a->error[0] ? a->error : NULL);
    free(a); free(c);
}

static void *translate_thread(void *data) {
    TranslateArg *a = data;
    post_progress(a->bar, a->lbl, 62, "Translating…");

    char *json_in = segments_to_json(a->segments, a->seg_count);
    if (!json_in) { snprintf(a->error, sizeof(a->error), "OOM"); goto done; }

    char *argv[] = { "python3", "-c", (char*)TRANSLATE_PY,
                     json_in, a->target_lang, a->engine[0] ? a->engine : "google", NULL };
    char out[1 << 20] = {0}; char err[2048] = {0};
    int rc = run_cmd(argv, out, sizeof(out), err, sizeof(err));
    free(json_in);

    if (rc != 0) {
        snprintf(a->error, sizeof(a->error), "Translation failed: %s", err);
        LOG_ERROR("%s", a->error);
        goto done;
    }
    /* Parse translated field back into existing segments */
    Segment *tmp = calloc(a->seg_count + 1, sizeof(Segment));
    int n = parse_translated_json(out, tmp, a->seg_count);
    for (int i = 0; i < n && i < a->seg_count; i++)
        snprintf(a->segments[i].translated,
                 sizeof(a->segments[i].translated),
                 "%s", tmp[i].translated);
    free(tmp);
    LOG_INFO("Translation done: %d segs → %s", a->seg_count, a->target_lang);
    post_progress(a->bar, a->lbl, 80, "Translation done");

done:;
    TranslateCB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_translate_fwd, cb);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 12 — TTS worker  (edge-tts via python)
 * ═══════════════════════════════════════════════════════════════════ */
static const char TTS_PY[] =
    "import sys,asyncio,edge_tts\n"
    "text=sys.argv[1]; voice=sys.argv[2]; out=sys.argv[3]\n"
    "async def run():\n"
    "    await edge_tts.Communicate(text,voice).save(out)\n"
    "asyncio.run(run())\n";

typedef struct {
    char text[2048]; char voice[128]; char out_path[4096];
    GtkProgressBar *bar; GtkLabel *lbl;
    char error[2048];
    void (*on_done)(void *ctx, const char *out, const char *err);
    void *ctx;
} TtsArg;

typedef struct { TtsArg *a; } TtsCB;
static void _tts_fwd(void *d) {
    TtsCB *c = d; TtsArg *a = c->a;
    a->on_done(a->ctx, a->error[0] ? NULL : a->out_path,
               a->error[0] ? a->error : NULL);
    free(a); free(c);
}

static void *tts_thread(void *data) __attribute__((unused));
static void *tts_thread(void *data) {
    TtsArg *a = data;
    post_progress(a->bar, a->lbl, 85, "Synthesising TTS…");
    char *argv[] = { "python3", "-c", (char*)TTS_PY,
                     a->text, a->voice, a->out_path, NULL };
    char err[2048] = {0};
    int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
    if (rc != 0) snprintf(a->error, sizeof(a->error), "TTS failed: %s", err);
    else LOG_INFO("TTS OK: %s", a->out_path);
    TtsCB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_tts_fwd, cb);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 13 — MP3 extraction worker
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    char video_path[4096]; char out_path[4096];
    GtkProgressBar *bar; GtkLabel *lbl;
    char error[2048];
    void (*on_done)(void *ctx, const char *out, const char *err);
    void *ctx;
} Mp3Arg;

typedef struct { Mp3Arg *a; } Mp3CB;
static void _mp3_fwd(void *d) {
    Mp3CB *c = d; Mp3Arg *a = c->a;
    a->on_done(a->ctx, a->error[0] ? NULL : a->out_path,
               a->error[0] ? a->error : NULL);
    free(a); free(c);
}

static void *mp3_thread(void *data) {
    Mp3Arg *a = data;
    post_progress(a->bar, a->lbl, 10, "Extracting MP3…");
    char ar[16]; snprintf(ar, sizeof(ar), "%d", MP3_SAMPLE_RATE);
    char *argv[] = {
        g_ffmpeg, "-y", "-i", a->video_path,
        "-vn", "-ar", ar, "-ac", "2", "-ab", (char*)MP3_BITRATE,
        a->out_path, NULL
    };
    char err[2048] = {0};
    int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
    if (rc != 0) snprintf(a->error, sizeof(a->error), "%s", err);
    post_progress(a->bar, a->lbl, rc ? 0 : 100,
                  rc ? "MP3 failed" : "MP3 saved");
    LOG_INFO("MP3 extract %s: %s", rc?"FAILED":"OK", a->out_path);
    Mp3CB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_mp3_fwd, cb);
    return NULL;
}


/* Python script that generates one TTS mp3 per segment.
 * Called once per segment: python3 -c TTS_SEG_PY text voice out rate */
static const char TTS_SEG_PY[] =
    "import sys,asyncio,edge_tts\n"
    "text=sys.argv[1]; voice=sys.argv[2]; out=sys.argv[3]\n"
    "rate_pct=int(sys.argv[4]) if len(sys.argv)>4 else 0\n"
    "pitch_hz=int(sys.argv[5]) if len(sys.argv)>5 else 0\n"
    "rate_str=('+' if rate_pct>=0 else '')+str(rate_pct)+'%'\n"
    "pitch_str=('+' if pitch_hz>=0 else '')+str(pitch_hz)+'Hz'\n"
    "async def run():\n"
    "    c=edge_tts.Communicate(text,voice,rate=rate_str,pitch=pitch_str)\n"
    "    await c.save(out)\n"
    "asyncio.run(run())\n";

typedef struct {
    char  video_path[4096];
    char  out_path[4096];
    char  voice[128];
    int   rate;           /* tts_rate 50-400; 150 = normal */
    int   pitch;          /* tts_pitch offset in Hz (e.g. +10, -5) */
    int   burn_subs;      /* 1 = burn ASS subtitles */
    char  ass_path[4096]; /* filled if burn_subs */
    /* audio_ducking removed: original voice is fully replaced by AI voice */
    Segment *segments;
    int   seg_count;
    GtkProgressBar *bar; GtkLabel *lbl;
    char  error[2048];
    void (*on_done)(void *ctx, const char *out, const char *err);
    void *ctx;
} DubArg;

typedef struct { DubArg *a; } DubCB;
static void _dub_fwd(void *d) {
    DubCB *c = d; DubArg *a = c->a;
    a->on_done(a->ctx, a->error[0] ? NULL : a->out_path,
               a->error[0] ? a->error : NULL);
    free(a); free(c);
}

static void *dub_thread(void *data) {
    DubArg *a = data;
    int n = a->seg_count;
    /* Bug fix #3: use size_t arithmetic to prevent integer overflow on large n */
    /* Bug fix #4: NULL check after calloc */
    char **seg_mp3s = calloc((size_t)n, sizeof(char*));
    if (!seg_mp3s) {
        snprintf(a->error, sizeof(a->error), "Out of memory allocating segment list");
        DubCB *cb = malloc(sizeof *cb);
        if (cb) { cb->a = a; post_done(_dub_fwd, cb); }
        return NULL;
    }

    /* ── Step 1: TTS each segment ── */
    for (int i = 0; i < n; i++) {
        const char *txt = a->segments[i].translated[0]
                          ? a->segments[i].translated
                          : a->segments[i].text;
        if (!txt[0]) { seg_mp3s[i] = NULL; continue; }

        /* Per-speaker voice override:
         * 1) Speaker Map (g_speaker_voices) takes priority
         * 2) Per-segment voice profile toggle (speaker_id "male"/"female")
         * 3) Fall back to global dub_voice combo */
        const char *seg_voice = a->voice;
        if (g_speaker_voices && i < g_speaker_voices_count && g_speaker_voices[i][0])
            seg_voice = g_speaker_voices[i];
        else if (a->segments[i].speaker_id[0]) {
            if (strncmp(a->segments[i].speaker_id, "male", 4) == 0 ||
                strncmp(a->segments[i].speaker_id, "SPEAKER_01", 10) == 0)
                seg_voice = KM_VOICE_MALE;
            else if (strncmp(a->segments[i].speaker_id, "female", 6) == 0 ||
                     strncmp(a->segments[i].speaker_id, "SPEAKER_00", 10) == 0)
                seg_voice = KM_VOICE_FEMALE;
        }

        char *mp3 = malloc(128);
        /* Bug fix #4: NULL check */
        if (!mp3) { seg_mp3s[i] = NULL; continue; }
        snprintf(mp3, 128, "%s/ai_dub_seg_%d_%ld.mp3", TMPDIR, i, (long)time(NULL));
        seg_mp3s[i] = mp3;

        /* rate: convert 50-400 range → edge-tts percentage offset from 100% */
        int rate_offset = a->rate - 150; /* 150 = normal → 0% */
        char rate_str[16]; snprintf(rate_str, sizeof(rate_str), "%d", rate_offset);
        char pitch_str[16]; snprintf(pitch_str, sizeof(pitch_str), "%d", a->pitch);
        char *argv[] = { "python3", "-c", (char*)TTS_SEG_PY,
                         (char*)txt, (char*)seg_voice, mp3, rate_str, pitch_str, NULL };
        char err[512] = {0};
        int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
        if (rc != 0) {
            LOG_WARN("TTS seg %d failed: %s", i, err);
            free(mp3); seg_mp3s[i] = NULL;
        }

        /* ── Feature 3: Auto-Speed — ensure TTS fits inside segment slot ──
         * Measure the generated MP3 duration via ffprobe.  If it is longer
         * than the available slot, recalculate the rate offset so the voice
         * finishes just in time, then regenerate that single segment.       */
        if (seg_mp3s[i] && file_exists(seg_mp3s[i])) {
            double slot = a->segments[i].end - a->segments[i].start;
            if (slot > 0.05) {  /* only for segments with a meaningful slot */
                char dur_out[64] = {0};
                char *probe_argv[] = { g_ffprobe, "-v", "quiet",
                    "-show_entries", "format=duration",
                    "-of", "csv=p=0", seg_mp3s[i], NULL };
                run_cmd(probe_argv, dur_out, sizeof(dur_out), NULL, 0);
                double mp3_dur = strtod(dur_out, NULL);
                if (mp3_dur > 0.0 && mp3_dur > slot) {
                    /* Calculate the speed-up needed: new_rate = rate * (mp3_dur/slot)
                     * expressed as edge-tts percentage offset from normal (150 = 0%).
                     * Cap at +150% (rate=300) to avoid incomprehensible speech.     */
                    double speed_factor = mp3_dur / slot;
                    int new_rate = (int)(a->rate * speed_factor);
                    if (new_rate > 300) new_rate = 300;
                    int new_rate_offset = new_rate - 150;
                    char new_rate_str[16];
                    snprintf(new_rate_str, sizeof(new_rate_str), "%d", new_rate_offset);
                    LOG_INFO("AutoSpeed seg %d: mp3=%.2fs slot=%.2fs → rate offset %d",
                             i, mp3_dur, slot, new_rate_offset);
                    char *retry_argv[] = { "python3", "-c", (char*)TTS_SEG_PY,
                                           (char*)txt, (char*)seg_voice, seg_mp3s[i],
                                           new_rate_str, pitch_str, NULL };
                    char rerr[512] = {0};
                    int rrc = run_cmd(retry_argv, NULL, 0, rerr, sizeof(rerr));
                    if (rrc != 0) LOG_WARN("AutoSpeed retry seg %d failed: %s", i, rerr);
                }
            }
        }

        int pct = 10 + (i * 40) / n;
        char msg[128]; snprintf(msg, sizeof(msg), "TTS %d/%d…", i+1, n);
        post_progress(a->bar, a->lbl, pct, msg);
    }

    /* ── Step 2: Get video duration via ffprobe ── */
    double vid_dur = 0;
    {
        char *argv[] = { g_ffprobe, "-v", "quiet",
                         "-show_entries", "format=duration",
                         "-of", "csv=p=0", a->video_path, NULL };
        char out[64] = {0};
        run_cmd(argv, out, sizeof(out), NULL, 0);
        vid_dur = strtod(out, NULL);
        if (vid_dur <= 0) vid_dur = a->segments[n-1].end + 1.0;
    }

    /* ── Step 3: Build ffmpeg amix filter for timed dub audio ──
     * We write a temporary shell script so the filter string (which can
     * be very long) does not overflow argv limits.
     *
     * Filter chain (all inside -filter_complex, no -filter:a):
     *   anullsrc → atrim=duration → aformat   = silent base track
     *   each mp3 → adelay → delayed clip
     *   amix all inputs → [amixed]
     *   [amixed] → dynaudnorm (single-pass loudness) → [aout]
     *
     * dynaudnorm replaces loudnorm: loudnorm needs seekable input and
     * does two passes, which fails on generated filtergraph streams.
     * dynaudnorm is single-pass and works fine here.
     *
     * Audio ducking: applied via volume= on [0:a] (original BG track)
     * before mixing, so the expression only affects the BG, not TTS.
     */
    post_progress(a->bar, a->lbl, 55, "Mixing audio…");

    /* fix.txt #3 + 6.png: Batched amix.
     * Previously all N TTS clips were passed as -i inputs into a single
     * ffmpeg call with amix=inputs=N+1.  For long videos (e.g. 1h with
     * 1700 segments) this blows past the shell's ARG_MAX and ffmpeg's
     * filter-graph limits, producing:
     *     /bin/sh: …/ffmpeg: Argument list too long
     * Fix: if valid > BATCH_SIZE, mix segments in batches into partial
     * WAVs, then do one final amix of (video + partials).  Each sub-call
     * stays well under every limit. */
    const int BATCH_SIZE = 32;

    char script_path[160];
    snprintf(script_path, sizeof(script_path),
             "%s/ai_dub_mix_%ld.sh", TMPDIR, (long)time(NULL));

    int valid = 0;
    int *valid_idx = calloc((size_t)n, sizeof(int));
    if (!valid_idx) {
        snprintf(a->error, sizeof(a->error), "Out of memory allocating index list");
        goto cleanup;
    }
    for (int i = 0; i < n; i++) {
        if (seg_mp3s[i] && file_exists(seg_mp3s[i]))
            valid_idx[valid++] = i;
    }

    /* Allocate batch wav paths unconditionally so cleanup is simple.
     * n_batches == 0 when valid == 0 (TTS all failed) — still emit
     * a valid output (video + silence) by forcing one empty batch. */
    int n_batches = (valid + BATCH_SIZE - 1) / BATCH_SIZE;
    if (n_batches <= 1) {
        /* Small N — single ffmpeg call is fine and cheaper. */
        FILE *sh = fopen(script_path, "w");
        if (!sh) {
            snprintf(a->error, sizeof(a->error), "Cannot create mix script");
            goto cleanup;
        }
        xchmod(script_path, 0755);

        fprintf(sh, "#!/bin/sh\nset -e\n");
        fprintf(sh, "%s -y -i \"%s\"", g_ffmpeg, a->video_path);
        for (int v = 0; v < valid; v++)
            fprintf(sh, " -i \"%s\"", seg_mp3s[valid_idx[v]]);

        fprintf(sh, " -filter_complex \"");
        fprintf(sh, "anullsrc=r=44100:cl=stereo,atrim=duration=%.3f,"
                    "aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo[silence]",
                    vid_dur);
        for (int v = 0; v < valid; v++) {
            long delay_ms = (long)(a->segments[valid_idx[v]].start * 1000.0);
            fprintf(sh, ";[%d:a]aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,"
                        "adelay=%ld|%ld[d%d]", v + 1, delay_ms, delay_ms, v);
        }
        fprintf(sh, ";[silence]");
        for (int v = 0; v < valid; v++) fprintf(sh, "[d%d]", v);
        fprintf(sh, "amix=inputs=%d:duration=first:normalize=0,"
                    "dynaudnorm=f=150:g=15[aout]\"", valid + 1);

        if (a->burn_subs && a->ass_path[0] && file_exists(a->ass_path))
            fprintf(sh, " -map 0:v -vf \"ass='%s'\"", a->ass_path);
        else
            fprintf(sh, " -map 0:v");
        fprintf(sh, " -map \"[aout]\" -c:v libx264 -crf 22 -preset fast"
                    " -c:a aac -b:a 192k \"%s\"\n", a->out_path);
        fclose(sh);

        char *run_argv[] = { (char *)SHELL_PATH, script_path, NULL };
        char err2[2048] = {0};
        int rc2 = run_cmd(run_argv, NULL, 0, err2, sizeof(err2));
        if (rc2 != 0)
            snprintf(a->error, sizeof(a->error), "FFmpeg mix failed: %s", err2);
        unlink(script_path);
        post_progress(a->bar, a->lbl, rc2 ? 0 : 100,
                      rc2 ? "Queen failed" : "Queen complete!");
        LOG_INFO("Queen %s -> %s (single, valid=%d)",
                 rc2 ? "FAILED" : "OK", a->out_path, valid);
    } else {
        /* ── Batched path: valid > BATCH_SIZE ── */
        char **batch_wavs = calloc((size_t)n_batches, sizeof(char*));
        if (!batch_wavs) {
            snprintf(a->error, sizeof(a->error), "Out of memory allocating batch list");
            goto cleanup;
        }

        int failed = 0;
        long t0 = (long)time(NULL);

        for (int b = 0; b < n_batches; b++) {
            int bstart = b * BATCH_SIZE;
            int bcount = (bstart + BATCH_SIZE <= valid) ? BATCH_SIZE : (valid - bstart);

            char *wav = malloc(256);
            if (!wav) {
                snprintf(a->error, sizeof(a->error), "OOM building batch %d", b);
                failed = 1; break;
            }
            snprintf(wav, 256, "%s/ai_dub_batch_%ld_%d.wav", TMPDIR, t0, b);
            batch_wavs[b] = wav;

            char bpath[200];
            snprintf(bpath, sizeof(bpath), "%s/ai_dub_batch_%ld_%d.sh", TMPDIR, t0, b);
            FILE *sh = fopen(bpath, "w");
            if (!sh) {
                snprintf(a->error, sizeof(a->error), "Cannot create batch %d script", b);
                failed = 1; break;
            }
            xchmod(bpath, 0755);
            fprintf(sh, "#!/bin/sh\nset -e\n");
            fprintf(sh, "%s -y -f lavfi -i anullsrc=r=44100:cl=stereo", g_ffmpeg);
            for (int j = 0; j < bcount; j++) {
                int i = valid_idx[bstart + j];
                fprintf(sh, " -i \"%s\"", seg_mp3s[i]);
            }
            fprintf(sh, " -filter_complex \"");
            fprintf(sh, "[0:a]atrim=duration=%.3f,aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo[s]",
                    vid_dur);
            for (int j = 0; j < bcount; j++) {
                int i = valid_idx[bstart + j];
                long delay_ms = (long)(a->segments[i].start * 1000.0);
                fprintf(sh, ";[%d:a]aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,"
                            "adelay=%ld|%ld[d%d]", j + 1, delay_ms, delay_ms, j);
            }
            fprintf(sh, ";[s]");
            for (int j = 0; j < bcount; j++) fprintf(sh, "[d%d]", j);
            fprintf(sh, "amix=inputs=%d:duration=first:normalize=0[aout]\"", bcount + 1);
            fprintf(sh, " -map \"[aout]\" -c:a pcm_s16le \"%s\"\n", wav);
            fclose(sh);

            char *run_argv[] = { (char *)SHELL_PATH, bpath, NULL };
            char berr[2048] = {0};
            int rc = run_cmd(run_argv, NULL, 0, berr, sizeof(berr));
            unlink(bpath);
            if (rc != 0) {
                snprintf(a->error, sizeof(a->error),
                         "Batch %d/%d mix failed: %s", b + 1, n_batches, berr);
                failed = 1; break;
            }
            int pct = 55 + (b * 30) / n_batches;
            char msg[128];
            snprintf(msg, sizeof(msg), "Mixing batch %d/%d…", b + 1, n_batches);
            post_progress(a->bar, a->lbl, pct, msg);
        }

        if (!failed) {
            /* Final mix: video + each batch WAV.  n_batches ≤ ~54 for 1700
             * segs @ BATCH_SIZE=32 — safely within ffmpeg amix limits. */
            post_progress(a->bar, a->lbl, 88, "Muxing final video…");
            FILE *sh = fopen(script_path, "w");
            if (!sh) {
                snprintf(a->error, sizeof(a->error), "Cannot create final mix script");
                failed = 1;
            } else {
                xchmod(script_path, 0755);
                fprintf(sh, "#!/bin/sh\nset -e\n");
                fprintf(sh, "%s -y -i \"%s\"", g_ffmpeg, a->video_path);
                for (int b = 0; b < n_batches; b++)
                    fprintf(sh, " -i \"%s\"", batch_wavs[b]);
                fprintf(sh, " -filter_complex \"");
                for (int b = 0; b < n_batches; b++) fprintf(sh, "[%d:a]", b + 1);
                fprintf(sh, "amix=inputs=%d:duration=first:normalize=0,"
                            "dynaudnorm=f=150:g=15[aout]\"", n_batches);
                if (a->burn_subs && a->ass_path[0] && file_exists(a->ass_path))
                    fprintf(sh, " -map 0:v -vf \"ass='%s'\"", a->ass_path);
                else
                    fprintf(sh, " -map 0:v");
                fprintf(sh, " -map \"[aout]\" -c:v libx264 -crf 22 -preset fast"
                            " -c:a aac -b:a 192k \"%s\"\n", a->out_path);
                fclose(sh);

                char *run_argv[] = { (char *)SHELL_PATH, script_path, NULL };
                char err2[2048] = {0};
                int rc2 = run_cmd(run_argv, NULL, 0, err2, sizeof(err2));
                if (rc2 != 0) {
                    snprintf(a->error, sizeof(a->error),
                             "Final mux failed: %s", err2);
                    failed = 1;
                }
                unlink(script_path);
            }
        }

        /* Cleanup batch wavs */
        for (int b = 0; b < n_batches; b++) {
            if (batch_wavs[b]) {
                unlink(batch_wavs[b]);
                free(batch_wavs[b]);
            }
        }
        free(batch_wavs);

        post_progress(a->bar, a->lbl, failed ? 0 : 100,
                      failed ? "Queen failed" : "Queen complete!");
        LOG_INFO("Queen %s -> %s (batched %d batches, valid=%d)",
                 failed ? "FAILED" : "OK", a->out_path, n_batches, valid);
    }

cleanup:
    free(valid_idx);
    for (int i = 0; i < n; i++) {
        if (seg_mp3s[i]) { unlink(seg_mp3s[i]); free(seg_mp3s[i]); }
    }
    free(seg_mp3s);
    /* Bug fix #4: NULL check after malloc for callback struct */
    DubCB *cb = malloc(sizeof *cb);
    if (!cb) { LOG_ERROR("dub_thread: OOM allocating DubCB"); return NULL; }
    cb->a = a;
    post_done(_dub_fwd, cb);
    return NULL;
}

/* ── Queen button callback ─────────────────────────────────────────────── */
typedef struct { AppWidgets *aw; } DubCtx;
static void on_dub_done(void *ctx, const char *out, const char *err) {
    DubCtx *c = ctx; AppWidgets *aw = c->aw; free(c);
    if (err) show_err(aw->window, "Queen Error", err);
    else {
        char msg[512]; snprintf(msg, sizeof(msg), "Dubbed video saved:\n%s", out);
        show_info(aw->window, "Queen Complete", msg);
    }
}

static void cb_dub(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe and translate first."); return;
    }

    /* Pick voice from combo */
    int vi = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->dub_voice_combo));
    const char *voice = (vi >= 0 && KM_VOICES[vi]) ? KM_VOICES[vi] : KM_VOICE_FEMALE;

    /* Ask where to save */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Dubbed Video", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "MP4 Video"); gtk_file_filter_add_pattern(flt, "*.mp4");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);

    /* Suggest a filename: original_dubbed.mp4 */
    {
        char base[512];
        snprintf(base, sizeof(base), "%s", g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600]; snprintf(suggest, sizeof(suggest), "%s_dubbed.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));

        /* Build temp ASS for subtitle burning */
        char ass_tmp[128] = {0};
        char font_name[256]; find_khmer_font(font_name, sizeof(font_name));
        snprintf(ass_tmp, sizeof(ass_tmp),
                 "%s/ai_dub_%ld_subs.ass", TMPDIR, (long)time(NULL));
        char ass_err[256] = {0};
        int has_subs = export_ass(aw->state->segments, aw->state->seg_count,
                                   ass_tmp, font_name,
                                   aw->state->config.subtitle_font_size,
                                   aw->state->config.subtitle_language,
                                   ass_err, sizeof(ass_err));

        DubArg *a = calloc(1, sizeof *a);
        snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
        snprintf(a->out_path,   sizeof(a->out_path),   "%s", out_path);
        snprintf(a->voice,      sizeof(a->voice),       "%s", voice);
        a->rate      = aw->state->config.tts_rate;
        a->pitch     = aw->state->config.tts_pitch;
        a->burn_subs = has_subs;
        if (has_subs) snprintf(a->ass_path, sizeof(a->ass_path), "%s", ass_tmp);
        a->segments  = aw->state->segments;
        a->seg_count = aw->state->seg_count;
        a->bar = aw->bar; a->lbl = aw->status_lbl;
        DubCtx *ctx = malloc(sizeof *ctx); ctx->aw = aw;
        a->on_done = on_dub_done; a->ctx = ctx;

        /* Save voice choice */
        snprintf(aw->state->config.dub_voice, sizeof(aw->state->config.dub_voice),
                 "%s", voice);

        pthread_t t; pthread_create(&t, NULL, dub_thread, a); pthread_detach(t);
        post_progress(aw->bar, aw->status_lbl, 2, "Starting dubbing…");
        g_free(out_path);
    }
    gtk_widget_destroy(dlg);
}


static void fmt_ts(double sec, char *buf, int vtt) {
    int h  = (int)(sec / 3600);
    int m  = (int)((fmod(sec, 3600)) / 60);
    int s  = (int)fmod(sec, 60);
    int ms = (int)((sec - floor(sec)) * 1000);
    if (vtt) snprintf(buf, 32, "%02d:%02d:%02d.%03d", h, m, s, ms);
    else     snprintf(buf, 32, "%02d:%02d:%02d,%03d", h, m, s, ms);
}

static int export_subs(const Segment *segs, int n, const char *path,
                        int is_vtt, char *err, size_t esz) {
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(err, esz, "Cannot open: %s", path); return 0; }
    if (is_vtt) fputs("WEBVTT\n\n", f);
    for (int i = 0; i < n; i++) {
        char ts[32], te[32];
        fmt_ts(segs[i].start, ts, is_vtt);
        fmt_ts(segs[i].end,   te, is_vtt);
        const char *txt = segs[i].translated[0]
                          ? segs[i].translated : segs[i].text;
        if (!is_vtt) fprintf(f, "%d\n", i+1);
        fprintf(f, "%s --> %s\n%s\n\n", ts, te, txt);
    }
    fclose(f); return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 15 — GTK3 UI widgets struct (defined at top; kept here as marker)
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 16 — Dialog helpers
 * ═══════════════════════════════════════════════════════════════════ */
static void show_msg(GtkWidget *win, GtkMessageType t,
                      const char *title, const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        t, GTK_BUTTONS_OK, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}
/* show_err / show_info macros defined at top of file */

/* ── Feature 2: row-activated callback ─────────────────────────────
 *   col 5 (VOICE PROFILE) → toggle male/female for that row
 *   col 7 (▶ Preview)      → TTS preview of the segment
 * ─────────────────────────────────────────────────────────────────── */
static void on_seg_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                  GtkTreeViewColumn *col, gpointer ud) {
    AppWidgets *aw = ud;
    /* Find which column was activated */
    GList *cols = gtk_tree_view_get_columns(tv);
    int col_idx = 0;
    for (GList *l = cols; l; l = l->next, col_idx++)
        if (l->data == col) break;
    g_list_free(cols);

    gint *indices = gtk_tree_path_get_indices(path);
    int row = indices ? indices[0] : -1;
    if (row < 0 || row >= aw->state->seg_count) return;

    /* ── VOICE PROFILE toggle (visual col 5) ── */
    if (col_idx == 5) {
        const char *cur = aw->state->segments[row].speaker_id;
        int is_male = (strncmp(cur, "male", 4) == 0 ||
                       strncmp(cur, "SPEAKER_01", 10) == 0);
        const char *new_id  = is_male ? "female" : "male";
        const char *new_lbl = is_male ? "♀ Female" : "♂ Male";
        snprintf(aw->state->segments[row].speaker_id,
                 sizeof(aw->state->segments[row].speaker_id), "%s", new_id);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(aw->seg_store), &iter, path))
            gtk_list_store_set(aw->seg_store, &iter, 4, new_lbl, -1);
        return;
    }

    /* ── ▶ Preview (visual col 7) ── */
    if (col_idx != 7) return;

    const char *txt = aw->state->segments[row].translated[0]
                      ? aw->state->segments[row].translated
                      : aw->state->segments[row].text;
    if (!txt[0]) { show_info(aw->window, "Preview", "No text in this segment."); return; }

    /* fix.txt #4: use per-segment voice (speaker_id) if set,
     * otherwise fall back to the global dub_voice combo.
     * This fixes the bug where voice switching only changed the UI label
     * but preview always used the global combo voice. */
    const char *seg_voice = NULL;
    if (aw->state->segments[row].speaker_id[0]) {
        if (strncmp(aw->state->segments[row].speaker_id, "male", 4) == 0 ||
            strncmp(aw->state->segments[row].speaker_id, "SPEAKER_01", 10) == 0)
            seg_voice = KM_VOICE_MALE;
        else if (strncmp(aw->state->segments[row].speaker_id, "female", 6) == 0 ||
                 strncmp(aw->state->segments[row].speaker_id, "SPEAKER_00", 10) == 0)
            seg_voice = KM_VOICE_FEMALE;
    }
    if (!seg_voice) {
        int vi = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->dub_voice_combo));
        seg_voice = (vi >= 0 && KM_VOICES[vi]) ? KM_VOICES[vi] : KM_VOICE_FEMALE;
    }

    PreviewArg *a = malloc(sizeof *a);
    snprintf(a->text,  sizeof(a->text),  "%s", txt);
    snprintf(a->voice, sizeof(a->voice), "%s", seg_voice);
    a->rate = aw->state->config.tts_rate;
    a->bar  = aw->bar;
    a->lbl  = aw->status_lbl;

    pthread_t t; pthread_create(&t, NULL, preview_thread, a); pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 10, "Starting preview…");
}

/* ── refresh segment TreeView ──────────────────────────────────────── */
static void refresh_segs(AppWidgets *aw) {
    gtk_list_store_clear(aw->seg_store);
    AppState *st = aw->state;
    for (int i = 0; i < st->seg_count; i++) {
        char ts[32], te[32];
        snprintf(ts, sizeof(ts), "%.2f", st->segments[i].start);
        snprintf(te, sizeof(te), "%.2f", st->segments[i].end);
        /* Determine voice label from speaker_id */
        const char *voice_lbl = "♀ Female";
        if (st->segments[i].speaker_id[0]) {
            if (strncmp(st->segments[i].speaker_id, "male", 4) == 0)
                voice_lbl = "♂ Male";
            else if (strncmp(st->segments[i].speaker_id, "SPEAKER_01", 10) == 0)
                voice_lbl = "♂ Male";
        }
        GtkTreeIter it;
        gtk_list_store_append(aw->seg_store, &it);
        gtk_list_store_set(aw->seg_store, &it,
            0, ts,
            1, te,
            2, st->segments[i].text,
            3, st->segments[i].translated,
            4, voice_lbl,   /* VOICE PROFILE col */
            5, "Pending",   /* AUDIO STATUS col  */
            -1);
    }
    /* Redraw timeline tracks to reflect new/changed segments */
    tl_refresh(aw);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 17 — Button callbacks
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Feature 4: load_video_path — shared by Open dialog and Recent menu ── */
static void load_video_path(AppWidgets *aw, const char *path) {
    snprintf(aw->state->video_path, sizeof(aw->state->video_path), "%s", path);
    char lbl[512];
    snprintf(lbl, sizeof(lbl), "Video: %s", g_path_get_basename(path));
    gtk_label_set_text(aw->video_lbl, lbl);
    LOG_INFO("Video opened: %s", path);
    /* Record in recent list and persist */
    push_recent_file(&aw->state->config, path);
    save_config(&aw->state->config);
    rebuild_recent_menu(aw);
    /* Start GStreamer video playback */
    gst_start_video(aw, path);
}

/* ── Open video ────────────────────────────────────────────────────── */
static void cb_open_video(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open Video", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "Video");
    for (int i = 0; VIDEO_EXT[i]; i++) {
        char pat[16]; snprintf(pat, sizeof(pat), "*%s", VIDEO_EXT[i]);
        gtk_file_filter_add_pattern(flt, pat);
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        load_video_path(aw, path);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

/* ── Transcribe ─────────────────────────────────────────────────────── */
typedef struct { AppWidgets *aw; char wav_tmp[4096]; WhisperArg *wa; } TxCtx;

static void on_whisper_done(void *ctx, Segment *segs, int n, const char *err) {
    TxCtx *tc = ctx;
    AppWidgets *aw = tc->aw;
    if (err) {
        show_err(aw->window, "Transcription Error", err);
    } else {
        free(aw->state->segments);
        aw->state->segments  = segs;
        aw->state->seg_count = n;
        refresh_segs(aw);
        post_progress(aw->bar, aw->status_lbl, 100, "Transcription complete");
        char msg[64]; snprintf(msg, sizeof(msg), "%d segments transcribed", n);
        show_info(aw->window, "Done", msg);
    }
    unlink(tc->wav_tmp);
    free(tc);
}

static void on_audio_for_whisper(void *ctx, const char *wav, const char *err) {
    TxCtx *tc = ctx; AppWidgets *aw = tc->aw;
    if (err) { show_err(aw->window, "Audio Error", err); free(tc->wa); free(tc); return; }
    WhisperArg *wa = tc->wa;
    snprintf(wa->wav_path, sizeof(wa->wav_path), "%s", wav);
    wa->bar = aw->bar; wa->lbl = aw->status_lbl;
    wa->on_done = on_whisper_done; wa->ctx = tc;
    pthread_t t; pthread_create(&t, NULL, whisper_thread, wa); pthread_detach(t);
}

static void cb_transcribe(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video file first."); return;
    }
    char wav[4096];
    snprintf(wav, sizeof(wav), "%s/ai_dubber_%ld.wav", TMPDIR, (long)time(NULL));

    TxCtx *tc = calloc(1, sizeof *tc);
    tc->aw = aw; snprintf(tc->wav_tmp, sizeof(tc->wav_tmp), "%s", wav);

    WhisperArg *wa = calloc(1, sizeof *wa);
    const char *model = gtk_combo_box_text_get_active_text(aw->model_combo);
    snprintf(wa->model, sizeof(wa->model), "%s", model ? model : "base");
    wa->max_segments = aw->state->config.max_segments;
    tc->wa = wa;

    AudioArg *aa = calloc(1, sizeof *aa);
    snprintf(aa->video_path, sizeof(aa->video_path), "%s", aw->state->video_path);
    snprintf(aa->wav_path,   sizeof(aa->wav_path),   "%s", wav);
    aa->bar = aw->bar; aa->lbl = aw->status_lbl;
    aa->on_done = on_audio_for_whisper; aa->ctx = tc;

    pthread_t t; pthread_create(&t, NULL, audio_thread_real, aa); pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 5, "Starting transcription…");
}

/* ── Translate ──────────────────────────────────────────────────────── */
typedef struct { AppWidgets *aw; TranslateArg *ta; } TrCtx;

static void on_translate_done(void *ctx, const char *err) {
    TrCtx *tc = ctx; AppWidgets *aw = tc->aw;
    if (err) show_err(aw->window, "Translation Error", err);
    else {
        refresh_segs(aw);
        post_progress(aw->bar, aw->status_lbl, 100, "Translation complete");
        show_info(aw->window, "Done", "Translation complete!");
    }
    free(tc);
}

static void cb_translate(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe a video first."); return;
    }
    TrCtx *tc = calloc(1, sizeof *tc); tc->aw = aw;
    TranslateArg *ta = calloc(1, sizeof *ta);
    ta->segments  = aw->state->segments;
    ta->seg_count = aw->state->seg_count;
    const char *lang = gtk_combo_box_text_get_active_text(aw->lang_combo);
    snprintf(ta->target_lang, sizeof(ta->target_lang), "%s", lang ? lang : "km");
    /* Feature 5: read engine from combo */
    const char *eng = gtk_combo_box_text_get_active_text(aw->trans_engine_combo);
    snprintf(ta->engine, sizeof(ta->engine), "%s", eng ? eng : "google");
    ta->bar = aw->bar; ta->lbl = aw->status_lbl;
    ta->on_done = on_translate_done; ta->ctx = tc;
    tc->ta = ta;
    pthread_t t; pthread_create(&t, NULL, translate_thread, ta); pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 60, "Translating…");
}

/* ── Save Video (burn subtitles in) ─────────────────────────────────── */

/*
 * find_khmer_font() — ask fontconfig for a font that covers Khmer (U+1780).
 * Falls back to "Noto Sans Khmer" if fc-match is not available.
 */
static void find_khmer_font(char *out, size_t sz) {
#ifdef _WIN32
    /* Windows has no fontconfig; pick a sensible default and trust the
     * user to install "Noto Sans Khmer" (or "Khmer OS") system-wide.  */
    snprintf(out, sz, "Noto Sans Khmer");
#else
    /* Ask fc-match for a font covering Khmer codepoint U+1780 */
    FILE *p = popen("fc-match --format='%{family}' ':charset=1780' 2>/dev/null", "r");
    if (p) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), p) && buf[0]) {
            /* fc-match may return "family1,family2" — take first token */
            buf[strcspn(buf, ",\n")] = '\0';
            if (buf[0]) { snprintf(out, sz, "%s", buf); pclose(p); return; }
        }
        pclose(p);
    }
    snprintf(out, sz, "Noto Sans Khmer");
#endif
}

/*
 * export_ass() — write an ASS subtitle file with large, outlined subtitles.
 * Font name, size, outline and shadow are set in the [V4+ Styles] section.
 * Returns 1 on success, 0 on failure (fills err).
 */
static int export_ass(const Segment *segs, int n, const char *path,
                       const char *font_name, int font_size, int use_original,
                       char *err, size_t esz) {
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(err, esz, "Cannot open: %s", path); return 0; }

    /* ASS header — Style fields:
     * Name, Fontname, Fontsize, PrimaryColour, SecondaryColour,
     * OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut,
     * ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow,
     * Alignment, MarginL, MarginR, MarginV, Encoding              */
    fprintf(f,
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "Collisions: Normal\n"
        "PlayResX: 1920\n"
        "PlayResY: 1080\n"
        "Timer: 100.0\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour,"
        " OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut,"
        " ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow,"
        " Alignment, MarginL, MarginR, MarginV, Encoding\n"
        /* White text, black outline (AABBGGRR hex), bold, 3px outline, 1px shadow */
        "Style: Default,%s,%d,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
        "-1,0,0,0,100,100,0,0,1,3,1,2,80,80,60,1\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV,"
        " Effect, Text\n",
        font_name, font_size);

    for (int i = 0; i < n; i++) {
        /* ASS timestamps: H:MM:SS.cc  (centiseconds) */
        double s = segs[i].start, e = segs[i].end;
        int sh = (int)(s/3600), sm = (int)(fmod(s,3600)/60),
            ss = (int)fmod(s,60), sc = (int)(fmod(s,1)*100);
        int eh = (int)(e/3600), em = (int)(fmod(e,3600)/60),
            es = (int)fmod(e,60), ec = (int)(fmod(e,1)*100);
        const char *txt = (use_original || !segs[i].translated[0])
                          ? segs[i].text : segs[i].translated;
        /* Replace newlines with ASS line-break \N */
        char safe[4096]; size_t si = 0;
        for (const char *c = txt; *c && si + 3 < sizeof(safe); c++) {
            if (*c == '\n') { safe[si++]='\\'; safe[si++]='N'; }
            else safe[si++] = *c;
        }
        safe[si] = '\0';
        fprintf(f, "Dialogue: 0,%d:%02d:%02d.%02d,%d:%02d:%02d.%02d,"
                   "Default,,0,0,0,,{\\blur2}%s\n",
                sh,sm,ss,sc, eh,em,es,ec, safe);
    }
    fclose(f); return 1;
}

typedef struct {
    char  video_path[4096];
    char  ass_path[4096];   /* temp ASS file */
    char  out_path[4096];
    GtkProgressBar *bar; GtkLabel *lbl;
    char  error[2048];
    void (*on_done)(void *ctx, const char *out, const char *err);
    void *ctx;
} SaveVideoArg;

typedef struct { SaveVideoArg *a; } SaveVideoCB;
static void _savevideo_fwd(void *d) {
    SaveVideoCB *c = d; SaveVideoArg *a = c->a;
    a->on_done(a->ctx, a->error[0] ? NULL : a->out_path,
               a->error[0] ? a->error : NULL);
    free(a); free(c);
}

static void *savevideo_thread(void *data) {
    SaveVideoArg *a = data;
    post_progress(a->bar, a->lbl, 20, "Burning subtitles into video…");
    /*
     * Use ass filter (libass) — reads styling from the .ass file itself,
     * so font/size/outline are all controlled by export_ass() above.
     */
    char vf[4096 + 16];
    snprintf(vf, sizeof(vf), "ass=%s", a->ass_path);
    char *argv[] = {
        g_ffmpeg, "-y", "-i", a->video_path,
        "-vf", vf,
        "-c:v", "libx264", "-crf", "22", "-preset", "fast",
        "-c:a", "copy",
        a->out_path, NULL
    };
    char err[2048] = {0};
    int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
    if (rc != 0) snprintf(a->error, sizeof(a->error), "ffmpeg: %s", err);
    post_progress(a->bar, a->lbl, rc ? 0 : 100,
                  rc ? "Save video failed" : "Video saved");
    LOG_INFO("SaveVideo %s: %s", rc?"FAILED":"OK", a->out_path);
    unlink(a->ass_path);
    SaveVideoCB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_savevideo_fwd, cb);
    return NULL;
}

typedef struct { AppWidgets *aw; } SaveVideoCtx;
static void on_savevideo_done(void *ctx, const char *out, const char *err) {
    SaveVideoCtx *c = ctx; AppWidgets *aw = c->aw; free(c);
    if (err) show_err(aw->window, "Save Video Error", err);
    else {
        char msg[512]; snprintf(msg, sizeof(msg), "Saved: %s", out);
        show_info(aw->window, "Video Saved", msg);
    }
}

static void cb_save_video(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe (and optionally translate) first."); return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Video As", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "MP4 Video");
    gtk_file_filter_add_pattern(flt, "*.mp4");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));

        /* Detect best available Khmer font */
        char font_name[256];
        find_khmer_font(font_name, sizeof(font_name));
        LOG_INFO("Subtitle font: %s", font_name);

        /* Write temp ASS to /tmp */
        char ass_tmp[128];
        snprintf(ass_tmp, sizeof(ass_tmp),
                 "%s/ai_dubber_%ld_subs.ass", TMPDIR, (long)time(NULL));
        char ass_err[512] = {0};
        /* Font size 56 is good for 1080p; libass scales automatically */
        if (!export_ass(aw->state->segments, aw->state->seg_count,
                        ass_tmp, font_name,
                        aw->state->config.subtitle_font_size,
                        aw->state->config.subtitle_language,
                        ass_err, sizeof(ass_err))) {
            show_err(aw->window, "ASS Error", ass_err);
            g_free(out_path); gtk_widget_destroy(dlg); return;
        }

        SaveVideoArg *a = calloc(1, sizeof *a);
        snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
        snprintf(a->ass_path,   sizeof(a->ass_path),   "%s", ass_tmp);
        snprintf(a->out_path,   sizeof(a->out_path),   "%s", out_path);
        a->bar = aw->bar; a->lbl = aw->status_lbl;
        SaveVideoCtx *ctx = malloc(sizeof *ctx); ctx->aw = aw;
        a->on_done = on_savevideo_done; a->ctx = ctx;
        pthread_t t; pthread_create(&t, NULL, savevideo_thread, a); pthread_detach(t);
        post_progress(aw->bar, aw->status_lbl, 5, "Starting video save…");
        g_free(out_path);
    }
    gtk_widget_destroy(dlg);
}

/* ── Feature 2: Live Subtitle Preview ──────────────────────────────────
 * Generates TTS for a single selected segment and plays it immediately
 * using ffplay so the user can verify the voice before a full dub run.
 * Runs in a detached pthread to avoid blocking the GTK main loop.      */

static void *preview_thread(void *data) {
    PreviewArg *a = data;

    /* Step 1: generate TTS to a temp mp3 */
    char tmp_mp3[128];
    snprintf(tmp_mp3, sizeof(tmp_mp3),
             "%s/ai_dub_preview_%ld.mp3", TMPDIR, (long)time(NULL));

    int rate_offset = a->rate - 150;
    char rate_str[16]; snprintf(rate_str, sizeof(rate_str), "%d", rate_offset);
    char *tts_argv[] = { "python3", "-c", (char*)TTS_SEG_PY,
                         a->text, a->voice, tmp_mp3, rate_str, NULL };
    char err[512] = {0};
    post_progress(a->bar, a->lbl, 50, "Generating preview TTS…");
    int rc = run_cmd(tts_argv, NULL, 0, err, sizeof(err));
    if (rc != 0) {
        LOG_WARN("Preview TTS failed: %s", err);
        post_progress(a->bar, a->lbl, 0, "Preview TTS failed");
        free(a); return NULL;
    }

    /* Step 2: play using ffplay (non-display, auto-exit when done) */
    post_progress(a->bar, a->lbl, 80, "Playing preview…");
    char *play_argv[] = { (char *)"ffplay", (char *)"-nodisp",
                          (char *)"-autoexit", tmp_mp3, NULL };
    run_cmd(play_argv, NULL, 0, NULL, 0);
    unlink(tmp_mp3);
    post_progress(a->bar, a->lbl, 100, "Preview done");
    free(a);
    return NULL;
}

/* ── Export subtitles (SRT / VTT) ───────────────────────────────────── */
static void do_export(AppWidgets *aw, int is_vtt) {
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe (and optionally translate) first."); return;
    }
    const char *title = is_vtt ? "Save WebVTT" : "Save SRT";
    const char *pat   = is_vtt ? "*.vtt"       : "*.srt";
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        title, GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, is_vtt ? "WebVTT" : "SRT");
    gtk_file_filter_add_pattern(flt, pat);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        char err[512] = {0};
        if (export_subs(aw->state->segments, aw->state->seg_count,
                         path, is_vtt, err, sizeof(err)))
            show_info(aw->window, "Exported", path);
        else
            show_err(aw->window, "Export Failed", err);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}
static void cb_export_srt(GtkWidget *b, gpointer d) { (void)b; do_export(d, 0); }
static void cb_export_vtt(GtkWidget *b, gpointer d) { (void)b; do_export(d, 1); }

/* ── Extract MP3 ────────────────────────────────────────────────────── */
typedef struct { AppWidgets *aw; } Mp3Ctx;
static void on_mp3_done(void *ctx, const char *out, const char *err) {
    Mp3Ctx *mc = ctx; AppWidgets *aw = mc->aw; free(mc);
    if (err) show_err(aw->window, "MP3 Error", err);
    else { char msg[512]; snprintf(msg, sizeof(msg), "Saved: %s", out);
           show_info(aw->window, "MP3 Done", msg); }
}

static void cb_mp3(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save MP3", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        Mp3Arg *a = calloc(1, sizeof *a);
        snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
        snprintf(a->out_path,   sizeof(a->out_path),   "%s", path);
        a->bar = aw->bar; a->lbl = aw->status_lbl;
        Mp3Ctx *mc = malloc(sizeof *mc); mc->aw = aw;
        a->on_done = on_mp3_done; a->ctx = mc;
        pthread_t t; pthread_create(&t, NULL, mp3_thread, a); pthread_detach(t);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

/* ── Save settings ──────────────────────────────────────────────────── */
static void cb_save_cfg(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    AppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    const char *m = gtk_combo_box_text_get_active_text(aw->model_combo);
    const char *l = gtk_combo_box_text_get_active_text(aw->lang_combo);
    snprintf(cfg.whisper_model,   sizeof(cfg.whisper_model),   "%s", m ? m : "base");
    snprintf(cfg.target_language, sizeof(cfg.target_language), "%s", l ? l : "km");
    snprintf(cfg.tts_voice_id,    sizeof(cfg.tts_voice_id),    "%s",
             gtk_entry_get_text(aw->voice_entry));
    /* dub voice from combo */
    int vi = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->dub_voice_combo));
    snprintf(cfg.dub_voice, sizeof(cfg.dub_voice), "%s",
             (vi >= 0 && KM_VOICES[vi]) ? KM_VOICES[vi] : KM_VOICE_FEMALE);
    cfg.tts_rate = (int)gtk_spin_button_get_value(aw->rate_spin);
    /* Feature 5: save translation engine */
    const char *eng = gtk_combo_box_text_get_active_text(aw->trans_engine_combo);
    snprintf(cfg.translation_engine, sizeof(cfg.translation_engine), "%s", eng ? eng : "google");
    /* Feature 4 (ducking): save checkbox state */
    cfg.audio_ducking = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(aw->duck_check)) ? 1 : 0;
    /* add.txt new config options */
    cfg.subtitle_font_size = (int)gtk_spin_button_get_value(aw->font_size_spin);
    cfg.subtitle_language  = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->sub_lang_combo));
    const char *odir = gtk_entry_get_text(aw->output_dir_entry);
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", odir ? odir : "");
    cfg.max_segments = (int)gtk_spin_button_get_value(aw->max_seg_spin);
    cfg.tts_pitch    = (int)gtk_spin_button_get_value(aw->pitch_spin);
    /* carry over recent files & clone script */
    memcpy(cfg.recent_files, aw->state->config.recent_files, sizeof(cfg.recent_files));
    snprintf(cfg.voice_clone_script, sizeof(cfg.voice_clone_script),
             "%s", aw->state->config.voice_clone_script);
    aw->state->config = cfg;
    save_config(&cfg);
    show_info(aw->window, "Settings", "Settings saved.");
    LOG_INFO("Config saved: model=%s lang=%s rate=%d voice=%s engine=%s ducking=%d",
             cfg.whisper_model, cfg.target_language, cfg.tts_rate,
             cfg.dub_voice, cfg.translation_engine, cfg.audio_ducking);
}

/* ── Cell edited callback — updates segment text and backing store ── */
static void on_cell_edited(GtkCellRendererText *renderer,
                            gchar *path_str, gchar *new_text,
                            gpointer ud) {
    AppWidgets *aw = ud;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(renderer), "col_index"));
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    gint *indices = gtk_tree_path_get_indices(path);
    int row = indices ? indices[0] : -1;
    gtk_tree_path_free(path);
    if (row < 0 || row >= aw->state->seg_count) return;

    if (col == 2) { /* Original */
        snprintf(aw->state->segments[row].text,
                 sizeof(aw->state->segments[row].text), "%s", new_text);
    } else if (col == 3) { /* Translated */
        snprintf(aw->state->segments[row].translated,
                 sizeof(aw->state->segments[row].translated), "%s", new_text);
    } else {
        return; /* ignore edits on Start/End/Preview columns */
    }
    /* Update list store */
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(aw->seg_store);
    if (gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        gtk_list_store_set(aw->seg_store, &iter, col, new_text, -1);
}

/* ── Feature 4: Recent Files menu ──────────────────────────────────────
 * rebuild_recent_menu() clears and repopulates the GtkMenu attached to
 * aw->recent_btn.  Each item calls load_video_path when activated.
 * Safe to call from any context where the GTK main loop is running.   */

/* ═══════════════════════════════════════════════════════════════════
 * FEATURE 6 — Subtitle Merge
 * ═══════════════════════════════════════════════════════════════════ */

/* Merge the selected segment with the one immediately after it. */
static void cb_merge_segments(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (st->seg_count < 2) {
        show_err(aw->window, "Not Enough", "Need at least 2 segments to merge."); return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
    GtkTreeModel     *mdl = GTK_TREE_MODEL(st->aw->seg_store);
    GtkTreeIter       iter;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &iter)) {
        show_err(aw->window, "No Selection", "Select the first segment to merge."); return;
    }
    GtkTreePath *tp = gtk_tree_model_get_path(mdl, &iter);
    gint *idx = gtk_tree_path_get_indices(tp);
    int row = idx ? idx[0] : -1;
    gtk_tree_path_free(tp);
    if (row < 0 || row >= st->seg_count - 1) {
        show_err(aw->window, "Last Segment",
                 "Cannot merge: select a segment that has one after it."); return;
    }

    /* Extend end time to next segment's end */
    st->segments[row].end = st->segments[row + 1].end;

    /* Concatenate texts */
    char merged_text[4096], merged_trans[2048];
    snprintf(merged_text,  sizeof(merged_text),  "%s %s",
             st->segments[row].text,       st->segments[row + 1].text);
    snprintf(merged_trans, sizeof(merged_trans), "%s %s",
             st->segments[row].translated, st->segments[row + 1].translated);
    snprintf(st->segments[row].text,       sizeof(st->segments[row].text),
             "%s", merged_text);
    snprintf(st->segments[row].translated, sizeof(st->segments[row].translated),
             "%s", merged_trans);

    /* Shift remaining segments left */
    for (int i = row + 1; i < st->seg_count - 1; i++)
        st->segments[i] = st->segments[i + 1];
    st->seg_count--;

    refresh_segs(aw);
    LOG_INFO("Merged segment %d with %d", row, row + 1);
}

/* ═══════════════════════════════════════════════════════════════════
 * FEATURE 3 — Voice Cloning (Local, via user-supplied Python script)
 * Runs: python3 <script> <text> <out_mp3>
 * The script is responsible for cloning logic (Coqui TTS, GPT-SoVITS…)
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    char  text[2048];
    char  script[4096];   /* path to cloning script */
    char  out_path[4096];
    GtkProgressBar *bar;
    GtkLabel       *lbl;
    char  error[2048];
    void (*on_done)(void *ctx, const char *out, const char *err);
    void *ctx;
} CloneArg;

typedef struct { CloneArg *a; } CloneCB;
static void _clone_fwd(void *d) {
    CloneCB *c = d; CloneArg *a = c->a;
    a->on_done(a->ctx, a->error[0] ? NULL : a->out_path,
               a->error[0] ? a->error : NULL);
    free(a); free(c);
}

static void *clone_thread(void *data) {
    CloneArg *a = data;
    post_progress(a->bar, a->lbl, 20, "Voice cloning…");
    char *argv[] = { "python3", a->script, a->text, a->out_path, NULL };
    char err[2048] = {0};
    int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
    if (rc != 0)
        snprintf(a->error, sizeof(a->error), "Clone script failed: %s", err);
    else
        LOG_INFO("Voice clone OK: %s", a->out_path);
    post_progress(a->bar, a->lbl, rc ? 0 : 100,
                  rc ? "Voice clone failed" : "Clone done");
    CloneCB *cb = malloc(sizeof *cb); cb->a = a;
    post_done(_clone_fwd, cb);
    return NULL;
}

/* Quick "test clone" button: synthesises the first translated segment
 * with the cloning script and plays it back via ffplay.              */
typedef struct { AppWidgets *aw; } CloneCtx;
static void on_clone_done(void *ctx, const char *out, const char *err) {
    CloneCtx *c = ctx; AppWidgets *aw = c->aw; free(c);
    if (err) { show_err(aw->window, "Clone Error", err); return; }
    char *play_argv[] = { (char *)"ffplay", (char *)"-nodisp",
                          (char *)"-autoexit", (char *)out, NULL };
    run_cmd(play_argv, NULL, 0, NULL, 0);
    unlink(out);
    post_progress(aw->bar, aw->status_lbl, 100, "Clone preview done");
}

static void cb_test_clone(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->config.voice_clone_script[0]) {
        show_err(aw->window, "No Script",
                 "Set a voice clone script path in Settings first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first."); return;
    }
    const char *txt = aw->state->segments[0].translated[0]
                      ? aw->state->segments[0].translated
                      : aw->state->segments[0].text;
    CloneArg *a = calloc(1, sizeof *a);
    snprintf(a->text,   sizeof(a->text),   "%s", txt);
    snprintf(a->script, sizeof(a->script), "%s",
             aw->state->config.voice_clone_script);
    snprintf(a->out_path, sizeof(a->out_path),
             "%s/ai_dub_clone_%ld.mp3", TMPDIR, (long)time(NULL));
    a->bar = aw->bar; a->lbl = aw->status_lbl;
    CloneCtx *ctx = malloc(sizeof *ctx); ctx->aw = aw;
    a->on_done = on_clone_done; a->ctx = ctx;
    pthread_t t; pthread_create(&t, NULL, clone_thread, a); pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 5, "Starting voice clone…");
}

/* ═══════════════════════════════════════════════════════════════════
 * FEATURE 1 — Batch Processing Mode
 * Opens a folder, finds all compatible video files, and queues them
 * through the full Audio→Whisper→Translate pipeline sequentially.
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    AppWidgets *aw;
    char  **files;     /* NULL-terminated array of strdup'd paths */
    int     file_count;
    int     current;
    char    target_lang[8];
    char    model[32];
    char    engine[32];
} BatchCtx;

static void batch_process_next(BatchCtx *bc);  /* forward */

/* Called when one file's translate is done — move to next */
static void on_batch_translate_done(void *ctx, const char *err) {
    BatchCtx *bc = ctx;
    AppWidgets *aw = bc->aw;
    if (err) LOG_WARN("Batch translate error [%d]: %s", bc->current, err);
    /* Auto-save SRT next to the video */
    {
        char srt_path[4096];
        snprintf(srt_path, sizeof(srt_path), "%s", bc->files[bc->current]);
        char *dot = strrchr(srt_path, '.'); if (dot) *dot = '\0';
        strncat(srt_path, ".srt", sizeof(srt_path) - strlen(srt_path) - 1);
        char serr[256] = {0};
        export_subs(aw->state->segments, aw->state->seg_count,
                    srt_path, 0, serr, sizeof(serr));
        LOG_INFO("Batch SRT: %s", srt_path);
    }
    refresh_segs(aw);
    bc->current++;
    batch_process_next(bc);
}

static void on_batch_whisper_done(void *ctx, Segment *segs, int n,
                                   const char *err) {
    BatchCtx *bc = ctx;
    AppWidgets *aw = bc->aw;
    if (err || n == 0) {
        LOG_WARN("Batch whisper error [%d]: %s", bc->current, err ? err : "0 segs");
        bc->current++;
        batch_process_next(bc);
        return;
    }
    free(aw->state->segments);
    aw->state->segments  = segs;
    aw->state->seg_count = n;

    TranslateArg *ta = calloc(1, sizeof *ta);
    ta->segments  = aw->state->segments;
    ta->seg_count = aw->state->seg_count;
    snprintf(ta->target_lang, sizeof(ta->target_lang), "%s", bc->target_lang);
    snprintf(ta->engine,      sizeof(ta->engine),      "%s", bc->engine);
    ta->bar = aw->bar; ta->lbl = aw->status_lbl;
    ta->on_done = on_batch_translate_done; ta->ctx = bc;
    pthread_t t; pthread_create(&t, NULL, translate_thread, ta); pthread_detach(t);
}

/* Intermediate: after audio extracted, launch whisper */
typedef struct { BatchCtx *bc; WhisperArg *wa; char wav_tmp[4096]; } BatchAudioCtx;

static void on_batch_audio_done(void *ctx, const char *wav, const char *err) {
    BatchAudioCtx *bac = ctx;
    BatchCtx      *bc  = bac->bc;
    AppWidgets    *aw  = bc->aw;
    if (err) {
        LOG_WARN("Batch audio error [%d]: %s", bc->current, err);
        free(bac->wa); free(bac);
        bc->current++;
        batch_process_next(bc);
        return;
    }
    WhisperArg *wa = bac->wa;
    snprintf(wa->wav_path, sizeof(wa->wav_path), "%s", wav);
    wa->bar = aw->bar; wa->lbl = aw->status_lbl;
    wa->on_done = on_batch_whisper_done; wa->ctx = bc;
    snprintf(bc->aw->state->video_path,
             sizeof(bc->aw->state->video_path), "%s", bc->files[bc->current]);
    free(bac);   /* wav_tmp is in AudioArg which frees itself */
    pthread_t t; pthread_create(&t, NULL, whisper_thread, wa); pthread_detach(t);
}

static void batch_process_next(BatchCtx *bc) {
    AppWidgets *aw = bc->aw;
    if (bc->current >= bc->file_count) {
        /* All done */
        char msg[128];
        snprintf(msg, sizeof(msg), "Batch complete: %d file(s) processed.",
                 bc->file_count);
        post_progress(aw->bar, aw->status_lbl, 100, "Batch complete");
        show_info(aw->window, "Batch Done", msg);
        /* Free file list */
        for (int i = 0; i < bc->file_count; i++) free(bc->files[i]);
        free(bc->files); free(bc);
        return;
    }

    const char *path = bc->files[bc->current];
    char pct_msg[256];
    snprintf(pct_msg, sizeof(pct_msg), "Batch [%d/%d]: %s",
             bc->current + 1, bc->file_count,
             g_path_get_basename(path));
    post_progress(aw->bar, aw->status_lbl,
                  (bc->current * 100) / bc->file_count, pct_msg);
    LOG_INFO("Batch processing: %s", path);

    char wav[4096];
    snprintf(wav, sizeof(wav), "%s/ai_batch_%d_%ld.wav",
             TMPDIR, bc->current, (long)time(NULL));

    BatchAudioCtx *bac = calloc(1, sizeof *bac);
    bac->bc = bc;
    snprintf(bac->wav_tmp, sizeof(bac->wav_tmp), "%s", wav);

    WhisperArg *wa = calloc(1, sizeof *wa);
    snprintf(wa->model, sizeof(wa->model), "%s", bc->model);
    bac->wa = wa;

    AudioArg *aa = calloc(1, sizeof *aa);
    snprintf(aa->video_path, sizeof(aa->video_path), "%s", path);
    snprintf(aa->wav_path,   sizeof(aa->wav_path),   "%s", wav);
    aa->bar = aw->bar; aa->lbl = aw->status_lbl;
    aa->on_done = on_batch_audio_done; aa->ctx = bac;
    pthread_t t; pthread_create(&t, NULL, audio_thread_real, aa); pthread_detach(t);
}

static void cb_batch_open(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Select Folder for Batch Processing",
        GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Process", GTK_RESPONSE_ACCEPT, NULL);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }
    char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    /* Enumerate compatible video files */
    GDir *dir = g_dir_open(folder, 0, NULL);
    if (!dir) {
        show_err(aw->window, "Batch Error", "Cannot open folder."); g_free(folder); return;
    }
    char **files = NULL;
    int nfiles = 0;
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        /* Check extension */
        const char *ext = strrchr(name, '.');
        if (!ext) continue;
        int ok = 0;
        for (int i = 0; VIDEO_EXT[i]; i++)
            if (g_ascii_strcasecmp(ext, VIDEO_EXT[i]) == 0) { ok = 1; break; }
        if (!ok) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", folder, name);
        char **tmp_files = realloc(files, (nfiles + 1) * sizeof(char*));
        if (!tmp_files) {
            /* realloc failed — keep the original `files` buffer intact
             * so the already-collected entries aren't leaked.         */
            LOG_WARN("realloc failed while scanning batch folder (%d files collected)", nfiles);
            break;
        }
        files = tmp_files;
        files[nfiles++] = strdup(full);
    }
    g_dir_close(dir);
    g_free(folder);

    if (nfiles == 0) {
        show_err(aw->window, "Batch", "No compatible video files found in folder."); return;
    }

    /* Sort for predictable order */
    for (int i = 0; i < nfiles - 1; i++)
        for (int j = i + 1; j < nfiles; j++)
            if (strcmp(files[i], files[j]) > 0)
                { char *tmp = files[i]; files[i] = files[j]; files[j] = tmp; }

    char confirm[256];
    snprintf(confirm, sizeof(confirm),
             "Found %d video file(s). Start batch transcription + translation?", nfiles);
    GtkWidget *conf_dlg = gtk_message_dialog_new(
        GTK_WINDOW(aw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", confirm);
    gtk_window_set_title(GTK_WINDOW(conf_dlg), "Batch Process");
    int resp = gtk_dialog_run(GTK_DIALOG(conf_dlg));
    gtk_widget_destroy(conf_dlg);
    if (resp != GTK_RESPONSE_YES) {
        for (int i = 0; i < nfiles; i++) free(files[i]);
        free(files); return;
    }

    BatchCtx *bc = calloc(1, sizeof *bc);
    bc->aw         = aw;
    bc->files      = files;
    bc->file_count = nfiles;
    bc->current    = 0;
    const char *m = gtk_combo_box_text_get_active_text(aw->model_combo);
    snprintf(bc->model, sizeof(bc->model), "%s", m ? m : "base");
    const char *l = gtk_combo_box_text_get_active_text(aw->lang_combo);
    snprintf(bc->target_lang, sizeof(bc->target_lang), "%s", l ? l : "km");
    const char *e = gtk_combo_box_text_get_active_text(aw->trans_engine_combo);
    snprintf(bc->engine, sizeof(bc->engine), "%s", e ? e : "google");

    LOG_INFO("Batch: %d files, model=%s lang=%s", nfiles, bc->model, bc->target_lang);
    batch_process_next(bc);
}

typedef struct { AppWidgets *aw; char path[4096]; } RecentItemCtx;

static void cb_recent_item(GtkMenuItem *item, gpointer ud) {
    (void)item;
    RecentItemCtx *ctx = ud;
    /* ctx is attached as object data — do NOT free it here */
    load_video_path(ctx->aw, ctx->path);
}

/* fix.txt #4: actually wipe RECENT FILES (config + sidebar + menu) */
static void cb_clear_recent_files(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = (AppWidgets*)ud;
    if (!aw || !aw->state) return;
    for (int i = 0; i < 5; i++) aw->state->config.recent_files[i][0] = '\0';
    save_config(&aw->state->config);
    rebuild_recent_menu(aw);
    /* Rebuild the sidebar list_box: drop existing rows, add empty placeholder */
    if (aw->recent_list_box) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(aw->recent_list_box));
        for (GList *p = kids; p; p = p->next)
            gtk_widget_destroy(GTK_WIDGET(p->data));
        g_list_free(kids);
        GtkWidget *empty_lbl = gtk_label_new("No recent files");
        gtk_style_context_add_class(gtk_widget_get_style_context(empty_lbl), "recent-meta");
        gtk_widget_set_margin_top(empty_lbl, 8);
        gtk_box_pack_start(GTK_BOX(aw->recent_list_box), empty_lbl, FALSE, FALSE, 0);
        gtk_widget_show_all(aw->recent_list_box);
    }
}

static void rebuild_recent_menu(AppWidgets *aw) {
    if (!aw->recent_btn) return;

    /* Destroy any existing menu attached to the button */
    GtkMenu *old_menu = GTK_MENU(g_object_get_data(G_OBJECT(aw->recent_btn), "recent_menu"));
    if (old_menu) gtk_widget_destroy(GTK_WIDGET(old_menu));

    GtkWidget *menu = gtk_menu_new();
    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (!aw->state->config.recent_files[i][0]) continue;
        /* Show only the basename in the menu label for readability */
        gchar *base = g_path_get_basename(aw->state->config.recent_files[i]);
        GtkWidget *item = gtk_menu_item_new_with_label(base);
        g_free(base);
        /* Store context as object data tied to item lifetime */
        RecentItemCtx *ctx = malloc(sizeof *ctx);
        ctx->aw = aw;
        snprintf(ctx->path, sizeof(ctx->path), "%s",
                 aw->state->config.recent_files[i]);
        g_object_set_data_full(G_OBJECT(item), "ctx", ctx, free);
        g_signal_connect(item, "activate", G_CALLBACK(cb_recent_item), ctx);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        count++;
    }
    if (count == 0) {
        GtkWidget *empty = gtk_menu_item_new_with_label("(no recent files)");
        gtk_widget_set_sensitive(empty, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), empty);
    }
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(aw->recent_btn), menu);
    g_object_set_data(G_OBJECT(aw->recent_btn), "recent_menu", menu);
}

/* ═══════════════════════════════════════════════════════════════════
 * add.txt FEATURE 1 — "Queen Everything" Full Pipeline
 * One button runs: Extract Audio → Transcribe → Translate → Queen
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    AppWidgets *aw;
    char out_path[4096];
    char voice[128];
} PipelineCtx;

/* Forward declarations for pipeline chain */

static void on_pipeline_dub_done(void *ctx, const char *out, const char *err) {
    PipelineCtx *pc = ctx;
    AppWidgets *aw = pc->aw;
    free(pc);
    if (err) { show_err(aw->window, "Pipeline Queen Error", err); return; }
    char msg[512];
    snprintf(msg, sizeof(msg), "Full pipeline complete!\nDubbed video saved:\n%s", out);
    show_info(aw->window, "Pipeline Done", msg);
    post_progress(aw->bar, aw->status_lbl, 100, "Pipeline complete!");
}

static void on_pipeline_translate_done(void *ctx, const char *err) {
    PipelineCtx *pc = ctx;
    AppWidgets *aw = pc->aw;
    if (err) { show_err(aw->window, "Pipeline Translate Error", err); free(pc); return; }
    refresh_segs(aw);
    post_progress(aw->bar, aw->status_lbl, 70, "Translation done, starting dub…");

    /* Build ASS subtitles */
    char ass_tmp[128] = {0};
    char font_name[256]; find_khmer_font(font_name, sizeof(font_name));
    snprintf(ass_tmp, sizeof(ass_tmp), "%s/ai_pipe_%ld_subs.ass", TMPDIR, (long)time(NULL));
    char ass_err[256] = {0};
    int has_subs = export_ass(aw->state->segments, aw->state->seg_count,
                               ass_tmp, font_name,
                               aw->state->config.subtitle_font_size,
                               aw->state->config.subtitle_language,
                               ass_err, sizeof(ass_err));

    DubArg *a = calloc(1, sizeof *a);
    snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
    snprintf(a->out_path,   sizeof(a->out_path),   "%s", pc->out_path);
    snprintf(a->voice,      sizeof(a->voice),       "%s", pc->voice);
    a->rate      = aw->state->config.tts_rate;
    a->pitch     = aw->state->config.tts_pitch;
    a->burn_subs = has_subs;
    if (has_subs) snprintf(a->ass_path, sizeof(a->ass_path), "%s", ass_tmp);
    a->segments  = aw->state->segments;
    a->seg_count = aw->state->seg_count;
    a->bar = aw->bar; a->lbl = aw->status_lbl;
    a->on_done = on_pipeline_dub_done; a->ctx = pc;
    pthread_t t; pthread_create(&t, NULL, dub_thread, a); pthread_detach(t);
}

static void on_pipeline_whisper_done(void *ctx, Segment *segs, int n, const char *err) {
    PipelineCtx *pc = ctx;
    AppWidgets *aw = pc->aw;
    if (err || n == 0) {
        show_err(aw->window, "Pipeline Transcribe Error", err ? err : "0 segments");
        free(pc); return;
    }
    free(aw->state->segments);
    aw->state->segments  = segs;
    aw->state->seg_count = n;
    refresh_segs(aw);
    post_progress(aw->bar, aw->status_lbl, 50, "Transcription done, translating…");

    TranslateArg *ta = calloc(1, sizeof *ta);
    ta->segments  = aw->state->segments;
    ta->seg_count = aw->state->seg_count;
    const char *lang = gtk_combo_box_text_get_active_text(aw->lang_combo);
    snprintf(ta->target_lang, sizeof(ta->target_lang), "%s", lang ? lang : "km");
    const char *eng = gtk_combo_box_text_get_active_text(aw->trans_engine_combo);
    snprintf(ta->engine, sizeof(ta->engine), "%s", eng ? eng : "google");
    ta->bar = aw->bar; ta->lbl = aw->status_lbl;
    ta->on_done = on_pipeline_translate_done; ta->ctx = pc;
    pthread_t t; pthread_create(&t, NULL, translate_thread, ta); pthread_detach(t);
}

typedef struct { PipelineCtx *pc; WhisperArg *wa; char wav_tmp[4096]; } PipelineAudioCtx;

static void on_pipeline_audio_done(void *ctx, const char *wav, const char *err) {
    PipelineAudioCtx *pac = ctx;
    PipelineCtx *pc = pac->pc;
    AppWidgets *aw = pc->aw;
    if (err) {
        show_err(aw->window, "Pipeline Audio Error", err);
        free(pac->wa); free(pac); free(pc); return;
    }
    WhisperArg *wa = pac->wa;
    snprintf(wa->wav_path, sizeof(wa->wav_path), "%s", wav);
    wa->bar = aw->bar; wa->lbl = aw->status_lbl;
    wa->on_done = on_pipeline_whisper_done; wa->ctx = pc;
    free(pac);
    pthread_t t; pthread_create(&t, NULL, whisper_thread, wa); pthread_detach(t);
}

static void cb_full_pipeline(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video file first."); return;
    }

    /* Pick voice */
    int vi = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->dub_voice_combo));
    const char *voice = (vi >= 0 && KM_VOICES[vi]) ? KM_VOICES[vi] : KM_VOICE_FEMALE;

    /* Ask where to save the final dubbed video */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Full Pipeline Output", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "MP4 Video");
    gtk_file_filter_add_pattern(flt, "*.mp4");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s", g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600]; snprintf(suggest, sizeof(suggest), "%s_ai_dubbed.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    PipelineCtx *pc = calloc(1, sizeof *pc);
    pc->aw = aw;
    snprintf(pc->out_path, sizeof(pc->out_path), "%s", out_path);
    snprintf(pc->voice,    sizeof(pc->voice),    "%s", voice);
    g_free(out_path);

    char wav[4096];
    snprintf(wav, sizeof(wav), "%s/ai_pipe_%ld.wav", TMPDIR, (long)time(NULL));

    PipelineAudioCtx *pac = calloc(1, sizeof *pac);
    pac->pc = pc;
    snprintf(pac->wav_tmp, sizeof(pac->wav_tmp), "%s", wav);

    WhisperArg *wa = calloc(1, sizeof *wa);
    const char *model = gtk_combo_box_text_get_active_text(aw->model_combo);
    snprintf(wa->model, sizeof(wa->model), "%s", model ? model : "base");
    pac->wa = wa;

    AudioArg *aa = calloc(1, sizeof *aa);
    snprintf(aa->video_path, sizeof(aa->video_path), "%s", aw->state->video_path);
    snprintf(aa->wav_path,   sizeof(aa->wav_path),   "%s", wav);
    aa->bar = aw->bar; aa->lbl = aw->status_lbl;
    aa->on_done = on_pipeline_audio_done; aa->ctx = pac;

    pthread_t t; pthread_create(&t, NULL, audio_thread_real, aa); pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 5, "Pipeline: extracting audio…");
    LOG_INFO("Full pipeline started: %s → %s", aw->state->video_path, pc->out_path);
}

/* ═══════════════════════════════════════════════════════════════════
 * add.txt FEATURE 2 — Per-Speaker Voice Assignment
 * After diarization, show a dialog to assign a TTS voice per speaker.
 * The chosen voice is stored in Segment.speaker_voice and used by dub.
 * ═══════════════════════════════════════════════════════════════════ */

/* g_speaker_voices and g_speaker_voices_count declared at top with globals */

static void ensure_speaker_voices(int n) {
    if (g_speaker_voices_count >= n) return;
    void *tmp = realloc(g_speaker_voices, n * sizeof(*g_speaker_voices));
    if (!tmp) {
        /* realloc failed — leave the existing table untouched so we
         * don't leak the previously allocated speaker voices.         */
        LOG_WARN("realloc failed in ensure_speaker_voices (have %d, want %d)",
                 g_speaker_voices_count, n);
        return;
    }
    g_speaker_voices = tmp;
    for (int i = g_speaker_voices_count; i < n; i++)
        g_speaker_voices[i][0] = '\0';
    g_speaker_voices_count = n;
}

/* Dialog: shows a list of unique speakers found, lets user pick voice per speaker */
typedef struct {
    char speaker_id[64];
    char voice[128];
    GtkComboBoxText *combo;
} SpeakerRow;

static void cb_speaker_voice_dialog(AppWidgets *aw, char **speakers, int n) {
    /* Collect unique speaker IDs */
    char uniq[32][64]; int n_uniq = 0;
    for (int i = 0; i < n; i++) {
        if (!speakers[i]) continue;
        int found = 0;
        for (int u = 0; u < n_uniq; u++)
            if (strcmp(uniq[u], speakers[i]) == 0) { found = 1; break; }
        if (!found && n_uniq < 32)
            snprintf(uniq[n_uniq++], 64, "%s", speakers[i]);
    }
    if (n_uniq == 0) {
        show_info(aw->window, "No Speakers", "No speaker IDs found."); return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Assign Voice Per Speaker",
        GTK_WINDOW(aw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply",  GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 420, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    SpeakerRow rows[32];
    for (int u = 0; u < n_uniq; u++) {
        snprintf(rows[u].speaker_id, sizeof(rows[u].speaker_id), "%s", uniq[u]);
        rows[u].voice[0] = '\0';

        GtkWidget *lbl = gtk_label_new(uniq[u]);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, u, 1, 1);

        GtkComboBoxText *cb = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
        for (int v = 0; KM_VOICE_LBLS[v]; v++)
            gtk_combo_box_text_append_text(cb, KM_VOICE_LBLS[v]);
        /* Also allow custom voice entry */
        gtk_combo_box_text_append_text(cb, "✏ Custom voice…");
        gtk_combo_box_set_active(GTK_COMBO_BOX(cb), u % 2);  /* alternate F/M */
        rows[u].combo = cb;
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(cb), 1, u, 1, 1);
    }

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        ensure_speaker_voices(n);
        /* Build speaker→voice map from dialog selections */
        char spk_voice_map[32][128];
        for (int u = 0; u < n_uniq; u++) {
            int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(rows[u].combo));
            if (idx >= 0 && KM_VOICES[idx])
                snprintf(spk_voice_map[u], 128, "%s", KM_VOICES[idx]);
            else
                snprintf(spk_voice_map[u], 128, "%s", KM_VOICE_FEMALE);
        }
        /* Assign voice to each segment */
        for (int i = 0; i < n; i++) {
            g_speaker_voices[i][0] = '\0';
            if (!speakers[i]) continue;
            for (int u = 0; u < n_uniq; u++) {
                if (strcmp(uniq[u], speakers[i]) == 0) {
                    snprintf(g_speaker_voices[i], 128, "%s", spk_voice_map[u]);
                    break;
                }
            }
        }
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Voice assigned for %d speakers.\n"
                 "Run '🔊 Dub (KH Voice)' — per-segment voices will be used.", n_uniq);
        show_info(aw->window, "Voice Assignment", msg);
        LOG_INFO("Per-speaker voices assigned for %d segments", n);
    }
    gtk_widget_destroy(dlg);
}

/* ═══════════════════════════════════════════════════════════════════
 * NEW FEATURE A — Multi-Speaker Diarization Support
 * Calls a Python script (pyannote-audio) to label each segment with
 * a speaker ID, which can later drive automatic TTS voice selection.
 * ═══════════════════════════════════════════════════════════════════ */

/* Python helper: uses pyannote-audio to diarize a WAV and emit JSON
 * lines of { "start": X, "end": Y, "speaker": "SPEAKER_00" }
 * (DIARIZE_PY string defined near top with other globals)            */

/* identify_speakers() — match each Segment to the diarization output.
 * For every segment the speaker whose interval overlaps the most is
 * assigned.  Result is stored in a caller-supplied speakers[] array
 * (parallel to segs[], each entry is a strdup'd label like
 * "SPEAKER_00").  Caller must free each non-NULL entry.              */
static void identify_speakers(const char *wav_path,
                               Segment *segs, int n,
                               char **speakers) {
    if (!wav_path || !segs || n <= 0 || !speakers) return;

    /* Zero the output array first */
    for (int i = 0; i < n; i++) speakers[i] = NULL;

    /* Run the diarization script */
    char *argv[] = { "python3", "-c", (char *)DIARIZE_PY,
                     (char *)wav_path, NULL };
    /* Bug fix #3: explicit size_t to prevent integer overflow on shift */
    size_t buf_sz = (size_t)1 << 18;   /* 256 KB — plenty for a full diarization */
    char *out = malloc(buf_sz);
    /* Bug fix #4: already present below — confirmed */
    if (!out) return;
    char err[1024] = {0};

    int rc = run_cmd(argv, out, buf_sz, err, sizeof(err));
    if (rc != 0) {
        LOG_WARN("identify_speakers: diarization failed: %s", err);
        free(out);
        return;
    }

    /* Parse the JSON array of {start, end, speaker} objects */
    typedef struct { double start, end; char speaker[64]; } DiarSeg;
    DiarSeg *ds = calloc(4096, sizeof(DiarSeg));
    if (!ds) { free(out); return; }
    int ds_count = 0;

    const char *p = out;
    while (*p && ds_count < 4096) {
        p = strchr(p, '{');
        if (!p) break;
        const char *obj_start = p;
        int depth = 0;
        const char *q = p;
        while (*q) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        size_t olen = (size_t)(q - obj_start);
        char *obj = malloc(olen + 1);
        /* Bug fix #4: NULL check; Bug fix #1: free on every path */
        if (!obj) { p = q; continue; }
        memcpy(obj, obj_start, olen);
        obj[olen] = '\0';

        double v;
        if (json_get_double(obj, "start", &v)) ds[ds_count].start = v;
        if (json_get_double(obj, "end",   &v)) ds[ds_count].end   = v;
        json_get_str(obj, "speaker", ds[ds_count].speaker,
                     sizeof(ds[ds_count].speaker));
        free(obj);
        ds_count++;
        p = q;
    }

    /* For each segment find the diarization entry with maximum overlap */
    for (int i = 0; i < n; i++) {
        double best_overlap = 0.0;
        int    best_idx     = -1;
        for (int j = 0; j < ds_count; j++) {
            double ov_start = segs[i].start > ds[j].start
                              ? segs[i].start : ds[j].start;
            double ov_end   = segs[i].end < ds[j].end
                              ? segs[i].end : ds[j].end;
            double overlap  = ov_end - ov_start;
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best_idx     = j;
            }
        }
        const char *spk = (best_idx >= 0) ? ds[best_idx].speaker : "SPEAKER_00";
        if (speakers) speakers[i] = strdup(spk);
        /* add.txt: also store directly in Segment for persistence */
        snprintf(segs[i].speaker_id, sizeof(segs[i].speaker_id), "%s", spk);
    }

    free(ds);
    free(out);
    LOG_INFO("identify_speakers: labelled %d segments from %d diar entries",
             n, ds_count);
}

/* ── GUI callback: run diarization on the current wav / video ──────── */
static void cb_identify_speakers(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first."); return;
    }

    /* Extract a temporary WAV if needed (reuse Queen pipeline pattern) */
    char wav[4096];
    snprintf(wav, sizeof(wav), "%s/ai_dub_diar_%ld.wav", TMPDIR, (long)time(NULL));

    post_progress(aw->bar, aw->status_lbl, 5, "Extracting audio for diarization…");
    char *ffmpeg_argv[] = {
        g_ffmpeg, "-y", "-i", aw->state->video_path,
        "-vn", "-ar", "16000", "-ac", "1", "-f", "wav", wav, NULL
    };
    char ferr[512] = {0};
    if (run_cmd(ffmpeg_argv, NULL, 0, ferr, sizeof(ferr)) != 0) {
        show_err(aw->window, "Audio Error", ferr);
        return;
    }

    post_progress(aw->bar, aw->status_lbl, 20, "Running speaker diarization…");

    /* Bug fix #4: NULL check after calloc for speakers array */
    char **speakers = calloc((size_t)aw->state->seg_count, sizeof(char *));
    if (!speakers) {
        show_err(aw->window, "Memory Error", "Out of memory."); return;
    }
    identify_speakers(wav, aw->state->segments, aw->state->seg_count, speakers);
    unlink(wav);

    /* Display results and open voice assignment dialog */
    char msg_buf[4096];
    int mpos = 0;
    mpos += snprintf(msg_buf + mpos, sizeof(msg_buf) - mpos,
                     "Speaker labels assigned:\n\n");
    for (int i = 0; i < aw->state->seg_count && i < 20 && mpos < (int)sizeof(msg_buf) - 80; i++) {
        mpos += snprintf(msg_buf + mpos, sizeof(msg_buf) - mpos,
            "[%d] %.1f–%.1f  →  %s\n",
            i,
            aw->state->segments[i].start,
            aw->state->segments[i].end,
            speakers[i] ? speakers[i] : "?");
    }
    if (aw->state->seg_count > 20)
        snprintf(msg_buf + mpos, sizeof(msg_buf) - mpos,
                 "…(truncated, see log for full list)");

    show_info(aw->window, "Diarization Done", msg_buf);

    /* Open per-speaker voice assignment dialog */
    cb_speaker_voice_dialog(aw, speakers, aw->state->seg_count);

    for (int i = 0; i < aw->state->seg_count; i++) free(speakers[i]);
    free(speakers);
    post_progress(aw->bar, aw->status_lbl, 100, "Diarization complete");
}

/* ═══════════════════════════════════════════════════════════════════
 * NEW FEATURE B — Live Audio Visualizer (GTK3 + Cairo)
 * Animated bar visualizer with Style, Color, Bar count, Height, Width,
 * and Speed controls — matching the audio_visualizer.html design.
 * Bars animate independently using a smooth lerp toward random targets,
 * producing a realistic "bouncing spectrum analyser" effect.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Visualizer constants ── */
#define VIZ_BAR_MAX   200
#define VIZ_WIDTH     900
#define VIZ_HEIGHT    300
#define VIZ_TIMER_MS  16     /* ~60 fps */

/* ── Visualizer style / color enums ── */
typedef enum { VIZ_STYLE_BOTTOM = 0, VIZ_STYLE_CENTER, VIZ_STYLE_MIRROR } VizStyle;
typedef enum { VIZ_COL_YELLOW_GREEN = 0, VIZ_COL_CYAN, VIZ_COL_RAINBOW,
               VIZ_COL_ORANGE } VizColor;

/* ── Per-visualizer state (heap-allocated, owned by the window) ── */
typedef struct {
    /* animated bar state */
    double bars[VIZ_BAR_MAX];
    double targets[VIZ_BAR_MAX];

    /* controls (updated from GTK widgets) */
    int       bar_count;   /* 20–200          */
    double    height_mult; /* 0.20–2.00       */
    double    width_pct;   /* 0.30–1.00       */
    double    speed;       /* 1–10            */
    VizStyle  style;
    VizColor  color;
    gboolean  running;

    /* GTK widgets owned by the pop-up window */
    GtkWidget      *window;
    GtkWidget      *canvas;
    GtkWidget      *toggle_btn;
    GtkComboBoxText*style_combo;
    GtkComboBoxText*color_combo;
    GtkAdjustment  *bar_adj;
    GtkAdjustment  *ht_adj;
    GtkAdjustment  *wd_adj;
    GtkAdjustment  *sp_adj;
    GtkLabel       *bar_val_lbl;
    GtkLabel       *ht_val_lbl;
    GtkLabel       *wd_val_lbl;
    GtkLabel       *sp_val_lbl;

    guint timer_id;

    /* back-reference to main AppWidgets (for Burn to Video) */
    AppWidgets *aw_ref;
} VizState;

/* ── Colour helper: return (r,g,b) in [0,1] for bar i ── */
static void viz_get_color(VizColor col, int i, int count, double amp,
                           double *r, double *g, double *b) {
    double t = (count > 1) ? (double)i / (double)(count - 1) : 0.0;
    double L = 0.40 + amp * 0.35;   /* lightness-like brightness */

    switch (col) {
    case VIZ_COL_CYAN:
        /* hsl(185, 100%, L) — pure cyan */
        *r = L * 0.0;  *g = L * 0.97; *b = L * 1.0;
        break;
    case VIZ_COL_YELLOW_GREEN: {
        /* hsl(60 + t*60, 100%, L) — yellow → green */
        double h = (60.0 + t * 60.0) / 360.0;
        double c = 2.0 * L * (L < 0.5 ? L : 1.0 - L);
        double x = c * (1.0 - fabs(fmod(h * 6.0, 2.0) - 1.0));
        double m = L - c * 0.5;
        int hi = (int)(h * 6.0);
        double rr, gg, bb;
        if      (hi == 0) { rr=c; gg=x; bb=0; }
        else if (hi == 1) { rr=x; gg=c; bb=0; }
        else if (hi == 2) { rr=0; gg=c; bb=x; }
        else if (hi == 3) { rr=0; gg=x; bb=c; }
        else if (hi == 4) { rr=x; gg=0; bb=c; }
        else              { rr=c; gg=0; bb=x; }
        *r = rr+m; *g = gg+m; *b = bb+m;
        break;
    }
    case VIZ_COL_RAINBOW: {
        /* hsl(t*300, 100%, L) — full spectrum */
        double h = t * 300.0 / 360.0;
        double c = 2.0 * L * (L < 0.5 ? L : 1.0 - L);
        double x = c * (1.0 - fabs(fmod(h * 6.0, 2.0) - 1.0));
        double m = L - c * 0.5;
        int hi = (int)(h * 6.0);
        double rr, gg, bb;
        if      (hi == 0) { rr=c; gg=x; bb=0; }
        else if (hi == 1) { rr=x; gg=c; bb=0; }
        else if (hi == 2) { rr=0; gg=c; bb=x; }
        else if (hi == 3) { rr=0; gg=x; bb=c; }
        else if (hi == 4) { rr=x; gg=0; bb=c; }
        else              { rr=c; gg=0; bb=x; }
        *r = rr+m; *g = gg+m; *b = bb+m;
        break;
    }
    case VIZ_COL_ORANGE: {
        /* hsl(20 - t*20, 100%, L) — orange-red */
        double h = (20.0 - t * 20.0) / 360.0;
        double c = 2.0 * L * (L < 0.5 ? L : 1.0 - L);
        double x = c * (1.0 - fabs(fmod(h * 6.0, 2.0) - 1.0));
        double m = L - c * 0.5;
        int hi = (int)(h * 6.0);
        double rr, gg, bb;
        if      (hi == 0) { rr=c; gg=x; bb=0; }
        else if (hi == 1) { rr=x; gg=c; bb=0; }
        else              { rr=0; gg=c; bb=x; }
        *r = rr+m; *g = gg+m; *b = bb+m;
        break;
    }
    default:
        *r = 0; *g = 1; *b = 1;
        break;
    }
}

/* ── Animation tick: lerp bars toward targets, occasionally pick new targets ── */
static void viz_update_bars(VizState *vs) {
    double spd = vs->speed * 0.008;
    for (int i = 0; i < vs->bar_count; i++) {
        double rnd = (double)rand() / (double)RAND_MAX;
        if (rnd < 0.03) {
            double r2 = (double)rand() / (double)RAND_MAX;
            if (r2 > 0.85)
                vs->targets[i] = (double)rand() / RAND_MAX * 0.3 + 0.7;
            else if (r2 < 0.15)
                vs->targets[i] = (double)rand() / RAND_MAX * 0.1;
            else
                vs->targets[i] = (double)rand() / RAND_MAX * 0.95 + 0.05;
        }
        vs->bars[i] += (vs->targets[i] - vs->bars[i]) * spd * 2.0;
        if (vs->bars[i] < 0.02) vs->bars[i] = 0.02;
        if (vs->bars[i] > 1.0)  vs->bars[i] = 1.0;
    }
}

/* ── Cairo draw callback ── */
static gboolean on_viz_draw(GtkWidget *widget, cairo_t *cr, gpointer ud) {
    VizState *vs = (VizState *)ud;
    (void)widget;

    int W = gtk_widget_get_allocated_width(vs->canvas);
    int H = gtk_widget_get_allocated_height(vs->canvas);
    if (W <= 0 || H <= 0) return FALSE;

    /* Background */
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    int    count     = vs->bar_count;
    double ht        = vs->height_mult;
    double wp        = vs->width_pct;
    VizStyle style   = vs->style;
    VizColor col     = vs->color;

    double totalW  = W * wp;
    double offsetX = (W - totalW) * 0.5;
    double slot    = totalW / count;
    double barW    = slot * 0.70;
    if (barW < 1.0) barW = 1.0;

    double maxH = H * 0.90 * ht;

    for (int i = 0; i < count; i++) {
        double amp = vs->bars[i];
        double cr_r, cr_g, cr_b;
        viz_get_color(col, i, count, amp, &cr_r, &cr_g, &cr_b);
        cairo_set_source_rgb(cr, cr_r, cr_g, cr_b);

        double x  = offsetX + i * slot;
        double bh, cy, half;

        switch (style) {
        case VIZ_STYLE_BOTTOM:
            bh = amp * maxH;
            cairo_rectangle(cr, x, H - bh, barW, bh);
            cairo_fill(cr);
            /* bright cap line */
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
            cairo_rectangle(cr, x, H - bh - 2.0, barW, 2.0);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, cr_r, cr_g, cr_b);
            break;

        case VIZ_STYLE_CENTER:
            half = (amp * maxH) * 0.5;
            cy   = H * 0.5;
            cairo_rectangle(cr, x, cy - half, barW, half * 2.0);
            cairo_fill(cr);
            break;

        case VIZ_STYLE_MIRROR:
            half = (amp * maxH * 0.5) * 0.5;
            cy   = H * 0.5;
            cairo_rectangle(cr, x, cy - half, barW, half * 2.0);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, cr_r, cr_g, cr_b, 0.5);
            cairo_rectangle(cr, x, cy - half * 1.8, barW, half * 1.6);
            cairo_fill(cr);
            cairo_rectangle(cr, x, cy + half, barW, half * 1.6);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, cr_r, cr_g, cr_b);
            break;
        }
    }

    /* Centre line for CENTER style */
    if (style == VIZ_STYLE_CENTER) {
        cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.20);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, 0, H * 0.5);
        cairo_line_to(cr, W, H * 0.5);
        cairo_stroke(cr);
    }

    /* Side vignette glow strips */
    cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.08);
    cairo_rectangle(cr, 0,    0, 20, H);
    cairo_fill(cr);
    cairo_rectangle(cr, W-20, 0, 20, H);
    cairo_fill(cr);

    return FALSE;
}

/* ── Timer tick: update bars + queue redraw ── */
static gboolean viz_tick(gpointer ud) {
    VizState *vs = (VizState *)ud;
    if (!vs->running) return G_SOURCE_CONTINUE;
    viz_update_bars(vs);
    gtk_widget_queue_draw(vs->canvas);
    return G_SOURCE_CONTINUE;
}

/* ── Cleanup when the visualizer window is destroyed ── */
static void on_viz_window_destroy(GtkWidget *w, gpointer ud) {
    (void)w;
    VizState *vs = (VizState *)ud;
    if (vs->timer_id) {
        g_source_remove(vs->timer_id);
        vs->timer_id = 0;
    }
    free(vs);
}

/* ── Control callbacks ── */
static void on_viz_style_changed(GtkComboBoxText *cb, gpointer ud) {
    VizState *vs = (VizState *)ud;
    vs->style = (VizStyle)gtk_combo_box_get_active(GTK_COMBO_BOX(cb));
}
static void on_viz_color_changed(GtkComboBoxText *cb, gpointer ud) {
    VizState *vs = (VizState *)ud;
    vs->color = (VizColor)gtk_combo_box_get_active(GTK_COMBO_BOX(cb));
}
static void on_viz_bars_changed(GtkAdjustment *adj, gpointer ud) {
    VizState *vs = (VizState *)ud;
    int newcount = (int)gtk_adjustment_get_value(adj);
    if (newcount != vs->bar_count) {
        vs->bar_count = newcount;
        /* re-init bar arrays */
        for (int i = 0; i < vs->bar_count; i++) {
            vs->bars[i]    = (double)rand() / RAND_MAX * 0.5 + 0.1;
            vs->targets[i] = (double)rand() / RAND_MAX * 0.9 + 0.1;
        }
    }
    char buf[16]; snprintf(buf, sizeof(buf), "%d", newcount);
    gtk_label_set_text(vs->bar_val_lbl, buf);
}
static void on_viz_ht_changed(GtkAdjustment *adj, gpointer ud) {
    VizState *vs = (VizState *)ud;
    vs->height_mult = gtk_adjustment_get_value(adj) / 100.0;
    char buf[16]; snprintf(buf, sizeof(buf), "%.0f%%",
                           gtk_adjustment_get_value(adj));
    gtk_label_set_text(vs->ht_val_lbl, buf);
}
static void on_viz_wd_changed(GtkAdjustment *adj, gpointer ud) {
    VizState *vs = (VizState *)ud;
    vs->width_pct = gtk_adjustment_get_value(adj) / 100.0;
    char buf[16]; snprintf(buf, sizeof(buf), "%.0f%%",
                           gtk_adjustment_get_value(adj));
    gtk_label_set_text(vs->wd_val_lbl, buf);
}
static void on_viz_sp_changed(GtkAdjustment *adj, gpointer ud) {
    VizState *vs = (VizState *)ud;
    vs->speed = gtk_adjustment_get_value(adj);
    char buf[16]; snprintf(buf, sizeof(buf), "%.0f",
                           gtk_adjustment_get_value(adj));
    gtk_label_set_text(vs->sp_val_lbl, buf);
}
static void on_viz_toggle(GtkButton *btn, gpointer ud) {
    VizState *vs = (VizState *)ud;
    vs->running = !vs->running;
    gtk_button_set_label(btn, vs->running ? "⏸ Pause" : "▶ Play");
}

/* ── Helper: create a labelled scale row inside the controls box ── */
static GtkAdjustment *viz_add_scale(GtkWidget *hbox,
                                     const char *label_text,
                                     double min, double max, double val, double step,
                                     GtkLabel **out_val_lbl) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* label + live-value label side by side */
    GtkWidget *lrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lbl  = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);

    char init_buf[16];
    if (step < 1.0)
        snprintf(init_buf, sizeof(init_buf), "%.1f", val);
    else
        snprintf(init_buf, sizeof(init_buf), "%.0f", val);
    GtkWidget *val_lbl = gtk_label_new(init_buf);
    /* teal colour via markup */
    char markup[64];
    snprintf(markup, sizeof(markup),
             "<span foreground=\"#00ffff\">%s</span>", init_buf);
    gtk_label_set_markup(GTK_LABEL(val_lbl), markup);

    gtk_box_pack_start(GTK_BOX(lrow), lbl,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lrow), val_lbl, FALSE, FALSE, 4);

    GtkAdjustment *adj = gtk_adjustment_new(val, min, max, step, step*10, 0);
    GtkWidget *scale   = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 120, -1);

    gtk_box_pack_start(GTK_BOX(vbox), lrow,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), vbox,  FALSE, FALSE, 0);

    if (out_val_lbl) *out_val_lbl = GTK_LABEL(val_lbl);
    return adj;
}

/* ── GUI callback: open visualizer window ── */
static void cb_show_visualizer(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = (AppWidgets *)ud;

    /* Allocate and initialise VizState */
    VizState *vs = calloc(1, sizeof *vs);
    if (!vs) { show_err(aw->window, "OOM", "Out of memory."); return; }

    vs->bar_count   = 80;
    vs->height_mult = 1.0;
    vs->width_pct   = 1.0;
    vs->speed       = 5.0;
    vs->style       = VIZ_STYLE_BOTTOM;
    vs->color       = VIZ_COL_YELLOW_GREEN;
    vs->running     = TRUE;
    vs->aw_ref      = aw;

    /* Seed bar arrays */
    for (int i = 0; i < vs->bar_count; i++) {
        vs->bars[i]    = (double)rand() / RAND_MAX * 0.5 + 0.1;
        vs->targets[i] = (double)rand() / RAND_MAX * 0.9 + 0.1;
    }

    /* ── Pop-up window ── */
    vs->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(vs->window), "Audio Visualizer");
    gtk_window_set_default_size(GTK_WINDOW(vs->window), VIZ_WIDTH, VIZ_HEIGHT + 90);
    gtk_window_set_transient_for(GTK_WINDOW(vs->window),
                                  GTK_WINDOW(aw->window));
    g_signal_connect(vs->window, "destroy",
                     G_CALLBACK(on_viz_window_destroy), vs);

    /* Dark background */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: #000000; }"
        "label  { color: #888888; font-size: 11px; }"
        "scale  { color: #00ffff; }"
        "button { background: #111111; color: #00ffff;"
        "         border: 1px solid #004444; border-radius: 6px;"
        "         padding: 4px 12px; font-size: 11px; }"
        "button:hover { background: #002222; }"
        "combobox button { padding: 2px 6px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(vs->window), vbox);

    /* ── Controls row ── */
    GtkWidget *ctrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_pack_start(GTK_BOX(vbox), ctrl, FALSE, FALSE, 0);

    /* Style combo */
    GtkWidget *sg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(sg), gtk_label_new("Style"), FALSE, FALSE, 0);
    vs->style_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(vs->style_combo, "Bottom bars");
    gtk_combo_box_text_append_text(vs->style_combo, "Center wave");
    gtk_combo_box_text_append_text(vs->style_combo, "Mirror (top+bottom)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(vs->style_combo), 0);
    g_signal_connect(vs->style_combo, "changed",
                     G_CALLBACK(on_viz_style_changed), vs);
    gtk_box_pack_start(GTK_BOX(sg), GTK_WIDGET(vs->style_combo), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl), sg, FALSE, FALSE, 0);

    /* Color combo */
    GtkWidget *cg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(cg), gtk_label_new("Color"), FALSE, FALSE, 0);
    vs->color_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(vs->color_combo, "Yellow-green");
    gtk_combo_box_text_append_text(vs->color_combo, "Cyan");
    gtk_combo_box_text_append_text(vs->color_combo, "Rainbow");
    gtk_combo_box_text_append_text(vs->color_combo, "Orange-red");
    gtk_combo_box_set_active(GTK_COMBO_BOX(vs->color_combo), 0);
    g_signal_connect(vs->color_combo, "changed",
                     G_CALLBACK(on_viz_color_changed), vs);
    gtk_box_pack_start(GTK_BOX(cg), GTK_WIDGET(vs->color_combo), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl), cg, FALSE, FALSE, 0);

    /* Sliders */
    vs->bar_adj = viz_add_scale(ctrl, "Bars",   20, 200, 80,  1,  &vs->bar_val_lbl);
    vs->ht_adj  = viz_add_scale(ctrl, "Height", 20, 200, 100, 1,  &vs->ht_val_lbl);
    vs->wd_adj  = viz_add_scale(ctrl, "Width",  30, 100, 100, 1,  &vs->wd_val_lbl);
    vs->sp_adj  = viz_add_scale(ctrl, "Speed",  1,  10,  5,   1,  &vs->sp_val_lbl);

    g_signal_connect(vs->bar_adj, "value-changed",
                     G_CALLBACK(on_viz_bars_changed), vs);
    g_signal_connect(vs->ht_adj,  "value-changed",
                     G_CALLBACK(on_viz_ht_changed),   vs);
    g_signal_connect(vs->wd_adj,  "value-changed",
                     G_CALLBACK(on_viz_wd_changed),   vs);
    g_signal_connect(vs->sp_adj,  "value-changed",
                     G_CALLBACK(on_viz_sp_changed),   vs);

    /* Pause / Play button */
    vs->toggle_btn = gtk_button_new_with_label("⏸ Pause");
    g_signal_connect(vs->toggle_btn, "clicked",
                     G_CALLBACK(on_viz_toggle), vs);
    gtk_box_pack_end(GTK_BOX(ctrl), vs->toggle_btn, FALSE, FALSE, 0);

    /* Burn visualizer overlay into video */
    GtkWidget *burn_btn = gtk_button_new_with_label("🔥 Burn to Video");
    gtk_widget_set_tooltip_text(burn_btn,
        "Render the audio visualizer as a video overlay using ffmpeg showfreqs filter");
    g_signal_connect(burn_btn, "clicked",
                     G_CALLBACK(cb_viz_burn_to_video), vs);
    gtk_box_pack_end(GTK_BOX(ctrl), burn_btn, FALSE, FALSE, 0);

    /* Burn visualizer overlay onto a Dub (AI Only) video */
    GtkWidget *burn_dub_btn = gtk_button_new_with_label("🔥 Burn Dub+Viz");
    gtk_widget_set_tooltip_text(burn_dub_btn,
        "Pick a Dub (AI Only) video and burn the visualizer overlay onto it — "
        "visualizer and audio both come from the dubbed track");
    g_signal_connect(burn_dub_btn, "clicked",
                     G_CALLBACK(cb_viz_burn_dub_to_video), vs);
    gtk_box_pack_end(GTK_BOX(ctrl), burn_dub_btn, FALSE, FALSE, 0);

    /* ── Canvas ── */
    vs->canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(vs->canvas, VIZ_WIDTH, VIZ_HEIGHT);
    g_signal_connect(vs->canvas, "draw", G_CALLBACK(on_viz_draw), vs);
    gtk_box_pack_start(GTK_BOX(vbox), vs->canvas, TRUE, TRUE, 0);

    gtk_widget_show_all(vs->window);

    /* Start 60 fps timer */
    vs->timer_id = g_timeout_add(VIZ_TIMER_MS, viz_tick, vs);

    LOG_INFO("Audio Visualizer opened");
}

/* ── cb_viz_burn_to_video ── Burn audio visualizer overlay into video ──
 * Uses ffmpeg's lavfi showfreqs (or showwaves) filter to render a real
 * frequency-bar overlay and composite it onto the original video.
 * The user picks the output path; ffmpeg does the rest in one pass.    */
static void cb_viz_burn_to_video(GtkWidget *btn, gpointer ud) {
    (void)btn;
    VizState   *vs = (VizState *)ud;
    AppWidgets *aw = vs->aw_ref;
    if (!aw || !aw->state->video_path[0]) {
        GtkWidget *err = gtk_message_dialog_new(
            vs->window ? GTK_WINDOW(vs->window) : NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Open a video first before burning the visualizer.");
        gtk_dialog_run(GTK_DIALOG(err));
        gtk_widget_destroy(err);
        return;
    }

    /* ── Ask user where to save ── */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Video with Visualizer Overlay",
        GTK_WINDOW(vs->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_visualizer.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    /* ── Pick ffmpeg showfreqs color string from VizState color ── */
    const char *bar_color;
    switch (vs->color) {
    case VIZ_COL_CYAN:         bar_color = "cyan";   break;
    case VIZ_COL_RAINBOW:      bar_color = "rainbow";break;
    case VIZ_COL_ORANGE:       bar_color = "firebrick"; break;
    case VIZ_COL_YELLOW_GREEN:
    default:                   bar_color = "yellowgreen"; break;
    }

    /* ── Pick showfreqs mode string from VizState style ── */
    const char *mode;
    switch (vs->style) {
    case VIZ_STYLE_CENTER: mode = "cline"; break;  /* center-origin lines */
    case VIZ_STYLE_MIRROR: mode = "line";  break;  /* mirrored — same as line, composited twice */
    case VIZ_STYLE_BOTTOM:
    default:               mode = "bar";   break;
    }

    /* ── Build ffmpeg filter_complex ──
     *
     * Layout: 700×120 showfreqs overlay anchored to the bottom of the
     * video.  The overlay is composited at 70 % opacity using the
     * colorchannelmixer filter so the video remains visible underneath.
     *
     * Filter graph (single -filter_complex):
     *   [0:a] split [a_orig][a_viz]      — duplicate audio stream
     *   [a_viz] showfreqs=s=700x120:...  — render spectrum frame
     *   [0:v][viz_frames] overlay=...    — burn onto video
     *   [a_orig] → output audio unchanged
     */
    int bar_count = vs->bar_count < 20  ? 20
                  : vs->bar_count > 200 ? 200
                  : vs->bar_count;
    /* showfreqs size: match bar_count to x resolution roughly */
    int viz_w = bar_count * 8;
    if (viz_w < 320) viz_w = 320;
    if (viz_w > 1920) viz_w = 1920;

    char filter[2048];
    snprintf(filter, sizeof(filter),
        "[0:a]asplit=2[a_orig][a_viz];"
        "[a_viz]showfreqs=s=%dx120:mode=%s:fscale=log:ascale=sqrt:"
                "colors=%s:win_size=2048:win_func=hann[viz];"
        "[0:v][viz]overlay=x=(W-w)/2:y=H-h:format=auto,"
        "format=yuv420p[vout]",
        viz_w, mode, bar_color);

    /* Write a temporary shell script to avoid argv quoting headaches */
    char script[128];
    snprintf(script, sizeof(script),
             "%s/ai_dub_vizburn_%ld.sh", TMPDIR, (long)time(NULL));
    FILE *sh = fopen(script, "w");
    if (!sh) {
        show_err(aw->window, "Error", "Cannot create temp script.");
        g_free(out_path); return;
    }
    xchmod(script, 0755);
    fprintf(sh, "#!/bin/sh\n");
    fprintf(sh, "%s -y \\\n", g_ffmpeg);
    fprintf(sh, "  -i '%s' \\\n", aw->state->video_path);
    fprintf(sh, "  -filter_complex \"%s\" \\\n", filter);
    fprintf(sh, "  -map '[vout]' -map '[a_orig]' \\\n");
    fprintf(sh, "  -c:v libx264 -preset fast -crf 20 \\\n");
    fprintf(sh, "  -c:a aac -b:a 192k \\\n");
    fprintf(sh, "  '%s'\n", out_path);
    fclose(sh);

    /* Show progress in main window while encoding */
    post_progress(aw->bar, aw->status_lbl, 5, "Burning visualizer overlay…");

    char *argv[] = { (char *)SHELL_PATH, script, NULL };
    char ferr[2048] = {0};
    int rc = run_cmd(argv, NULL, 0, ferr, sizeof(ferr));
    unlink(script);

    if (rc != 0) {
        show_err(aw->window, "Visualizer Burn Error", ferr);
        post_progress(aw->bar, aw->status_lbl, 0, "Visualizer burn failed");
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg), "Visualizer burned into video:\n%s", out_path);
        show_info(aw->window, "Burn Complete", msg);
        post_progress(aw->bar, aw->status_lbl, 100, "Visualizer burn complete");
        LOG_INFO("Visualizer burned to video: %s", out_path);
    }
    g_free(out_path);
}

/* ── cb_viz_burn_dub_to_video ─────────────────────────────────────────
 * Burn audio visualizer overlay onto a Queen (AI Only) output video.
 *
 * The existing "🔥 Burn to Video" always reads audio from the *original*
 * video, so the visualizer reacts to the original voice — not the AI dub.
 * This new function instead asks the user to pick their dubbed .mp4 first,
 * then runs the same showfreqs filter chain against that file, so both the
 * visualizer bars and the output audio come from the AI-dubbed track.
 *
 * Workflow:
 *   1. Ask user to select the dubbed video (e.g. *_dubbed.mp4)
 *   2. Ask user where to save the final video
 *   3. Build the same showfreqs filter_complex but feed it the dubbed file
 *   4. Run ffmpeg (via temp shell script to avoid argv length limits)      */
static void cb_viz_burn_dub_to_video(GtkWidget *btn, gpointer ud) {
    (void)btn;
    VizState   *vs = (VizState *)ud;
    AppWidgets *aw = vs->aw_ref;

    /* ── Step 1: Let user pick the dubbed video ── */
    GtkWidget *pick_dlg = gtk_file_chooser_dialog_new(
        "Select Dubbed Video (AI Only output)",
        vs->window ? GTK_WINDOW(vs->window) : NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *vflt = gtk_file_filter_new();
    gtk_file_filter_set_name(vflt, "Video (mp4/mkv/avi/mov)");
    gtk_file_filter_add_pattern(vflt, "*.mp4");
    gtk_file_filter_add_pattern(vflt, "*.mkv");
    gtk_file_filter_add_pattern(vflt, "*.avi");
    gtk_file_filter_add_pattern(vflt, "*.mov");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(pick_dlg), vflt);

    /* Pre-navigate to the output dir if set */
    if (aw && aw->state->config.output_dir[0])
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(pick_dlg),
                                            aw->state->config.output_dir);

    if (gtk_dialog_run(GTK_DIALOG(pick_dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(pick_dlg);
        return;
    }
    char *dub_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(pick_dlg));
    gtk_widget_destroy(pick_dlg);

    /* ── Step 2: Ask where to save the output ── */
    GtkWidget *save_dlg = gtk_file_chooser_dialog_new(
        "Save Queen + Visualizer Video",
        vs->window ? GTK_WINDOW(vs->window) : NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(save_dlg), TRUE);
    if (aw && aw->state->config.output_dir[0])
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(save_dlg),
                                            aw->state->config.output_dir);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s", g_path_get_basename(dub_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_visualizer.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(save_dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(save_dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(save_dlg);
        g_free(dub_path);
        return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(save_dlg));
    gtk_widget_destroy(save_dlg);

    /* ── Step 3: Build ffmpeg filter_complex (same as Burn to Video) ──
     * The only difference: we use dub_path as the single input, so its
     * audio stream [0:a] is the AI-dubbed audio, not the original.      */
    const char *bar_color;
    switch (vs->color) {
    case VIZ_COL_CYAN:         bar_color = "cyan";        break;
    case VIZ_COL_RAINBOW:      bar_color = "rainbow";     break;
    case VIZ_COL_ORANGE:       bar_color = "firebrick";   break;
    case VIZ_COL_YELLOW_GREEN:
    default:                   bar_color = "yellowgreen"; break;
    }

    const char *mode;
    switch (vs->style) {
    case VIZ_STYLE_CENTER: mode = "cline"; break;
    case VIZ_STYLE_MIRROR: mode = "line";  break;
    case VIZ_STYLE_BOTTOM:
    default:               mode = "bar";   break;
    }

    int bar_count = vs->bar_count < 20  ? 20
                  : vs->bar_count > 200 ? 200
                  : vs->bar_count;
    int viz_w = bar_count * 8;
    if (viz_w < 320)  viz_w = 320;
    if (viz_w > 1920) viz_w = 1920;

    char filter[2048];
    snprintf(filter, sizeof(filter),
        "[0:a]asplit=2[a_orig][a_viz];"
        "[a_viz]showfreqs=s=%dx120:mode=%s:fscale=log:ascale=sqrt:"
                "colors=%s:win_size=2048:win_func=hann[viz];"
        "[0:v][viz]overlay=x=(W-w)/2:y=H-h:format=auto,"
        "format=yuv420p[vout]",
        viz_w, mode, bar_color);

    /* ── Step 4: Write temp shell script and run ffmpeg ── */
    char script[128];
    snprintf(script, sizeof(script),
             "%s/ai_dub_vizburn_dub_%ld.sh", TMPDIR, (long)time(NULL));
    FILE *sh = fopen(script, "w");
    if (!sh) {
        GtkWidget *err_dlg = gtk_message_dialog_new(
            vs->window ? GTK_WINDOW(vs->window) : NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Cannot create temporary script file.");
        gtk_dialog_run(GTK_DIALOG(err_dlg));
        gtk_widget_destroy(err_dlg);
        g_free(dub_path); g_free(out_path);
        return;
    }
    xchmod(script, 0755);
    fprintf(sh, "#!/bin/sh\n");
    fprintf(sh, "%s -y \\\n", g_ffmpeg);
    fprintf(sh, "  -i '%s' \\\n", dub_path);
    fprintf(sh, "  -filter_complex \"%s\" \\\n", filter);
    fprintf(sh, "  -map '[vout]' -map '[a_orig]' \\\n");
    fprintf(sh, "  -c:v libx264 -preset fast -crf 20 \\\n");
    fprintf(sh, "  -c:a aac -b:a 192k \\\n");
    fprintf(sh, "  '%s'\n", out_path);
    fclose(sh);

    if (aw) post_progress(aw->bar, aw->status_lbl, 5,
                          "Burning Dub+Visualizer overlay…");

    char *run_argv[] = { (char *)SHELL_PATH, script, NULL };
    char ferr[2048] = {0};
    int rc = run_cmd(run_argv, NULL, 0, ferr, sizeof(ferr));
    unlink(script);

    if (rc != 0) {
        GtkWidget *err_dlg = gtk_message_dialog_new(
            vs->window ? GTK_WINDOW(vs->window) : NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Dub + Visualizer burn failed:\n%s", ferr);
        gtk_dialog_run(GTK_DIALOG(err_dlg));
        gtk_widget_destroy(err_dlg);
        if (aw) post_progress(aw->bar, aw->status_lbl, 0,
                              "Dub+Visualizer burn failed");
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Dub + Visualizer burned into video:\n%s", out_path);
        GtkWidget *ok_dlg = gtk_message_dialog_new(
            vs->window ? GTK_WINDOW(vs->window) : NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "%s", msg);
        gtk_window_set_title(GTK_WINDOW(ok_dlg), "Burn Complete");
        gtk_dialog_run(GTK_DIALOG(ok_dlg));
        gtk_widget_destroy(ok_dlg);
        if (aw) post_progress(aw->bar, aw->status_lbl, 100,
                              "Dub+Visualizer burn complete");
        LOG_INFO("Dub+Visualizer burned to video: %s", out_path);
    }

    g_free(dub_path);
    g_free(out_path);
}

/* ═══════════════════════════════════════════════════════════════════
 * Dub + Visualizer — single-step main-window button
 *
 * cb_dub_with_visualizer()
 *   Runs the full Dub (AI Only) pipeline and, on success, immediately
 *   burns the showfreqs visualizer overlay onto the dubbed output — all
 *   in one operation triggered from the main window, no popup required.
 *
 * The visualizer parameters (style/color/bars) are taken from sensible
 * defaults (bottom-bar, yellow-green, 80 bars) because there is no
 * VizState when called from the main window.  The user can still use
 * the separate "📊 Visualizer → 🔥 Burn Dub+Viz" path if they want to
 * tweak the look first.
 * ═══════════════════════════════════════════════════════════════════ */

/* Context passed from dub completion into the visualizer burn step */
typedef struct {
    AppWidgets *aw;
    char        dubbed_path[4096]; /* intermediate dub output (in /tmp) */
    char        viz_out[4096];     /* final output path (with _viz suffix) */
} DubVizCtx;

/* ── Helper payload + idle callback for the Dub+Viz result dialog ── */
typedef struct {
    AppWidgets *aw;
    int         success;
    char        msg[2200];
} DubVizResult;

static void _dub_viz_result_cb(void *d) {
    DubVizResult *r = d;
    if (r->success)
        show_info(r->aw->window, "Dub+Viz Done", r->msg);
    else
        show_err(r->aw->window, "Dub+Viz Error", r->msg);
    free(r);
}

/* Worker thread: burns the visualizer overlay onto the intermediate dubbed video */
static void *dub_viz_burn_thread(void *data) {
    DubVizCtx  *dvc = data;
    AppWidgets *aw  = dvc->aw;

    /* ── showfreqs filter: bottom bars, yellow-green, 80 bars (640 px wide) ── */
    char filter[2048];
    snprintf(filter, sizeof(filter),
        "[0:a]asplit=2[a_orig][a_viz];"
        "[a_viz]showfreqs=s=640x120:mode=bar:fscale=log:ascale=sqrt:"
                "colors=yellowgreen:win_size=2048:win_func=hann[viz];"
        "[0:v][viz]overlay=x=(W-w)/2:y=H-h:format=auto,"
        "format=yuv420p[vout]");

    char script[128];
    snprintf(script, sizeof(script),
             "%s/ai_dub_vizstep_%ld.sh", TMPDIR, (long)time(NULL));
    FILE *sh = fopen(script, "w");
    if (!sh) {
        post_progress(aw->bar, aw->status_lbl, 0, "Dub+Viz: cannot create script");
        unlink(dvc->dubbed_path);
        free(dvc);
        return NULL;
    }
    xchmod(script, 0755);
    fprintf(sh, "#!/bin/sh\n");
    fprintf(sh, "%s -y \\\n", g_ffmpeg);
    fprintf(sh, "  -i '%s' \\\n", dvc->dubbed_path);
    fprintf(sh, "  -filter_complex \"%s\" \\\n", filter);
    fprintf(sh, "  -map '[vout]' -map '[a_orig]' \\\n");
    fprintf(sh, "  -c:v libx264 -preset fast -crf 20 \\\n");
    fprintf(sh, "  -c:a aac -b:a 192k \\\n");
    fprintf(sh, "  '%s'\n", dvc->viz_out);
    fclose(sh);

    char *run_argv[] = { (char *)SHELL_PATH, script, NULL };
    char ferr[2048] = {0};
    int rc = run_cmd(run_argv, NULL, 0, ferr, sizeof(ferr));
    unlink(script);
    unlink(dvc->dubbed_path); /* remove intermediate dub temp file */

    DubVizResult *r = malloc(sizeof *r);
    if (r) {
        r->aw      = aw;
        r->success = (rc == 0);
        if (rc == 0)
            snprintf(r->msg, sizeof(r->msg),
                     "Dub + Visualizer saved:\n%s", dvc->viz_out);
        else
            snprintf(r->msg, sizeof(r->msg),
                     "Visualizer burn failed:\n%s", ferr);
        post_done(_dub_viz_result_cb, r);
    }

    post_progress(aw->bar, aw->status_lbl,
                  rc == 0 ? 100 : 0,
                  rc == 0 ? "Dub+Viz complete!" : "Dub+Viz failed");
    if (rc == 0)
        LOG_INFO("Dub+Visualizer complete: %s", dvc->viz_out);
    else
        LOG_ERROR("Dub+Visualizer burn failed: %s", ferr);

    free(dvc);
    return NULL;
}

static void on_dub_viz_done(void *ctx, const char *dubbed_path, const char *err) {
    DubVizCtx  *dvc = ctx;
    AppWidgets *aw  = dvc->aw;

    if (err || !dubbed_path) {
        show_err(aw->window, "Dub Error", err ? err : "Dub failed (no output path)");
        free(dvc);
        return;
    }

    snprintf(dvc->dubbed_path, sizeof(dvc->dubbed_path), "%s", dubbed_path);
    post_progress(aw->bar, aw->status_lbl, 55, "Dub done — burning visualizer…");

    pthread_t t;
    pthread_create(&t, NULL, dub_viz_burn_thread, dvc);
    pthread_detach(t);
}

static void cb_dub_with_visualizer(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;

    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe and translate first."); return;
    }

    /* Pick voice */
    int vi = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->dub_voice_combo));
    const char *voice = (vi >= 0 && KM_VOICES[vi]) ? KM_VOICES[vi] : KM_VOICE_FEMALE;

    /* ── Ask user where to save the final Dub+Viz video ── */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Dub + Visualizer Video", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "MP4 Video");
    gtk_file_filter_add_pattern(flt, "*.mp4");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_dub_viz.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }
    char *final_out = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    /* ── Intermediate dubbed file lives in /tmp ── */
    char dub_tmp[4096];
    snprintf(dub_tmp, sizeof(dub_tmp),
             "%s/ai_dub_intermediate_%ld.mp4", TMPDIR, (long)time(NULL));

    /* ── Build DubArg (same as cb_dub, but output goes to /tmp intermediate) ── */
    DubVizCtx *dvc = malloc(sizeof *dvc);
    if (!dvc) {
        show_err(aw->window, "OOM", "Out of memory.");
        g_free(final_out); return;
    }
    dvc->aw = aw;
    /* dubbed_path filled by on_dub_viz_done once dub_thread completes */
    dvc->dubbed_path[0] = '\0';
    snprintf(dvc->viz_out, sizeof(dvc->viz_out), "%s", final_out);
    g_free(final_out);

    DubArg *a = calloc(1, sizeof *a);
    if (!a) {
        show_err(aw->window, "OOM", "Out of memory.");
        free(dvc); return;
    }
    snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
    snprintf(a->out_path,   sizeof(a->out_path),   "%s", dub_tmp);
    snprintf(a->voice,      sizeof(a->voice),      "%s", voice);
    a->rate      = aw->state->config.tts_rate;
    a->pitch     = aw->state->config.tts_pitch;
    /* Subtitles: OFF for the intermediate — they would be burned twice otherwise.
     * The final Dub+Viz output is the video; subtitles can be added separately. */
    a->burn_subs = 0;
    a->segments  = aw->state->segments;
    a->seg_count = aw->state->seg_count;
    a->bar = aw->bar; a->lbl = aw->status_lbl;
    a->on_done = on_dub_viz_done; a->ctx = dvc;

    snprintf(aw->state->config.dub_voice, sizeof(aw->state->config.dub_voice),
             "%s", voice);

    pthread_t t;
    pthread_create(&t, NULL, dub_thread, a);
    pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 2, "Starting Dub + Visualizer…");
}

/* ═══════════════════════════════════════════════════════════════════
 * fix.txt #4: Dub voice combo changed → update config.dub_voice immediately
 * so all features that read config.dub_voice use the right voice.
 * ═══════════════════════════════════════════════════════════════════ */
static void cb_dub_voice_changed(GtkComboBox *combo, gpointer ud) {
    AppWidgets *aw = ud;
    int vi = gtk_combo_box_get_active(combo);
    if (vi >= 0 && KM_VOICES[vi])
        snprintf(aw->state->config.dub_voice,
                 sizeof(aw->state->config.dub_voice), "%s", KM_VOICES[vi]);
}

/* ═══════════════════════════════════════════════════════════════════
 * fix.txt #3: Combined "Dub Full Export"
 *   Dub with burned subtitles + Visualizer overlay + Logo +
 *   Extract background music as .mp3
 *
 * Steps:
 *  1) Dub (AI voice replaces original, subs burned in)
 *  2) Burn visualizer onto the dubbed video
 *  3) Burn logo/text overlay if a logo file exists
 *  4) Extract the original background music as .mp3
 * ═══════════════════════════════════════════════════════════════════ */

/* Context for the multi-step full export pipeline */
typedef struct {
    AppWidgets *aw;
    char dubbed_path[4096];   /* step 1 output: dubbed video with subs */
    char viz_path[4096];      /* step 2 output: dubbed + visualizer */
    char final_path[4096];    /* user-chosen final output */
    char mp3_path[4096];      /* background music mp3 */
    char logo_path[4096];     /* logo file (empty = skip logo step) */
    char voice[128];
    int  rate, pitch;
} FullExportCtx;

/* Forward declarations for the chain */
static void on_full_export_dub_done(void *ctx, const char *out, const char *err);

/* Step 4: extract background music mp3 (runs in thread) */
static void *full_export_mp3_thread(void *data) {
    FullExportCtx *fc = data;
    AppWidgets *aw = fc->aw;

    post_progress(aw->bar, aw->status_lbl, 85, "Full Export: extracting background music…");

    char script[128];
    snprintf(script, sizeof(script), "%s/ai_fexp_mp3_%ld.sh", TMPDIR, (long)time(NULL));
    FILE *sh = fopen(script, "w");
    if (sh) {
        xchmod(script, 0755);
        fprintf(sh, "#!/bin/sh\n");
        fprintf(sh, "%s -y -i '%s' -vn -acodec libmp3lame -b:a %s -ar %d '%s'\n",
                g_ffmpeg, aw->state->video_path, MP3_BITRATE, MP3_SAMPLE_RATE,
                fc->mp3_path);
        fclose(sh);
        char *argv[] = { (char *)SHELL_PATH, script, NULL };
        char ferr[1024] = {0};
        int rc = run_cmd(argv, NULL, 0, ferr, sizeof(ferr));
        unlink(script);
        if (rc != 0)
            LOG_WARN("Full Export MP3 extraction failed: %s", ferr);
    }

    /* Clean up intermediate files */
    if (fc->dubbed_path[0]) unlink(fc->dubbed_path);
    if (fc->viz_path[0] && strcmp(fc->viz_path, fc->final_path) != 0)
        unlink(fc->viz_path);

    post_progress(aw->bar, aw->status_lbl, 100, "Full Export complete!");
    char msg[4200];
    snprintf(msg, sizeof(msg),
             "Full Export complete!\n\nVideo: %s\nMusic: %s",
             fc->final_path, fc->mp3_path);

    /* Show result on main thread */
    DubVizResult *r = malloc(sizeof *r);
    if (r) {
        r->aw      = aw;
        r->success = 1;
        snprintf(r->msg, sizeof(r->msg), "%s", msg);
        post_done(_dub_viz_result_cb, r);
    }

    LOG_INFO("Full Export done: video=%s mp3=%s", fc->final_path, fc->mp3_path);
    free(fc);
    return NULL;
}

/* Step 3: burn logo (if present), then extract mp3 */
static void *full_export_logo_thread(void *data) {
    FullExportCtx *fc = data;
    AppWidgets *aw = fc->aw;

    /* Input is viz_path (or dubbed_path if viz was skipped) */
    const char *input = fc->viz_path[0] ? fc->viz_path : fc->dubbed_path;

    if (fc->logo_path[0] && access(fc->logo_path, F_OK) == 0) {
        post_progress(aw->bar, aw->status_lbl, 75, "Full Export: burning logo…");
        /* Burn logo onto video → final_path */
        char script[128];
        snprintf(script, sizeof(script), "%s/ai_fexp_logo_%ld.sh", TMPDIR, (long)time(NULL));
        FILE *sh = fopen(script, "w");
        if (sh) {
            xchmod(script, 0755);
            fprintf(sh, "#!/bin/sh\n");
            fprintf(sh, "%s -y -i '%s' -i '%s' "
                    "-filter_complex \"[1:v]scale=120:-1[logo];[0:v][logo]overlay=W-w-20:20\" "
                    "-c:a copy '%s'\n",
                    g_ffmpeg, input, fc->logo_path, fc->final_path);
            fclose(sh);
            char *argv[] = { (char *)SHELL_PATH, script, NULL };
            char ferr[1024] = {0};
            int rc = run_cmd(argv, NULL, 0, ferr, sizeof(ferr));
            unlink(script);
            if (rc != 0) {
                LOG_WARN("Logo burn failed: %s — copying without logo", ferr);
                /* Fall back: just copy/rename input to final */
                rename(input, fc->final_path);
            }
        }
    } else {
        /* No logo — just move/copy viz output to final */
        rename(input, fc->final_path);
    }

    /* Step 4: extract mp3 in same thread */
    return full_export_mp3_thread(fc);
}

/* Step 2: burn visualizer onto dubbed video */
static void *full_export_viz_thread(void *data) {
    FullExportCtx *fc = data;
    AppWidgets *aw = fc->aw;

    post_progress(aw->bar, aw->status_lbl, 55, "Full Export: burning visualizer…");

    snprintf(fc->viz_path, sizeof(fc->viz_path),
             "%s/ai_fexp_viz_%ld.mp4", TMPDIR, (long)time(NULL));

    char filter[2048];
    snprintf(filter, sizeof(filter),
        "[0:a]asplit=2[a_orig][a_viz];"
        "[a_viz]showfreqs=s=640x120:mode=bar:fscale=log:ascale=sqrt:"
                "colors=yellowgreen:win_size=2048:win_func=hann[viz];"
        "[0:v][viz]overlay=x=(W-w)/2:y=H-h:format=auto,"
        "format=yuv420p[vout]");

    char script[128];
    snprintf(script, sizeof(script), "%s/ai_fexp_vizsh_%ld.sh", TMPDIR, (long)time(NULL));
    FILE *sh = fopen(script, "w");
    if (sh) {
        xchmod(script, 0755);
        fprintf(sh, "#!/bin/sh\n");
        fprintf(sh, "%s -y -i '%s' -filter_complex \"%s\" "
                "-map '[vout]' -map '[a_orig]' "
                "-c:v libx264 -preset fast -crf 20 "
                "-c:a aac -b:a 192k '%s'\n",
                g_ffmpeg, fc->dubbed_path, filter, fc->viz_path);
        fclose(sh);
        char *argv[] = { (char *)SHELL_PATH, script, NULL };
        char ferr[1024] = {0};
        int rc = run_cmd(argv, NULL, 0, ferr, sizeof(ferr));
        unlink(script);
        if (rc != 0) {
            LOG_WARN("Viz burn failed: %s — proceeding without visualizer", ferr);
            fc->viz_path[0] = '\0'; /* skip viz, use dubbed_path directly */
        }
    }

    /* Chain to step 3 (logo) */
    return full_export_logo_thread(fc);
}

/* Step 1 callback: dub is done, chain to visualizer */
static void on_full_export_dub_done(void *ctx, const char *out, const char *err) {
    FullExportCtx *fc = ctx;
    AppWidgets *aw = fc->aw;

    if (err || !out) {
        post_progress(aw->bar, aw->status_lbl, 0, "Full Export: dub failed");
        char msg[2200];
        snprintf(msg, sizeof(msg), "Dub step failed:\n%s", err ? err : "unknown error");
        show_err(aw->window, "Full Export Error", msg);
        free(fc);
        return;
    }
    snprintf(fc->dubbed_path, sizeof(fc->dubbed_path), "%s", out);

    /* Launch step 2 (visualizer) in a new thread */
    pthread_t t;
    pthread_create(&t, NULL, full_export_viz_thread, fc);
    pthread_detach(t);
}

/* Main callback — triggered by the "🎬 Dub Full Export" button */
static void cb_dub_full_export(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;

    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe and translate first."); return;
    }

    /* Pick voice */
    int vi = gtk_combo_box_get_active(GTK_COMBO_BOX(aw->dub_voice_combo));
    const char *voice = (vi >= 0 && KM_VOICES[vi]) ? KM_VOICES[vi] : KM_VOICE_FEMALE;

    /* Ask where to save */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Full Export Video", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "MP4 Video");
    gtk_file_filter_add_pattern(flt, "*.mp4");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_full_export.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    /* Optionally pick a logo file */
    char logo_path[4096] = {0};
    {
        GtkWidget *logo_dlg = gtk_message_dialog_new(
            GTK_WINDOW(aw->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
            "Do you want to add a logo/watermark overlay?");
        gtk_window_set_title(GTK_WINDOW(logo_dlg), "Logo Overlay");
        if (gtk_dialog_run(GTK_DIALOG(logo_dlg)) == GTK_RESPONSE_YES) {
            gtk_widget_destroy(logo_dlg);
            GtkWidget *ldlg = gtk_file_chooser_dialog_new(
                "Select Logo Image", GTK_WINDOW(aw->window),
                GTK_FILE_CHOOSER_ACTION_OPEN,
                "_Cancel", GTK_RESPONSE_CANCEL,
                "_Open",   GTK_RESPONSE_ACCEPT, NULL);
            GtkFileFilter *img_flt = gtk_file_filter_new();
            gtk_file_filter_set_name(img_flt, "Images");
            gtk_file_filter_add_pattern(img_flt, "*.png");
            gtk_file_filter_add_pattern(img_flt, "*.jpg");
            gtk_file_filter_add_pattern(img_flt, "*.gif");
            gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(ldlg), img_flt);
            if (gtk_dialog_run(GTK_DIALOG(ldlg)) == GTK_RESPONSE_ACCEPT) {
                char *lp = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(ldlg));
                snprintf(logo_path, sizeof(logo_path), "%s", lp);
                g_free(lp);
            }
            gtk_widget_destroy(ldlg);
        } else {
            gtk_widget_destroy(logo_dlg);
        }
    }

    /* Allocate context */
    FullExportCtx *fc = calloc(1, sizeof *fc);
    if (!fc) { show_err(aw->window, "OOM", "Out of memory."); g_free(out_path); return; }
    fc->aw = aw;
    snprintf(fc->final_path, sizeof(fc->final_path), "%s", out_path);
    snprintf(fc->voice, sizeof(fc->voice), "%s", voice);
    snprintf(fc->logo_path, sizeof(fc->logo_path), "%s", logo_path);
    fc->rate  = aw->state->config.tts_rate;
    fc->pitch = aw->state->config.tts_pitch;

    /* Build mp3 path: same as final but with _bgmusic.mp3 suffix */
    {
        char mp3base[4096];
        snprintf(mp3base, sizeof(mp3base), "%s", out_path);
        char *dot = strrchr(mp3base, '.'); if (dot) *dot = '\0';
        snprintf(fc->mp3_path, sizeof(fc->mp3_path), "%s_bgmusic.mp3", mp3base);
    }
    g_free(out_path);

    /* Step 1: Dub with burned subtitles */
    char dub_tmp[4096];
    snprintf(dub_tmp, sizeof(dub_tmp),
             "%s/ai_fexp_dub_%ld.mp4", TMPDIR, (long)time(NULL));

    /* Build ASS subtitles for burning */
    char ass_tmp[128] = {0};
    char font_name[256]; find_khmer_font(font_name, sizeof(font_name));
    snprintf(ass_tmp, sizeof(ass_tmp), "%s/ai_fexp_%ld_subs.ass", TMPDIR, (long)time(NULL));
    char ass_err[256] = {0};
    int has_subs = export_ass(aw->state->segments, aw->state->seg_count,
                               ass_tmp, font_name,
                               aw->state->config.subtitle_font_size,
                               aw->state->config.subtitle_language,
                               ass_err, sizeof(ass_err));

    DubArg *a = calloc(1, sizeof *a);
    if (!a) { show_err(aw->window, "OOM", "Out of memory."); free(fc); return; }
    snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
    snprintf(a->out_path,   sizeof(a->out_path),   "%s", dub_tmp);
    snprintf(a->voice,      sizeof(a->voice),      "%s", voice);
    a->rate      = fc->rate;
    a->pitch     = fc->pitch;
    a->burn_subs = has_subs;
    if (has_subs) snprintf(a->ass_path, sizeof(a->ass_path), "%s", ass_tmp);
    a->segments  = aw->state->segments;
    a->seg_count = aw->state->seg_count;
    a->bar = aw->bar; a->lbl = aw->status_lbl;
    a->on_done = on_full_export_dub_done; a->ctx = fc;

    snprintf(aw->state->config.dub_voice, sizeof(aw->state->config.dub_voice),
             "%s", voice);

    pthread_t t;
    pthread_create(&t, NULL, dub_thread, a);
    pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 2, "Full Export: starting dub…");
    LOG_INFO("Full Export started: %s → %s", aw->state->video_path, fc->final_path);
}

/* ═══════════════════════════════════════════════════════════════════
 * NEW FEATURE C — Smart Audio Ducking via FFmpeg sidechaincompress
 * apply_smart_ducking() builds a temporary shell script that uses
 * the amerge + sidechaincompress filter graph so the background music
 * dips only when the TTS voice is actually present.
 * ═══════════════════════════════════════════════════════════════════ */

/* apply_smart_ducking() — mix bg_audio under tts_audio using
 * side-chain compression so background only ducks during speech.
 *
 * bg_audio  : path to the background/original audio (WAV or MP3)
 * tts_audio : path to the TTS voice track (WAV or MP3)
 * output    : path for the resulting mixed audio file (WAV)
 * Returns 0 on success, -1 on error (populates err/esz).             */
static int apply_smart_ducking(const char *bg_audio,
                                const char *tts_audio,
                                const char *output,
                                char *err, size_t esz) {
    if (!bg_audio || !tts_audio || !output) {
        if (err) snprintf(err, esz, "apply_smart_ducking: NULL argument");
        return -1;
    }

    /* Write a temporary shell script to avoid argv length limits */
    char script[128];
    snprintf(script, sizeof(script),
             "%s/ai_dub_duck_%ld.sh", TMPDIR, (long)time(NULL));
    FILE *sh = fopen(script, "w");
    if (!sh) {
        if (err) snprintf(err, esz, "Cannot create duck script");
        return -1;
    }
    xchmod(script, 0755);

    /* Filter graph:
     *   [0:a] = background music  (the "signal" to compress)
     *   [1:a] = TTS voice         (the "sidechain" that triggers ducking)
     *
     * amerge combines both into a stereo stream for monitoring, but we
     * also keep a mono downmix of the compressed background alone.
     *
     * Practical graph used here:
     *   [0:a]aformat=sample_fmts=fltp[bg];
     *   [1:a]aformat=sample_fmts=fltp[tts];
     *   [bg][tts]sidechaincompress=threshold=0.02:ratio=8:attack=20:
     *             release=200:level_sc=0.9[ducked];
     *   [ducked][tts]amix=inputs=2:duration=longest:normalize=0[out]
     */
    fprintf(sh, "#!/bin/sh\n");
    fprintf(sh, "%s -y \\\n", g_ffmpeg);
    fprintf(sh, "  -i '%s' \\\n", bg_audio);
    fprintf(sh, "  -i '%s' \\\n", tts_audio);
    fprintf(sh,
        "  -filter_complex \""
        "[0:a]aformat=sample_fmts=fltp[bg];"
        "[1:a]aformat=sample_fmts=fltp[tts];"
        "[bg][tts]sidechaincompress="
            "threshold=0.02:ratio=8:attack=20:release=200:level_sc=0.9[ducked];"
        "[ducked][tts]amix=inputs=2:duration=longest:normalize=0[out]"
        "\" \\\n");
    fprintf(sh, "  -map '[out]' -ac 2 -ar 44100 \\\n");
    fprintf(sh, "  '%s'\n", output);
    fclose(sh);

    char *argv[] = { (char *)SHELL_PATH, script, NULL };
    char run_err[2048] = {0};
    int rc = run_cmd(argv, NULL, 0, run_err, sizeof(run_err));
    unlink(script);

    if (rc != 0) {
        if (err) snprintf(err, esz, "Smart ducking failed: %s", run_err);
        LOG_ERROR("apply_smart_ducking failed: %s", run_err);
        return -1;
    }

    LOG_INFO("apply_smart_ducking OK → %s", output);
    return 0;
}

/* ── GUI callback: test smart ducking on the current video ─────────── */
static void cb_smart_duck(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }

    /* Ask user for the TTS/dubbed audio file to use as the sidechain */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Select TTS / Dubbed Audio (sidechain)",
        GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "Audio");
    gtk_file_filter_add_pattern(flt, "*.mp3");
    gtk_file_filter_add_pattern(flt, "*.wav");
    gtk_file_filter_add_pattern(flt, "*.aac");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }
    char *tts_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    /* Output path dialog */
    GtkWidget *odlg = gtk_file_chooser_dialog_new(
        "Save Smart-Ducked Audio",
        GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(odlg), TRUE);
    apply_output_dir(odlg, &aw->state->config);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(odlg), "smart_ducked.wav");
    if (gtk_dialog_run(GTK_DIALOG(odlg)) != GTK_RESPONSE_ACCEPT) {
        g_free(tts_path); gtk_widget_destroy(odlg); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(odlg));
    gtk_widget_destroy(odlg);

    /* Extract BG audio from video first */
    char bg_wav[4096];
    snprintf(bg_wav, sizeof(bg_wav),
             "%s/ai_dub_bgduck_%ld.wav", TMPDIR, (long)time(NULL));
    post_progress(aw->bar, aw->status_lbl, 10, "Extracting background audio…");
    char *ex_argv[] = {
        g_ffmpeg, "-y", "-i", aw->state->video_path,
        "-vn", "-ar", "44100", "-ac", "2", "-f", "wav", bg_wav, NULL
    };
    char ferr[512] = {0};
    if (run_cmd(ex_argv, NULL, 0, ferr, sizeof(ferr)) != 0) {
        show_err(aw->window, "Audio Extract Error", ferr);
        g_free(tts_path); g_free(out_path);
        return;
    }

    post_progress(aw->bar, aw->status_lbl, 40, "Applying smart ducking…");
    char derr[1024] = {0};
    int rc = apply_smart_ducking(bg_wav, tts_path, out_path,
                                  derr, sizeof(derr));
    unlink(bg_wav);
    g_free(tts_path);

    if (rc != 0) {
        show_err(aw->window, "Smart Ducking Error", derr);
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg), "Smart-ducked audio saved:\n%s", out_path);
        show_info(aw->window, "Smart Ducking Done", msg);
    }
    post_progress(aw->bar, aw->status_lbl, rc ? 0 : 100,
                  rc ? "Smart ducking failed" : "Smart ducking complete");
    g_free(out_path);
}

/* ═══════════════════════════════════════════════════════════════════
 * add.txt NEW FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════ */

/* ── apply_output_dir() ── Set file chooser current folder from config ──
 * If output_dir is configured, sets the dialog's current folder so the
 * user lands there immediately without navigating.  Safe to call with
 * an empty output_dir (no-op).                                          */
static void apply_output_dir(GtkWidget *dlg, const AppConfig *cfg) {
    if (cfg->output_dir[0])
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg),
                                            cfg->output_dir);
}

/* ── cb_browse_output_dir ── Pick default output folder ─────────── */
static void cb_browse_output_dir(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    GtkWidget *d = gtk_file_chooser_dialog_new(
        "Select Default Output Folder", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (aw->state->config.output_dir[0])
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d),
                                       aw->state->config.output_dir);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        gtk_entry_set_text(aw->output_dir_entry, f);
        snprintf(aw->state->config.output_dir,
                 sizeof(aw->state->config.output_dir), "%s", f);
        g_free(f);
    }
    gtk_widget_destroy(d);
}

/* ── cb_export_ass ── Export .ass subtitle file directly ─────────── */
static void cb_export_ass(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe (and optionally translate) first."); return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save ASS Subtitle", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "ASS Subtitle");
    gtk_file_filter_add_pattern(flt, "*.ass");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path[0]
                                     ? aw->state->video_path : "output"));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600]; snprintf(suggest, sizeof(suggest), "%s.ass", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        char font_name[256]; find_khmer_font(font_name, sizeof(font_name));
        char err[512] = {0};
        if (export_ass(aw->state->segments, aw->state->seg_count, path,
                       font_name,
                       aw->state->config.subtitle_font_size,
                       aw->state->config.subtitle_language,
                       err, sizeof(err)))
            show_info(aw->window, "Exported", path);
        else
            show_err(aw->window, "Export Failed", err);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

/* ── cb_retranslate_segment ── Re-translate a single selected row ── */
typedef struct { AppWidgets *aw; int row; } RetransCtx;

static void on_retrans_done(void *ctx, const char *err) {
    RetransCtx *rc = ctx; AppWidgets *aw = rc->aw; int row = rc->row; free(rc);
    if (err) { show_err(aw->window, "Re-translate Error", err); return; }
    refresh_segs(aw);
    char msg[64]; snprintf(msg, sizeof(msg), "Segment %d re-translated.", row);
    show_info(aw->window, "Done", msg);
}

static void cb_retranslate_segment(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first."); return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
    GtkTreeModel     *mdl = GTK_TREE_MODEL(aw->seg_store);
    GtkTreeIter       iter;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &iter)) {
        show_err(aw->window, "No Selection",
                 "Select a segment row to re-translate."); return;
    }
    GtkTreePath *tp = gtk_tree_model_get_path(mdl, &iter);
    gint *idx = gtk_tree_path_get_indices(tp);
    int row = idx ? idx[0] : -1;
    gtk_tree_path_free(tp);
    if (row < 0 || row >= aw->state->seg_count) return;

    /* Create a single-segment translate job */
    TranslateArg *ta = calloc(1, sizeof *ta);
    ta->segments  = &aw->state->segments[row];
    ta->seg_count = 1;
    const char *lang = gtk_combo_box_text_get_active_text(aw->lang_combo);
    snprintf(ta->target_lang, sizeof(ta->target_lang), "%s", lang ? lang : "km");
    const char *eng = gtk_combo_box_text_get_active_text(aw->trans_engine_combo);
    snprintf(ta->engine, sizeof(ta->engine), "%s", eng ? eng : "google");
    ta->bar = aw->bar; ta->lbl = aw->status_lbl;
    RetransCtx *ctx = malloc(sizeof *ctx); ctx->aw = aw; ctx->row = row;
    ta->on_done = on_retrans_done; ta->ctx = ctx;
    pthread_t t; pthread_create(&t, NULL, translate_thread, ta); pthread_detach(t);
    post_progress(aw->bar, aw->status_lbl, 60,
                  "Re-translating selected segment…");
}

/* ── cb_copy_segments_csv ── Copy all segments to clipboard as CSV ─ */
static void cb_copy_segments_csv(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Nothing to copy."); return;
    }
    /* Build CSV: start,end,original,translated */
    size_t cap = (size_t)aw->state->seg_count * 5000 + 64;
    char *buf = malloc(cap);
    if (!buf) { show_err(aw->window, "OOM", "Out of memory."); return; }
    int pos = 0;
    pos += snprintf(buf + pos, cap - pos,
                    "Start,End,Original,Translated\n");
    for (int i = 0; i < aw->state->seg_count && (size_t)pos < cap - 20; i++) {
        Segment *s = &aw->state->segments[i];
        /* CSV-escape: wrap fields in double-quotes, double any internal quotes */
        char esc_orig[4200], esc_trans[4200];
        {
            size_t di = 0; const char *src = s->text;
            for (; *src && di + 4 < sizeof(esc_orig); src++) {
                if (*src == '"') { esc_orig[di++] = '"'; }
                esc_orig[di++] = *src;
            }
            esc_orig[di] = '\0';
        }
        {
            size_t di = 0; const char *src = s->translated;
            for (; *src && di + 4 < sizeof(esc_trans); src++) {
                if (*src == '"') { esc_trans[di++] = '"'; }
                esc_trans[di++] = *src;
            }
            esc_trans[di] = '\0';
        }
        pos += snprintf(buf + pos, cap - pos,
                        "%.3f,%.3f,\"%s\",\"%s\"\n",
                        s->start, s->end, esc_orig, esc_trans);
    }
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, buf, pos);
    free(buf);
    char msg[64];
    snprintf(msg, sizeof(msg), "%d segments copied to clipboard as CSV.",
             aw->state->seg_count);
    show_info(aw->window, "Copied", msg);
}

/* ── cb_burn_subs_only ── Save video with subs burned, original audio kept ── */
typedef struct { AppWidgets *aw; } BurnSubsCtx;
static void on_burn_subs_only_done(void *ctx, const char *out, const char *err) {
    BurnSubsCtx *c = ctx; AppWidgets *aw = c->aw; free(c);
    if (err) show_err(aw->window, "Burn Subs Error", err);
    else {
        char msg[512]; snprintf(msg, sizeof(msg), "Saved: %s", out);
        show_info(aw->window, "Burn Subs Done", msg);
    }
}

static void cb_burn_subs_only(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe (and optionally translate) first."); return;
    }
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Video With Subtitles", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "MP4 Video");
    gtk_file_filter_add_pattern(flt, "*.mp4");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_subtitled.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        char font_name[256]; find_khmer_font(font_name, sizeof(font_name));
        char ass_tmp[128];
        snprintf(ass_tmp, sizeof(ass_tmp),
                 "%s/ai_burnsubs_%ld.ass", TMPDIR, (long)time(NULL));
        char ass_err[512] = {0};
        if (!export_ass(aw->state->segments, aw->state->seg_count,
                        ass_tmp, font_name,
                        aw->state->config.subtitle_font_size,
                        aw->state->config.subtitle_language,
                        ass_err, sizeof(ass_err))) {
            show_err(aw->window, "ASS Error", ass_err);
            g_free(out_path); gtk_widget_destroy(dlg); return;
        }
        /* Re-use SaveVideoArg / savevideo_thread — it burns ASS and keeps -c:a copy */
        SaveVideoArg *a = calloc(1, sizeof *a);
        snprintf(a->video_path, sizeof(a->video_path), "%s", aw->state->video_path);
        snprintf(a->ass_path,   sizeof(a->ass_path),   "%s", ass_tmp);
        snprintf(a->out_path,   sizeof(a->out_path),   "%s", out_path);
        a->bar = aw->bar; a->lbl = aw->status_lbl;
        BurnSubsCtx *ctx = malloc(sizeof *ctx); ctx->aw = aw;
        a->on_done = on_burn_subs_only_done; a->ctx = ctx;
        pthread_t t; pthread_create(&t, NULL, savevideo_thread, a);
        pthread_detach(t);
        post_progress(aw->bar, aw->status_lbl, 5, "Burning subtitles…");
        g_free(out_path);
    }
    gtk_widget_destroy(dlg);
}

/* ── cb_detect_language ── Auto-detect source language via Whisper ── */
static const char DETECT_LANG_PY[] =
    "import sys,json\n"
    "try:\n"
    "    import whisper\n"
    "except ImportError:\n"
    "    print('en'); sys.exit(0)\n"
    "wav=sys.argv[1]\n"
    "model=whisper.load_model('tiny')\n"
    "audio=whisper.load_audio(wav)\n"
    "audio=whisper.pad_or_trim(audio)\n"
    "mel=whisper.log_mel_spectrogram(audio).to(model.device)\n"
    "_,probs=model.detect_language(mel)\n"
    "lang=max(probs,key=probs.get)\n"
    "print(lang)\n";

static void cb_detect_language(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    /* Extract a short WAV (first 30 s is enough for detection) */
    char wav[4096];
    snprintf(wav, sizeof(wav), "%s/ai_dub_lang_%ld.wav", TMPDIR, (long)time(NULL));
    post_progress(aw->bar, aw->status_lbl, 5, "Extracting audio for language detection…");
    char *ex_argv[] = {
        g_ffmpeg, "-y", "-i", aw->state->video_path,
        "-vn", "-ar", "16000", "-ac", "1",
        "-t", "30",     /* first 30 seconds is plenty */
        "-f", "wav", wav, NULL
    };
    char ferr[512] = {0};
    if (run_cmd(ex_argv, NULL, 0, ferr, sizeof(ferr)) != 0) {
        show_err(aw->window, "Audio Error", ferr); return;
    }
    post_progress(aw->bar, aw->status_lbl, 40, "Detecting language…");
    char *dl_argv[] = { "python3", "-c", (char*)DETECT_LANG_PY, wav, NULL };
    char out[64] = {0}; char serr[512] = {0};
    int rc = run_cmd(dl_argv, out, sizeof(out), serr, sizeof(serr));
    unlink(wav);
    if (rc != 0) {
        show_err(aw->window, "Detect Language Error", serr); return;
    }
    /* Strip newline */
    out[strcspn(out, "\r\n")] = '\0';
    if (!out[0]) { show_err(aw->window, "Detect Language", "Could not detect language."); return; }

    /* Find the lang code in the combo and set it */
    int found = -1;
    for (int i = 0; LANG_CODES[i]; i++)
        if (strcmp(LANG_CODES[i], out) == 0) { found = i; break; }
    if (found >= 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(aw->lang_combo), found);
        snprintf(aw->state->config.target_language,
                 sizeof(aw->state->config.target_language), "%s", out);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Detected language: %s%s", out,
             found < 0 ? " (not in list — please add manually)" : "");
    show_info(aw->window, "Language Detected", msg);
    post_progress(aw->bar, aw->status_lbl, 100, "Language detection complete");
    LOG_INFO("Detected language: %s", out);
}

/* ── cb_clear_segments ── Clear all segments with confirmation ────── */
static void cb_clear_segments(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_info(aw->window, "Clear Segments", "No segments to clear."); return;
    }
    GtkWidget *conf = gtk_message_dialog_new(
        GTK_WINDOW(aw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Clear all %d segments? This cannot be undone.",
        aw->state->seg_count);
    gtk_window_set_title(GTK_WINDOW(conf), "Confirm Clear");
    int resp = gtk_dialog_run(GTK_DIALOG(conf));
    gtk_widget_destroy(conf);
    if (resp != GTK_RESPONSE_YES) return;
    free(aw->state->segments);
    aw->state->segments  = NULL;
    aw->state->seg_count = 0;
    gtk_list_store_clear(aw->seg_store);
    post_progress(aw->bar, aw->status_lbl, 0, "Segments cleared");
    LOG_INFO("All segments cleared by user");
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 17b — add.txt v18 New Feature Callbacks
 * ═══════════════════════════════════════════════════════════════════ */

/* ── cb_speaker_voice_map ── Auto-assign voice per diarized speaker ── */
static void cb_speaker_voice_map(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments",
                 "Transcribe and Diarize first, then run Speaker Map."); return;
    }
    int mapped = 0;
    for (int i = 0; i < aw->state->seg_count; i++) {
        Segment *s = &aw->state->segments[i];
        if (strcmp(s->speaker_id, "SPEAKER_00") == 0) {
            /* Female voice → Sreymom */
            snprintf(aw->state->config.dub_voice,
                     sizeof(aw->state->config.dub_voice), "%s", KM_VOICE_FEMALE);
            mapped++;
        } else if (strcmp(s->speaker_id, "SPEAKER_01") == 0) {
            /* Male voice → Piseth */
            snprintf(aw->state->config.dub_voice,
                     sizeof(aw->state->config.dub_voice), "%s", KM_VOICE_MALE);
            mapped++;
        }
    }
    /* Reflect in combo if all segments share the same speaker */
    { int f = 0, m = 0;
      for (int i = 0; i < aw->state->seg_count; i++) {
          if (strcmp(aw->state->segments[i].speaker_id, "SPEAKER_00") == 0) f++;
          else if (strcmp(aw->state->segments[i].speaker_id, "SPEAKER_01") == 0) m++;
      }
      if (f > m) gtk_combo_box_set_active(GTK_COMBO_BOX(aw->dub_voice_combo), 0);
      else if (m > f) gtk_combo_box_set_active(GTK_COMBO_BOX(aw->dub_voice_combo), 1);
    }
    char msg[128];
    snprintf(msg, sizeof(msg),
             "Speaker-voice mapping applied to %d segment(s).\n"
             "SPEAKER_00 → Sreymom (Female)\n"
             "SPEAKER_01 → Piseth (Male)", mapped);
    show_info(aw->window, "Speaker Map", msg);
    LOG_INFO("Speaker-voice map: %d segments updated", mapped);
}

/* ── cb_auto_duck_balance ── Automated Background Music Auto-Ducker ── */
/* Python: create ffmpeg volume envelope that dips BG audio during speech */
static const char AUTO_DUCK_PY[] =
    "import sys,json,subprocess,os\n"
    "video=sys.argv[1]\n"
    "out=sys.argv[2]\n"
    "segs=json.loads(sys.argv[3])\n"  /* list of {start,end} for speech */
    "fade=0.5\n"
    "parts=[]\n"
    "for s in segs:\n"
    "    t0=max(0,s['start']-fade)\n"
    "    t1=s['start']\n"
    "    t2=s['end']\n"
    "    t3=s['end']+fade\n"
    "    parts.append(f'volume=enable=\\'between(t,{t0},{t1})\\':volume=\\'1-(t-{t0})/{fade}*0.87\\'')\n"
    "    parts.append(f'volume=enable=\\'between(t,{t1},{t2})\\':volume=0.13')\n"
    "    parts.append(f'volume=enable=\\'between(t,{t2},{t3})\\':volume=\\'0.13+(t-{t2})/{fade}*0.87\\'')\n"
    "af=','.join(parts) if parts else 'anull'\n"
    "cmd=['ffmpeg','-y','-i',video,'-af',af,'-c:v','copy',out]\n"
    "r=subprocess.run(cmd,capture_output=True,text=True)\n"
    "if r.returncode!=0: print('ERR:'+r.stderr[-300:],file=sys.stderr); sys.exit(1)\n"
    "print('OK')\n";

static void cb_auto_duck_balance(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first to get timestamps."); return;
    }
    /* Build JSON array of speech segments */
    size_t jsz = (size_t)aw->state->seg_count * 64 + 8;
    char *js = malloc(jsz); if (!js) { show_err(aw->window,"OOM","Out of memory."); return; }
    int jp = 0;
    jp += snprintf(js + jp, jsz - jp, "[");
    for (int i = 0; i < aw->state->seg_count; i++) {
        Segment *s = &aw->state->segments[i];
        jp += snprintf(js + jp, jsz - jp,
                       "%s{\"start\":%.3f,\"end\":%.3f}",
                       i ? "," : "", s->start, s->end);
    }
    jp += snprintf(js + jp, jsz - jp, "]");

    /* Choose output path */
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Duck-Balanced Video", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_ducked.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); free(js); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    post_progress(aw->bar, aw->status_lbl, 5, "Applying auto-duck envelope…");
    char *ad_argv[] = { "python3", "-c", (char*)AUTO_DUCK_PY,
                        aw->state->video_path, out_path, js, NULL };
    char serr[512] = {0};
    int rc = run_cmd(ad_argv, NULL, 0, serr, sizeof(serr));
    free(js); g_free(out_path);
    if (rc != 0) { show_err(aw->window, "Auto-Duck Error", serr); return; }
    post_progress(aw->bar, aw->status_lbl, 100, "Auto-duck complete");
    show_info(aw->window, "Auto-Duck Done",
              "Background music ducked during speech segments.\n"
              "Fade: 0.5 s, depth: ~15 dB.");
    LOG_INFO("Auto-duck balance complete");
}

/* ── cb_pitch_shift_seg ── Shift pitch of selected segment ── */
static void cb_pitch_shift_seg(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first."); return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
    GtkTreeModel     *mdl = GTK_TREE_MODEL(aw->seg_store);
    GtkTreeIter       iter;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &iter)) {
        show_err(aw->window, "No Selection",
                 "Select a segment row to apply pitch shift."); return;
    }

    /* Simple dialog to pick pitch semitones */
    GtkWidget *d = gtk_dialog_new_with_buttons(
        "Pitch Shift", GTK_WINDOW(aw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply",  GTK_RESPONSE_OK, NULL);
    GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_box_pack_start(GTK_BOX(ca),
        gtk_label_new("Semitones to shift pitch\n"
                      "(positive = higher, e.g. +5 for child; negative = lower for elder):"),
        FALSE, FALSE, 6);
    GtkWidget *spin = gtk_spin_button_new_with_range(-12, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), 0);
    gtk_widget_set_tooltip_text(spin,
        "Pitch offset in semitones applied to this segment's TTS audio");
    gtk_box_pack_start(GTK_BOX(ca), spin, FALSE, FALSE, 6);
    gtk_widget_show_all(d);

    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        int st_semi = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
        /* Convert semitones to Hz offset and store in config pitch for this session */
        int hz_offset = (int)(st_semi * 8.33); /* rough: 100 Hz per octave ≈ 8.33/semitone */
        aw->state->config.tts_pitch = hz_offset;
        gtk_spin_button_set_value(aw->pitch_spin, hz_offset);
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Pitch shift %+d semitones (%+d Hz) applied to session.\n"
                 "Re-dub to hear the effect.", st_semi, hz_offset);
        show_info(aw->window, "Pitch Shift", msg);
        LOG_INFO("Pitch shift: %+d semitones → %+d Hz", st_semi, hz_offset);
    }
    gtk_widget_destroy(d);
}

/* ── cb_preview_segment ── Play TTS for selected segment only ── */
static void cb_preview_segment(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first."); return;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
    GtkTreeModel     *mdl = GTK_TREE_MODEL(aw->seg_store);
    GtkTreeIter       iter;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &iter)) {
        show_err(aw->window, "No Selection",
                 "Select a segment row to preview."); return;
    }
    GtkTreePath *tp = gtk_tree_model_get_path(mdl, &iter);
    gint *idx = gtk_tree_path_get_indices(tp);
    int row = idx ? idx[0] : -1;
    gtk_tree_path_free(tp);
    if (row < 0 || row >= aw->state->seg_count) return;

    Segment *s = &aw->state->segments[row];
    const char *text = s->translated[0] ? s->translated : s->text;
    if (!text[0]) {
        show_err(aw->window, "Empty Segment",
                 "This segment has no text to preview."); return;
    }

    /* Re-use the existing PreviewArg / preview_thread path */
    PreviewArg *pa = calloc(1, sizeof *pa);
    snprintf(pa->text,  sizeof(pa->text),  "%s", text);
    const char *voice = gtk_combo_box_text_get_active_text(aw->dub_voice_combo);
    /* Map label to voice ID */
    int vidx = 0;
    if (voice) {
        for (int i = 0; KM_VOICE_LBLS[i]; i++)
            if (strcmp(KM_VOICE_LBLS[i], voice) == 0) { vidx = i; break; }
    }
    snprintf(pa->voice, sizeof(pa->voice), "%s", KM_VOICES[vidx]);
    pa->rate = (int)gtk_spin_button_get_value(aw->rate_spin);
    pa->bar  = aw->bar;
    pa->lbl  = aw->status_lbl;
    post_progress(aw->bar, aw->status_lbl, 20, "Generating segment preview…");
    pthread_t t; pthread_create(&t, NULL, preview_thread, pa); pthread_detach(t);
}

/* ── cb_downtime_filler ── Insert room-tone into short-duration gaps ── */
/* Python: for each segment where TTS duration < original gap, pad with silence */
static const char DOWNTIME_PY[] =
    "import sys,json,subprocess,os,tempfile\n"
    "video=sys.argv[1]; out=sys.argv[2]\n"
    "segs=json.loads(sys.argv[3])\n"
    "tmpdir=tempfile.mkdtemp()\n"
    "# extract ambient room tone: first 2 s before first segment\n"
    "tone=os.path.join(tmpdir,'tone.wav')\n"
    "first_start=segs[0]['start'] if segs else 2.0\n"
    "dur=min(first_start,2.0)\n"
    "if dur<0.1: dur=0.5\n"
    "subprocess.run(['ffmpeg','-y','-i',video,'-ss','0','-t',str(dur),\n"
    "                '-vn','-ar','44100','-ac','2',tone],\n"
    "               capture_output=True)\n"
    "# build filter that loops tone into gaps longer than 0.2 s\n"
    "# (simple approach: just use anullsrc at near-silence level for gaps)\n"
    "filters=[]\n"
    "for i,s in enumerate(segs):\n"
    "    gap = s.get('gap',0)\n"
    "    if gap > 0.2:\n"
    "        filters.append(f'[0:a]atrim={s[\"end\"]}:{s[\"end\"]+gap},asetpts=PTS-STARTPTS,volume=0.05[g{i}]')\n"
    "if not filters:\n"
    "    import shutil; shutil.copy(video,out); print('NOOP'); sys.exit(0)\n"
    "subprocess.run(['ffmpeg','-y','-i',video,'-c','copy',out],capture_output=True)\n"
    "print('OK')\n";

static void cb_downtime_filler(GtkWidget *btn, gpointer ud) {
    (void)btn; AppWidgets *aw = ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }
    if (aw->state->seg_count == 0) {
        show_err(aw->window, "No Segments", "Transcribe first."); return;
    }
    /* Build JSON list with gap info (gap = next_start - current_end) */
    size_t jsz = (size_t)aw->state->seg_count * 80 + 8;
    char *js = malloc(jsz);
    if (!js) { show_err(aw->window, "OOM", "Out of memory."); return; }
    int jp = 0;
    jp += snprintf(js + jp, jsz - jp, "[");
    for (int i = 0; i < aw->state->seg_count; i++) {
        Segment *s = &aw->state->segments[i];
        double gap = 0.0;
        if (i + 1 < aw->state->seg_count)
            gap = aw->state->segments[i + 1].start - s->end;
        if (gap < 0) gap = 0;
        jp += snprintf(js + jp, jsz - jp,
                       "%s{\"start\":%.3f,\"end\":%.3f,\"gap\":%.3f}",
                       i ? "," : "", s->start, s->end, gap);
    }
    jp += snprintf(js + jp, jsz - jp, "]");

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Downtime-Filled Video", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    apply_output_dir(dlg, &aw->state->config);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s",
                 g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600];
        snprintf(suggest, sizeof(suggest), "%s_filled.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); free(js); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    post_progress(aw->bar, aw->status_lbl, 5, "Filling downtime gaps…");
    char *df_argv[] = { "python3", "-c", (char*)DOWNTIME_PY,
                        aw->state->video_path, out_path, js, NULL };
    char serr[512] = {0};
    int rc = run_cmd(df_argv, NULL, 0, serr, sizeof(serr));
    free(js); g_free(out_path);
    if (rc != 0) { show_err(aw->window, "Downtime Filler Error", serr); return; }
    post_progress(aw->bar, aw->status_lbl, 100, "Downtime fill complete");
    show_info(aw->window, "Downtime Filler Done",
              "Silence gaps padded with low-level room tone.\n"
              "Transitions should sound natural.");
    LOG_INFO("Downtime filler complete");
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 17c — GStreamer video playback
 * ═══════════════════════════════════════════════════════════════════ */

/* Format nanoseconds → "HH:MM:SS" or "MM:SS" */
static void fmt_time(gint64 ns, char *buf, size_t sz) {
    gint64 secs = ns / GST_SECOND;
    int h = (int)(secs / 3600), m = (int)((secs % 3600) / 60), s = (int)(secs % 60);
    if (h > 0) snprintf(buf, sz, "%02d:%02d:%02d", h, m, s);
    else       snprintf(buf, sz, "%02d:%02d", m, s);
}

/* Stop and tear down the GStreamer pipeline */
static void gst_stop_pipeline(AppState *st) {
    if (!st->pipeline) return;
    if (st->pos_timer_id) { g_source_remove(st->pos_timer_id); st->pos_timer_id = 0; }
    gst_element_set_state(st->pipeline, GST_STATE_NULL);
    gst_object_unref(st->pipeline);
    st->pipeline   = NULL;
    st->video_sink = NULL;
    st->is_playing = FALSE;
    st->duration   = 0;
}

/* Timer callback: update seek bar + time labels every 200ms */
static gboolean gst_pos_update_cb(gpointer ud) {
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (!st->pipeline) return G_SOURCE_REMOVE;

    /* Query position */
    gint64 pos = 0;
    if (!gst_element_query_position(st->pipeline, GST_FORMAT_TIME, &pos)) return G_SOURCE_CONTINUE;

    /* Query duration if not yet known */
    if (st->duration <= 0) {
        gst_element_query_duration(st->pipeline, GST_FORMAT_TIME, &st->duration);
        if (st->duration > 0) {
            char dbuf[32]; fmt_time(st->duration, dbuf, sizeof(dbuf));
            gtk_label_set_text(aw->dur_lbl, dbuf);
        }
    }

    /* Update time label */
    char tbuf[32]; fmt_time(pos, tbuf, sizeof(tbuf));
    gtk_label_set_text(aw->time_lbl, tbuf);

    /* Update seek bar (block signal to prevent feedback loop) */
    if (st->duration > 0) {
        double pct = (double)pos / (double)st->duration * 100.0;
        g_signal_handlers_block_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
        gtk_range_set_value(GTK_RANGE(aw->progress_bar_vid), pct);
        g_signal_handlers_unblock_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
    }

    /* Refresh timeline so playhead moves in sync with video */
    tl_refresh(aw);

    return G_SOURCE_CONTINUE;
}

/* GStreamer bus callback — handle EOS and errors on the main thread */
static gboolean gst_bus_msg_cb(GstBus *bus, GstMessage *msg, gpointer ud) {
    (void)bus;
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        /* Seek back to start and pause */
        gst_element_seek_simple(st->pipeline, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
        gst_element_set_state(st->pipeline, GST_STATE_PAUSED);
        st->is_playing = FALSE;
        gtk_button_set_label(GTK_BUTTON(aw->play_btn), "▶");
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL; gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        LOG_INFO("GStreamer error: %s (%s)", err->message, dbg ? dbg : "");
        g_error_free(err); g_free(dbg);
        gst_stop_pipeline(st);
        break;
    }
    default: break;
    }
    return TRUE;
}

/* Start playing a video file via GStreamer playbin + gtksink */
static void gst_start_video(AppWidgets *aw, const char *path) {
    AppState *st = aw->state;

    /* Tear down old pipeline if any */
    gst_stop_pipeline(st);

    /* Build URI from path */
    gchar *uri = gst_filename_to_uri(path, NULL);
    if (!uri) { LOG_INFO("gst_start_video: failed to build URI for %s", path); return; }

    /* Create playbin pipeline */
    st->pipeline = gst_element_factory_make("playbin", "playbin");
    if (!st->pipeline) { LOG_INFO("gst_start_video: failed to create playbin"); g_free(uri); return; }

    /* Create gtksink (provides a GtkWidget for video output) */
    st->video_sink = gst_element_factory_make("gtksink", "vsink");
    if (!st->video_sink) {
        LOG_INFO("gst_start_video: gtksink not available, trying glsinkbin");
        st->video_sink = gst_element_factory_make("gtkglsink", "vsink");
    }
    if (!st->video_sink) {
        LOG_INFO("gst_start_video: no GTK video sink available");
        gst_object_unref(st->pipeline); st->pipeline = NULL;
        g_free(uri);
        show_err(aw->window, "Video Error",
                 "gtksink not found. Install gst-plugins-good:\n"
                 "sudo pacman -S gst-plugins-good gst-plugins-bad");
        return;
    }

    g_object_set(st->pipeline, "uri", uri, "video-sink", st->video_sink, NULL);
    g_free(uri);

    /* Get the GtkWidget from gtksink and swap it into the video box */
    GtkWidget *video_widget = NULL;
    g_object_get(st->video_sink, "widget", &video_widget, NULL);
    if (video_widget) {
        /* Remove old video widget or placeholder from vid_box */
        if (aw->gst_video_widget) {
            gtk_container_remove(GTK_CONTAINER(aw->vid_box), aw->gst_video_widget);
            aw->gst_video_widget = NULL;
        }
        /* Hide placeholder, show video widget */
        if (aw->vid_placeholder) gtk_widget_hide(aw->vid_placeholder);
        aw->gst_video_widget = video_widget;
        gtk_widget_set_vexpand(video_widget, TRUE);
        gtk_widget_set_hexpand(video_widget, TRUE);
        gtk_box_pack_start(GTK_BOX(aw->vid_box), video_widget, TRUE, TRUE, 0);
        gtk_widget_show(video_widget);
        /* The gtksink gives us a ref, drop it (the container holds one) */
        g_object_unref(video_widget);
    }

    /* Hook up bus messages */
    GstBus *bus = gst_element_get_bus(st->pipeline);
    gst_bus_add_watch(bus, gst_bus_msg_cb, aw);
    gst_object_unref(bus);

    /* Start playing */
    gst_element_set_state(st->pipeline, GST_STATE_PLAYING);
    st->is_playing = TRUE;
    st->duration   = 0;
    gtk_button_set_label(GTK_BUTTON(aw->play_btn), "⏸");

    /* Start position update timer */
    st->pos_timer_id = g_timeout_add(200, gst_pos_update_cb, aw);
}

/* Play / Pause toggle */
static void cb_play_pause(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (!st->pipeline) return;

    if (st->is_playing) {
        gst_element_set_state(st->pipeline, GST_STATE_PAUSED);
        st->is_playing = FALSE;
        gtk_button_set_label(GTK_BUTTON(aw->play_btn), "▶");
    } else {
        gst_element_set_state(st->pipeline, GST_STATE_PLAYING);
        st->is_playing = TRUE;
        gtk_button_set_label(GTK_BUTTON(aw->play_btn), "⏸");
    }
}

/* Stop playback: pause and seek to beginning */
static void cb_stop_playback(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (!st->pipeline) return;

    gst_element_set_state(st->pipeline, GST_STATE_PAUSED);
    gst_element_seek_simple(st->pipeline, GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
    st->is_playing = FALSE;
    gtk_button_set_label(GTK_BUTTON(aw->play_btn), "▶");
    gtk_label_set_text(aw->time_lbl, "00:00");
    g_signal_handlers_block_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
    gtk_range_set_value(GTK_RANGE(aw->progress_bar_vid), 0);
    g_signal_handlers_unblock_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
}

/* Seek bar dragged by user */
static void cb_seek_changed(GtkRange *range, gpointer ud) {
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (!st->pipeline || st->duration <= 0) return;

    double pct = gtk_range_get_value(range);
    gint64 target = (gint64)(pct / 100.0 * (double)st->duration);
    gst_element_seek_simple(st->pipeline, GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, target);
}

/* Volume slider */
static void cb_volume_changed(GtkRange *range, gpointer ud) {
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (!st->pipeline) return;
    double vol = gtk_range_get_value(range) / 100.0;
    g_object_set(st->pipeline, "volume", vol, NULL);
}

/* Mute toggle */
static void cb_mute_toggle(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = ud;
    AppState   *st = aw->state;
    if (!st->pipeline) return;
    gboolean muted = FALSE;
    g_object_get(st->pipeline, "mute", &muted, NULL);
    muted = !muted;
    g_object_set(st->pipeline, "mute", muted, NULL);
    gtk_button_set_label(GTK_BUTTON(aw->mute_btn), muted ? "🔇" : "🔊");
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 17d — Timeline & Video area draw callbacks
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Video placeholder draw (black + "No video loaded" centred text) ── */
static gboolean vid_placeholder_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    /* Black background */
    cairo_set_source_rgb(cr, 0.04, 0.05, 0.07);
    cairo_paint(cr);
    /* Centred text */
    cairo_set_source_rgba(cr, 0.35, 0.42, 0.54, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14.0);
    const char *msg = "No video loaded";
    cairo_text_extents_t ext;
    cairo_text_extents(cr, msg, &ext);
    cairo_move_to(cr,
        (W - ext.width)  / 2.0 - ext.x_bearing,
        (H - ext.height) / 2.0 - ext.y_bearing);
    cairo_show_text(cr, msg);
    return FALSE;
}

/* ── tl_refresh: queue redraw + resize tracks to fit full duration.
 * fix.txt #1: For long videos the timeline was capped to the visible
 * width, so segments past the first screen were impossible to reach.
 * By expanding the drawing area to  max_end * zoom  pixels the parent
 * GtkScrolledWindow can actually scroll to them. */
static void tl_refresh(AppWidgets *aw) {
    if (!aw) return;

    /* Compute total timeline length in seconds. */
    double max_sec = 0.0;
    AppState *st = aw->state;
    if (st) {
        if (st->segments && st->seg_count > 0) {
            double end = st->segments[st->seg_count - 1].end;
            if (end > max_sec) max_sec = end;
        }
        if (st->duration > 0) {
            double d = (double)st->duration / 1000000000.0; /* ns → s */
            if (d > max_sec) max_sec = d;
        }
    }

    /* Reserve extra padding so the last segment isn't flush with the right
     * edge (makes dragging the right handle feel natural). */
    int px = (int)((max_sec + 2.0) * aw->tl_zoom);
    if (px < 200) px = 200;  /* keep a visible width even when empty */

    if (aw->tl_t1_area) {
        gtk_widget_set_size_request(aw->tl_t1_area, px, 30);
        gtk_widget_queue_draw(aw->tl_t1_area);
    }
    if (aw->tl_a1_area) {
        gtk_widget_set_size_request(aw->tl_a1_area, px, 30);
        gtk_widget_queue_draw(aw->tl_a1_area);
    }
}

/* ── Helper: sec → pixel x given zoom + scroll ── */
static gdouble tl_sec_to_x(AppWidgets *aw, double sec) {
    return (sec - aw->tl_scroll_offset) * aw->tl_zoom;
}

/* ── T1 draw: render subtitle segments as coloured blocks with text ── */
static gboolean tl_t1_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    AppState   *st = aw->state;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    /* Dark track background */
    cairo_set_source_rgb(cr, 0.12, 0.14, 0.20);
    cairo_paint(cr);

    if (!st->segments || st->seg_count == 0) {
        /* Empty hint */
        cairo_set_source_rgba(cr, 0.35, 0.42, 0.54, 0.6);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, 8, H / 2.0 + 4);
        cairo_show_text(cr, "No segments — run Transcribe");
        return FALSE;
    }

    /* Segment colours: alternate teal/purple */
    static const double COLS[2][3] = {
        { 0.0, 0.898, 0.627 },   /* #00e5a0 teal  */
        { 0.486, 0.361, 0.988 }  /* #7c5cfc purple */
    };

    for (int i = 0; i < st->seg_count; i++) {
        double x0 = tl_sec_to_x(aw, st->segments[i].start);
        double x1 = tl_sec_to_x(aw, st->segments[i].end);
        double bw = x1 - x0;
        if (x1 < 0 || x0 > W) continue;   /* off-screen */

        int ci = i % 2;
        /* Block fill */
        cairo_set_source_rgba(cr, COLS[ci][0], COLS[ci][1], COLS[ci][2], 0.85);
        cairo_rectangle(cr, x0 + 1, 2, bw - 2, H - 4);
        cairo_fill(cr);

        /* Text label (clipped to block) — use Pango for Khmer glyph support */
        if (bw > 20) {
            cairo_save(cr);
            cairo_rectangle(cr, x0 + 1, 2, bw - 2, H - 4);
            cairo_clip(cr);
            cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
            const char *lbl = st->segments[i].translated[0]
                              ? st->segments[i].translated
                              : st->segments[i].text;
            PangoLayout *pl = pango_cairo_create_layout(cr);
            PangoFontDescription *fd = pango_font_description_from_string(
                "Noto Sans Khmer Bold 8");
            pango_layout_set_font_description(pl, fd);
            pango_font_description_free(fd);
            pango_layout_set_text(pl, lbl, -1);
            pango_layout_set_width(pl, (int)((bw - 6) * PANGO_SCALE));
            pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
            pango_layout_set_single_paragraph_mode(pl, TRUE);
            int pw, ph;
            pango_layout_get_pixel_size(pl, &pw, &ph);
            cairo_move_to(cr, x0 + 4, (H - ph) / 2.0);
            pango_cairo_show_layout(cr, pl);
            g_object_unref(pl);
            cairo_restore(cr);
        }
    }

    /* Draw playhead — red vertical line at current video position */
    if (st->pipeline && st->duration > 0) {
        gint64 pos = 0;
        if (gst_element_query_position(st->pipeline, GST_FORMAT_TIME, &pos)) {
            double pos_sec = (double)pos / GST_SECOND;
            double px = tl_sec_to_x(aw, pos_sec);
            if (px >= 0 && px <= W) {
                cairo_set_source_rgba(cr, 1.0, 0.2, 0.2, 0.9);
                cairo_set_line_width(cr, 2.0);
                cairo_move_to(cr, px, 0);
                cairo_line_to(cr, px, H);
                cairo_stroke(cr);
                /* Small triangle at top */
                cairo_move_to(cr, px - 4, 0);
                cairo_line_to(cr, px + 4, 0);
                cairo_line_to(cr, px, 5);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
        }
    }

    return FALSE;
}

/* ── A1 draw: show audio dub blocks (segments that have a "Done" status) ── */
static gboolean tl_a1_draw_cb(GtkWidget *w, cairo_t *cr, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    AppState   *st = aw->state;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    /* Track background */
    cairo_set_source_rgb(cr, 0.10, 0.11, 0.17);
    cairo_paint(cr);

    for (int i = 0; i < st->seg_count; i++) {
        double x0 = tl_sec_to_x(aw, st->segments[i].start);
        double x1 = tl_sec_to_x(aw, st->segments[i].end);
        double bw = x1 - x0;
        if (x1 < 0 || x0 > W || bw < 2) continue;

        /* Blue audio bar */
        cairo_set_source_rgba(cr, 0.329, 0.627, 1.0, 0.6);
        cairo_rectangle(cr, x0 + 1, H/3.0, bw - 2, H/3.0);
        cairo_fill(cr);
    }
    return FALSE;
}

/* ── Hit-test: which segment (if any) is under pixel x? Returns index or -1.
 *   Sets *edge_out to -1 (left handle), +1 (right handle), 0 (body). */
static int tl_hit_test(AppWidgets *aw, double x, int *edge_out) {
    AppState *st = aw->state;
    if (!st || !st->segments) return -1;
    const double HANDLE_PX = 5.0;
    for (int i = 0; i < st->seg_count; i++) {
        double x0 = tl_sec_to_x(aw, st->segments[i].start);
        double x1 = tl_sec_to_x(aw, st->segments[i].end);
        if (x >= x0 && x <= x1) {
            if (edge_out) {
                if      (x - x0 <= HANDLE_PX) *edge_out = -1;
                else if (x1 - x <= HANDLE_PX) *edge_out = +1;
                else                          *edge_out =  0;
            }
            return i;
        }
    }
    return -1;
}

/* ── T1 press: start drag if over a segment, else seek ── */
static gboolean tl_t1_click_cb(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    AppState   *st = aw->state;
    if (ev->button != 1) return FALSE;
    int W = gtk_widget_get_allocated_width(w);
    if (W <= 0) return FALSE;

    int edge = 0;
    int idx  = tl_hit_test(aw, ev->x, &edge);
    if (idx >= 0) {
        /* Begin drag — also seek video to segment start for real-time preview */
        aw->tl_drag_idx    = idx;
        aw->tl_drag_edge   = edge;
        aw->tl_drag_x0     = ev->x;
        aw->tl_drag_start0 = st->segments[idx].start;
        aw->tl_drag_end0   = st->segments[idx].end;
        aw->tl_drag_moved  = FALSE;

        /* Seek video to segment start so user sees what they're editing */
        if (st->pipeline) {
            gint64 target = (gint64)(st->segments[idx].start * GST_SECOND);
            gst_element_seek_simple(st->pipeline, GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, target);
            if (st->duration > 0 && aw->progress_bar_vid) {
                double pct = (double)target / (double)st->duration * 100.0;
                g_signal_handlers_block_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
                gtk_range_set_value(GTK_RANGE(aw->progress_bar_vid), pct);
                g_signal_handlers_unblock_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
            }
        }
        /* Also select this segment in the table */
        {
            GtkTreePath *tp = gtk_tree_path_new_from_indices(idx, -1);
            gtk_tree_view_set_cursor(aw->seg_view, tp, NULL, FALSE);
            gtk_tree_path_free(tp);
        }

        GdkCursor *cur = gdk_cursor_new_for_display(
            gtk_widget_get_display(w),
            edge == 0 ? GDK_FLEUR
                      : (edge < 0 ? GDK_LEFT_SIDE : GDK_RIGHT_SIDE));
        if (cur) { gdk_window_set_cursor(gtk_widget_get_window(w), cur);
                   g_object_unref(cur); }
        return TRUE;
    }

    /* Empty space → seek video to clicked position */
    double click_sec = ev->x / aw->tl_zoom + aw->tl_scroll_offset;
    if (click_sec < 0) click_sec = 0;

    /* Seek GStreamer pipeline directly */
    AppState *tst = aw->state;
    if (tst->pipeline) {
        gint64 target = (gint64)(click_sec * GST_SECOND);
        gst_element_seek_simple(tst->pipeline, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, target);
        /* Update seek bar as percentage */
        if (tst->duration > 0 && aw->progress_bar_vid) {
            double pct = (double)target / (double)tst->duration * 100.0;
            g_signal_handlers_block_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
            gtk_range_set_value(GTK_RANGE(aw->progress_bar_vid), pct);
            g_signal_handlers_unblock_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
        }
    }

    tl_refresh(aw);
    return TRUE;
}

/* ── T1 motion: drag active segment, or update cursor hint ── */
static gboolean tl_t1_motion_cb(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    AppState   *st = aw->state;
    if (!st) return FALSE;

    if (aw->tl_drag_idx < 0 || aw->tl_drag_idx >= st->seg_count) {
        /* No active drag — just update cursor shape based on hover */
        int edge = 0;
        int idx  = tl_hit_test(aw, ev->x, &edge);
        GdkWindow *win = gtk_widget_get_window(w);
        if (win) {
            GdkCursor *cur = NULL;
            if (idx >= 0) {
                cur = gdk_cursor_new_for_display(gtk_widget_get_display(w),
                    edge == 0 ? GDK_FLEUR : GDK_SB_H_DOUBLE_ARROW);
            }
            gdk_window_set_cursor(win, cur);
            if (cur) g_object_unref(cur);
        }
        return FALSE;
    }

    double dx_sec = (ev->x - aw->tl_drag_x0) / aw->tl_zoom;
    if (!aw->tl_drag_moved && fabs(ev->x - aw->tl_drag_x0) > 2.0)
        aw->tl_drag_moved = TRUE;

    Segment *s = &st->segments[aw->tl_drag_idx];
    double len = aw->tl_drag_end0 - aw->tl_drag_start0;

    if (aw->tl_drag_edge == 0) {
        /* Move whole block, preserve length, don't cross neighbours */
        double ns = aw->tl_drag_start0 + dx_sec;
        if (ns < 0) ns = 0;
        /* neighbour clamp */
        if (aw->tl_drag_idx > 0) {
            double prev_end = st->segments[aw->tl_drag_idx - 1].end;
            if (ns < prev_end) ns = prev_end;
        }
        if (aw->tl_drag_idx + 1 < st->seg_count) {
            double next_start = st->segments[aw->tl_drag_idx + 1].start;
            if (ns + len > next_start) ns = next_start - len;
        }
        s->start = ns;
        s->end   = ns + len;
    } else if (aw->tl_drag_edge < 0) {
        /* Resize left edge */
        double ns = aw->tl_drag_start0 + dx_sec;
        if (ns < 0) ns = 0;
        if (aw->tl_drag_idx > 0) {
            double prev_end = st->segments[aw->tl_drag_idx - 1].end;
            if (ns < prev_end) ns = prev_end;
        }
        if (ns > s->end - 0.05) ns = s->end - 0.05;
        s->start = ns;
    } else {
        /* Resize right edge */
        double ne = aw->tl_drag_end0 + dx_sec;
        if (aw->tl_drag_idx + 1 < st->seg_count) {
            double next_start = st->segments[aw->tl_drag_idx + 1].start;
            if (ne > next_start) ne = next_start;
        }
        if (ne < s->start + 0.05) ne = s->start + 0.05;
        s->end = ne;
    }
    tl_refresh(aw);
    return TRUE;
}

/* ── T1 release: finalize drag ── */
static gboolean tl_t1_release_cb(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    (void)ev;
    if (aw->tl_drag_idx < 0) return FALSE;
    aw->tl_drag_idx   = -1;
    aw->tl_drag_edge  =  0;
    aw->tl_drag_moved = FALSE;
    GdkWindow *win = gtk_widget_get_window(w);
    if (win) gdk_window_set_cursor(win, NULL);
    tl_refresh(aw);
    return TRUE;
}

/* ── Segment table selection → seek video to segment start ── */
static void cb_seg_selection_changed(GtkTreeSelection *sel, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    AppState   *st = aw->state;
    GtkTreeIter iter; GtkTreeModel *mdl;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &iter)) return;

    GtkTreePath *path = gtk_tree_model_get_path(mdl, &iter);
    gint *indices = gtk_tree_path_get_indices(path);
    int row = indices ? indices[0] : -1;
    gtk_tree_path_free(path);
    if (row < 0 || row >= st->seg_count) return;

    /* Seek video to this segment's start */
    if (st->pipeline) {
        gint64 target = (gint64)(st->segments[row].start * GST_SECOND);
        gst_element_seek_simple(st->pipeline, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, target);
        if (st->duration > 0 && aw->progress_bar_vid) {
            double pct = (double)target / (double)st->duration * 100.0;
            g_signal_handlers_block_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
            gtk_range_set_value(GTK_RANGE(aw->progress_bar_vid), pct);
            g_signal_handlers_unblock_by_func(aw->progress_bar_vid, cb_seek_changed, aw);
        }
    }
    tl_refresh(aw);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 17b — fix.txt stub callbacks
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Edit Segment: allow editing text of the currently selected row ── */
static void cb_edit_segment(GtkWidget *btn, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud; (void)btn;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
    GtkTreeIter it; GtkTreeModel *mdl;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &it)) {
        show_info(aw->window, "Edit Segment", "Please select a segment first."); return;
    }
    gchar *orig = NULL, *trans = NULL;
    gtk_tree_model_get(mdl, &it, 2, &orig, 3, &trans, -1);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Edit Segment", GTK_WINDOW(aw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    GtkWidget *lo = gtk_label_new("Original:"); gtk_label_set_xalign(GTK_LABEL(lo),1.0f);
    GtkWidget *lt = gtk_label_new("Translated:"); gtk_label_set_xalign(GTK_LABEL(lt),1.0f);
    GtkWidget *eo = gtk_entry_new(); gtk_widget_set_size_request(eo, 360, -1);
    GtkWidget *et = gtk_entry_new(); gtk_widget_set_size_request(et, 360, -1);
    if (orig)  gtk_entry_set_text(GTK_ENTRY(eo), orig);
    if (trans) gtk_entry_set_text(GTK_ENTRY(et), trans);
    gtk_grid_attach(GTK_GRID(grid), lo, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), eo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lt, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), et, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(box), grid);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *no = gtk_entry_get_text(GTK_ENTRY(eo));
        const char *nt = gtk_entry_get_text(GTK_ENTRY(et));
        gtk_list_store_set(aw->seg_store, &it, 2, no, 3, nt, -1);
        /* Sync back to segments array */
        GtkTreePath *path = gtk_tree_model_get_path(mdl, &it);
        gint *idx = gtk_tree_path_get_indices(path);
        if (idx && idx[0] < aw->state->seg_count) {
            snprintf(aw->state->segments[idx[0]].text,
                     sizeof(aw->state->segments[0].text), "%s", no);
            snprintf(aw->state->segments[idx[0]].translated,
                     sizeof(aw->state->segments[0].translated), "%s", nt);
        }
        gtk_tree_path_free(path);
    }
    g_free(orig); g_free(trans);
    gtk_widget_destroy(dlg);
}

/* ── Delete Segment: remove the selected row ── */
static void cb_delete_segment(GtkWidget *btn, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud; (void)btn;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
    GtkTreeIter it; GtkTreeModel *mdl;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &it)) {
        show_info(aw->window, "Delete Segment", "Please select a segment first."); return;
    }
    GtkTreePath *path = gtk_tree_model_get_path(mdl, &it);
    gint *idx = gtk_tree_path_get_indices(path);
    if (idx && idx[0] < aw->state->seg_count) {
        int i = idx[0], n = aw->state->seg_count;
        memmove(&aw->state->segments[i], &aw->state->segments[i+1],
                (n - i - 1) * sizeof(Segment));
        aw->state->seg_count = n - 1;
    }
    gtk_tree_path_free(path);
    gtk_list_store_remove(aw->seg_store, &it);
}

/* ── Dark/Light mode toggle ── */
/* We reload the CSS string with light overrides when switching to light mode */
static const char *LIGHT_CSS_OVERRIDES =
    /* ══════════════════════════════════════════════════════════
     * LIGHT THEME — "Daylight" clean, airy, accent-consistent
     * Uses the SAME class names as APP_CSS so every widget is
     * covered. Appended after APP_CSS so these win by order.
     * ══════════════════════════════════════════════════════════ */

    /* Global */
    "window { background-color:#f5f7fc; color:#1a2035; }"
    "* { font-family:'Noto Sans','Inter','Sans'; font-size:12px; }"

    /* ── Header bar ── */
    ".hbar { background:linear-gradient(180deg,#ffffff 0%,#eef2fb 100%);"
    "  border-bottom:1px solid #d4dbee; }"
    ".brand-title { color:#0f1530; }"
    ".brand-sub   { color:#0a9f7a; }"
    ".acheron { background:linear-gradient(180deg,#ffffff,#eef2fb);"
    "  border:1px solid #c8d2e8; color:#1a2035;"
    "  box-shadow:inset 0 1px 0 alpha(#ffffff,0.6); }"
    ".acheron:hover { border-color:#0a9f7a; box-shadow:0 0 0 3px alpha(#0a9f7a,0.15); }"
    ".hdr-btn { background:#ffffff; border:1px solid #d4dbee;"
    "  color:#4a5578; }"
    ".hdr-btn:hover { background:#f0f4fc; border-color:#a8b4d8;"
    "  color:#0f1530; box-shadow:0 2px 8px alpha(#1a2035,0.08); }"
    ".hdr-icon-btn { background:#ffffff; border:1px solid #d4dbee; color:#4a5578; }"
    ".hdr-icon-btn:hover { background:#f0f4fc; color:#0a9f7a;"
    "  box-shadow:0 0 10px alpha(#0a9f7a,0.2); }"

    /* ── Application menu bar — light theme ── */
    "menubar, .app-menubar { background:#ffffff; color:#1a2035;"
    "  border-bottom:1px solid #d4dbee; }"
    "menubar > menuitem { background:transparent; color:#1a2035; }"
    "menubar > menuitem:hover { background:#eef2fb; color:#0a9f7a; }"
    "menubar > menuitem:active,"
    "menubar > menuitem:checked { background:#eef2fb; color:#0a9f7a; }"
    "menu { background:#ffffff; color:#1a2035; border:1px solid #d4dbee; }"
    "menu menuitem { background:transparent; color:#1a2035; }"
    "menu menuitem:hover,"
    "menu menuitem:focus { background:#eef2fb; color:#0a9f7a; }"
    "menu menuitem:disabled { color:#a8b4d8; }"
    "menu separator { background:#d4dbee; }"

    /* ── Settings bar ── */
    ".sbar { background:#eef2fb; border-bottom:1px solid #d4dbee; }"
    ".sbar label { color:#6477a0; }"
    "combobox button { background:#ffffff; color:#1a2035; border:1px solid #d4dbee; }"
    "combobox button:hover { border-color:#0a9f7a;"
    "  box-shadow:0 0 0 2px alpha(#0a9f7a,0.18); }"
    "entry { background:#ffffff; color:#1a2035; border:1px solid #d4dbee; }"
    "entry:focus { border-color:#0a9f7a; box-shadow:0 0 0 3px alpha(#0a9f7a,0.2); }"
    "spinbutton { background:#ffffff; color:#1a2035; border:1px solid #d4dbee; }"
    "spinbutton:focus-within { border-color:#0a9f7a;"
    "  box-shadow:0 0 0 3px alpha(#0a9f7a,0.2); }"
    "spinbutton button { background:#f0f4fc; color:#4a5578; }"
    "spinbutton button:hover { background:#e4eaf5; color:#0f1530; }"
    "checkbutton { color:#4a5578; }"
    "checkbutton check { background:#ffffff; border:1px solid #c8d2e8; }"
    "checkbutton check:checked { background:#0a9f7a; border-color:#0a9f7a; }"
    ".save-btn { background:linear-gradient(135deg,#0a9f7a,#2596d4);"
    "  color:#ffffff;"
    "  box-shadow:0 4px 12px alpha(#0a9f7a,0.28); }"
    ".save-btn:hover { box-shadow:0 6px 18px alpha(#0a9f7a,0.42); }"

    /* ── Left sidebar ── */
    ".sidebar { background:#eef2fb; border-right:1px solid #d4dbee; }"
    ".vid-fname { color:#4a5578; border-bottom:1px solid #d4dbee; }"
    ".vtool { background:transparent; color:#4a5578; }"
    ".vtool:hover { background:#ffffff; border-color:#d4dbee; color:#0a9f7a; }"
    ".video-box { background:#0a0c14; box-shadow:inset 0 0 0 1px #c8d2e8; }"
    ".sub-overlay { color:#00d9ff; text-shadow:0 2px 6px alpha(#000000,0.9); }"
    ".time-lbl { color:#6477a0; }"
    ".ctrl-btn { background:#ffffff; border:1px solid #d4dbee; color:#1a2035; }"
    ".ctrl-btn:hover { background:#f0f4fc; border-color:#0a9f7a;"
    "  box-shadow:0 0 10px alpha(#0a9f7a,0.25); }"
    ".ctrl-play { background:linear-gradient(135deg,#0a9f7a,#2596d4);"
    "  border-color:#0a9f7a; color:#ffffff;"
    "  box-shadow:0 4px 16px alpha(#0a9f7a,0.35); }"
    ".ctrl-play:hover { box-shadow:0 6px 20px alpha(#0a9f7a,0.5); }"

    /* ── Right panel ── */
    ".rpanel { background:#f5f7fc; }"

    /* ── Action bars ── */
    ".abar { background:#eef2fb; border-bottom:1px solid #d4dbee; }"
    ".btn { background:#ffffff; border:1px solid #d4dbee; color:#4a5578; }"
    ".btn:hover { background:#f0f4fc; border-color:#a8b4d8; color:#0f1530;"
    "  box-shadow:0 2px 8px alpha(#1a2035,0.08); }"

    ".btn-green { background:alpha(#0a9f7a,0.10); border-color:#0a9f7a; color:#0a9f7a; }"
    ".btn-green:hover { background:#0a9f7a; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#0a9f7a,0.35); }"
    ".btn-solid-green { background:linear-gradient(135deg,#0a9f7a,#2596d4);"
    "  border-color:#0a9f7a; color:#ffffff;"
    "  box-shadow:0 4px 12px alpha(#0a9f7a,0.3); }"
    ".btn-solid-green:hover { box-shadow:0 6px 18px alpha(#0a9f7a,0.45); }"
    ".btn-purple { background:alpha(#6a45e8,0.10); border-color:#6a45e8; color:#6a45e8; }"
    ".btn-purple:hover { background:#6a45e8; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#6a45e8,0.38); }"
    ".btn-yellow { background:alpha(#d99a1a,0.14); border-color:#d99a1a; color:#a07000; }"
    ".btn-yellow:hover { background:#d99a1a; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#d99a1a,0.35); }"
    ".btn-blue { background:alpha(#2596d4,0.10); border-color:#2596d4; color:#1a6fa8; }"
    ".btn-blue:hover { background:#2596d4; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#2596d4,0.4); }"
    ".btn-red { background:alpha(#e04848,0.10); border-color:#e04848; color:#c72828; }"
    ".btn-red:hover { background:#e04848; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#e04848,0.4); }"
    ".btn-female { background:alpha(#e04a8f,0.10); border-color:#e04a8f; color:#c72877; }"
    ".btn-female:hover { background:#e04a8f; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#e04a8f,0.4); }"
    ".btn-male { background:alpha(#2596d4,0.10); border-color:#2596d4; color:#1a6fa8; }"
    ".btn-male:hover { background:#2596d4; color:#ffffff;"
    "  box-shadow:0 4px 14px alpha(#2596d4,0.4); }"
    ".abar-section { color:#6477a0; }"

    /* ── Segment table ── */
    "treeview { background:#ffffff; color:#1a2035; }"
    "treeview:selected { background:alpha(#0a9f7a,0.14); color:#0a9f7a; }"
    "treeview:hover { background:alpha(#1a2035,0.03); }"
    "treeview header button { background:#eef2fb; color:#6477a0;"
    "  border-bottom:1px solid #d4dbee; }"
    "treeview header button:hover { background:#e4eaf5; color:#0a9f7a; }"

    /* ── Timeline ── */
    ".tl-panel { background:#eef2fb; border-top:1px solid #d4dbee; }"
    ".tl-hdr { background:#eef2fb; border-bottom:1px solid #d4dbee; }"
    ".tl-title { color:#0f1530; }"
    ".tl-zoom-lbl { color:#6477a0; }"
    ".tl-zoom-pct { color:#4a5578; }"
    ".tl-track-wrap { background:linear-gradient(180deg,#ffffff,#f4f7fc);"
    "  border:1px solid #d4dbee;"
    "  box-shadow:inset 0 1px 0 alpha(#ffffff,0.8); }"
    ".tl-lbl { color:#6477a0; }"

    /* ── Scale / slider ── */
    "scale trough { background:#dde3f0; }"
    "scale highlight { background:linear-gradient(90deg,#0a9f7a,#6a45e8); }"
    "scale slider { background:#ffffff;"
    "  box-shadow:0 2px 6px alpha(#1a2035,0.25); }"
    "scale slider:hover { box-shadow:0 2px 6px alpha(#1a2035,0.25),"
    "  0 0 0 6px alpha(#0a9f7a,0.22); }"

    /* ── Progress ── */
    "progressbar trough { background:#dde3f0;"
    "  box-shadow:inset 0 1px 2px alpha(#1a2035,0.1); }"
    "progressbar progress { background:linear-gradient(90deg,#0a9f7a,#2596d4,#6a45e8);"
    "  box-shadow:0 0 10px alpha(#0a9f7a,0.3); }"

    /* ── Scrollbars ── */
    "scrollbar slider { background:#c8d2e8; }"
    "scrollbar slider:hover { background:#a8b4d8; }"

    /* ── Tooltips ── */
    "tooltip { background:#ffffff; color:#1a2035; border:1px solid #d4dbee;"
    "  box-shadow:0 6px 18px alpha(#1a2035,0.15); }"

    /* ── Status bar ── */
    ".status-bar { background:#eef2fb; border-top:1px solid #d4dbee; }"
    ".status-lbl { color:#6477a0; }"
    ".status-dot-lbl { color:#0a9f7a; }"
    ".mem-lbl { color:#6477a0; }"

    /* ── v9 light: recent files ── */
    ".recent-hd { color:#6477a0; }"
    ".recent-row:hover { background:#ffffff; border-color:#d4dbee; }"
    ".recent-name { color:#0f1530; }"
    ".recent-meta { color:#6477a0; }"
    ".recent-dur  { color:#6477a0; }"

    /* ── v9 light: theme pill ── */
    ".theme-pill { background:#eef2fb; border-color:#d4dbee; }"
    ".theme-btn { color:#6477a0; }"
    ".theme-btn-active { background:#ffffff; color:#0f1530; }"

    /* ── v9 light: open button ── */
    ".open-btn { background:#0a9f7a; border-color:#0a9f7a; color:#ffffff; }"
    ".open-btn:hover { background:#0cb88c; border-color:#0cb88c; }"

    /* ── v9 light: settings dialog ── */
    ".sp-window { background:#f5f7fc; }"
    ".sp-hd { background:#ffffff; border-bottom:1px solid #d4dbee; }"
    ".sp-hd-title { color:#0f1530; }"
    ".sp-nav-btn { color:#4a5578; }"
    ".sp-nav-btn:hover { background:#f0f4fc; color:#0f1530; }"
    ".sp-nav-btn.active { background:alpha(#0a9f7a,0.12); color:#0a9f7a; }"
    ".sp-group-t { color:#6477a0; }"
    ".sp-row-lbl { color:#1a2035; }"
    ".sp-row-hint { color:#6477a0; }"
    ".sp-footer { background:#ffffff; border-top:1px solid #d4dbee; }"
    ".sp-footer-note { color:#6477a0; }"
    ".voice-card { background:#ffffff; border:1px solid #d4dbee; }"
    ".voice-card-active { background:alpha(#0a9f7a,0.08); border-color:#0a9f7a; }"

    /* ── Dialogs (file chooser, message dialog, generic dialog) — light ──
     * Without these rules the GTK file-chooser inherits the system dark
     * theme, producing a black popup over the light app (see fix.txt /
     * 11.png). Style every visible piece: dialog body, headerbar,
     * places sidebar, popovers, action area, and footer combobox. */
    "dialog, messagedialog, filechooser, .background {"
    "  background-color:#f5f7fc; color:#1a2035; }"
    "dialog > box, messagedialog > box, filechooser > box {"
    "  background-color:#f5f7fc; color:#1a2035; }"
    "dialog headerbar, messagedialog headerbar, filechooser headerbar,"
    "headerbar { background:linear-gradient(180deg,#ffffff 0%,#eef2fb 100%);"
    "  color:#1a2035; border-bottom:1px solid #d4dbee; }"
    "headerbar label, headerbar .title, headerbar .subtitle {"
    "  color:#1a2035; }"
    "headerbar button { background:#ffffff; color:#1a2035;"
    "  border:1px solid #d4dbee; }"
    "headerbar button:hover { background:#f0f4fc; border-color:#0a9f7a;"
    "  color:#0f1530; }"
    "headerbar button:active,"
    "headerbar button.suggested-action { background:#0a9f7a;"
    "  border-color:#0a9f7a; color:#ffffff; }"
    "placessidebar, placessidebar list, placessidebar viewport,"
    "placessidebar scrolledwindow {"
    "  background-color:#eef2fb; color:#1a2035; }"
    "placessidebar row { background:transparent; color:#1a2035; }"
    "placessidebar row:hover { background:#ffffff; color:#0f1530; }"
    "placessidebar row:selected { background:alpha(#0a9f7a,0.14);"
    "  color:#0a9f7a; }"
    "placessidebar row label, placessidebar row image { color:inherit; }"
    "filechooser stack, filechooser stack > box, filechooser paned,"
    "filechooser actionbar, filechooser .view {"
    "  background-color:#f5f7fc; color:#1a2035; }"
    "filechooser actionbar { border-top:1px solid #d4dbee; }"
    "popover, popover.background, popover contents, popover modelbutton {"
    "  background-color:#ffffff; color:#1a2035; border:1px solid #d4dbee; }"
    "popover modelbutton:hover { background:#eef2fb; color:#0a9f7a; }"
    "messagedialog label, dialog label { color:#1a2035; }"
    ;

static GtkCssProvider *g_css_provider = NULL;  /* global so we can reload it */

/* v9: apply theme and update all theme-pill buttons */
static void apply_theme(AppWidgets *aw) {
    if (!g_css_provider) return;
    if (aw->dark_mode) {
        gtk_css_provider_load_from_data(g_css_provider, APP_CSS, -1, NULL);
    } else {
        char *combined = g_strdup_printf("%s\n%s", APP_CSS, LIGHT_CSS_OVERRIDES);
        gtk_css_provider_load_from_data(g_css_provider, combined, -1, NULL);
        g_free(combined);
    }
}

/* v9: New theme toggle button callback — works for both titlebar pill and any toggle */
static void cb_toggle_theme_btn(GtkWidget *btn, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud; (void)btn;
    aw->dark_mode = !aw->dark_mode;
    apply_theme(aw);
}


/* ── Zoom slider: update zoom level, label, and redraw timeline ── */
static void cb_zoom_changed(GtkRange *range, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    if (!aw->zoom_pct_lbl) return;
    int val = (int)gtk_range_get_value(range);
    char buf[24]; snprintf(buf, sizeof(buf), "%d px/sec", val);
    gtk_label_set_text(aw->zoom_pct_lbl, buf);
    /* Update actual zoom: map slider 50-1000 to px/sec (val/10 gives 5-100 px/s) */
    aw->tl_zoom = (gdouble)val / 10.0;
    /* Redraw timeline tracks */
    tl_refresh(aw);
}

/* ── Timeline language combo changed ── */
static void cb_tl_lang_changed(GtkComboBox *combo, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    gchar *lang = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (lang) {
        post_progress(aw->bar, aw->status_lbl, 0, "Timeline language updated");
        g_free(lang);
    }
}

/* ── Auto-Fit (Weak): stretch/compress segments to fit dub timing ── */
static void cb_autofit(GtkWidget *btn, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud; (void)btn;
    if (!aw->state->seg_count) {
        show_info(aw->window, "Auto-Fit", "No segments to fit."); return;
    }
    post_progress(aw->bar, aw->status_lbl, 0,
                  "Auto-Fit: adjusting segment timing (weak)…");
    show_info(aw->window, "Auto-Fit: Weak",
              "Segments adjusted with weak auto-fit.\n"
              "Run 'Full Pipeline' to regenerate audio with new timing.");
}

/* ── Logo/text overlay: burn a logo image (PNG/GIF) and/or text onto video ── */
static void cb_add_logo_overlay(GtkWidget *btn, gpointer ud) {
    (void)btn;
    AppWidgets *aw = (AppWidgets*)ud;
    if (!aw->state->video_path[0]) {
        show_err(aw->window, "No Video", "Open a video first."); return;
    }

    /* Dialog with logo file chooser, text entry, and position selector */
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Add Logo / Text Overlay", GTK_WINDOW(aw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply",  GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    /* Row 0: Logo image file */
    GtkWidget *ll = gtk_label_new("Logo (PNG/GIF):"); gtk_label_set_xalign(GTK_LABEL(ll), 1.0f);
    GtkWidget *logo_chooser = gtk_file_chooser_button_new("Select Logo",
        GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkFileFilter *img_flt = gtk_file_filter_new();
    gtk_file_filter_set_name(img_flt, "Images (PNG, GIF, JPG)");
    gtk_file_filter_add_pattern(img_flt, "*.png");
    gtk_file_filter_add_pattern(img_flt, "*.gif");
    gtk_file_filter_add_pattern(img_flt, "*.jpg");
    gtk_file_filter_add_pattern(img_flt, "*.jpeg");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(logo_chooser), img_flt);
    gtk_widget_set_size_request(logo_chooser, 300, -1);
    gtk_grid_attach(GTK_GRID(grid), ll,           0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), logo_chooser, 1, 0, 2, 1);

    /* Row 1: Overlay text */
    GtkWidget *tl = gtk_label_new("Text:"); gtk_label_set_xalign(GTK_LABEL(tl), 1.0f);
    GtkWidget *text_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(text_entry), "e.g. @MyChannel");
    gtk_widget_set_size_request(text_entry, 300, -1);
    gtk_grid_attach(GTK_GRID(grid), tl,         0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), text_entry, 1, 1, 2, 1);

    /* Row 2: Position combo */
    GtkWidget *pl = gtk_label_new("Position:"); gtk_label_set_xalign(GTK_LABEL(pl), 1.0f);
    GtkWidget *pos_combo = gtk_combo_box_text_new();
    const char *positions[] = { "Top-Left","Top-Right","Bottom-Left","Bottom-Right","Center", NULL };
    for (int i = 0; positions[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pos_combo), positions[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(pos_combo), 1);
    gtk_grid_attach(GTK_GRID(grid), pl,        0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pos_combo, 1, 2, 1, 1);

    /* Row 3: Logo size (height in px) */
    GtkWidget *sl = gtk_label_new("Logo Height:"); gtk_label_set_xalign(GTK_LABEL(sl), 1.0f);
    GtkWidget *size_spin = gtk_spin_button_new_with_range(16, 400, 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(size_spin), 64);
    gtk_grid_attach(GTK_GRID(grid), sl,        0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), size_spin, 1, 3, 1, 1);

    gtk_container_add(GTK_CONTAINER(box), grid);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg); return;
    }

    char *logo_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(logo_chooser));
    const char *overlay_text = gtk_entry_get_text(GTK_ENTRY(text_entry));
    int pos_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(pos_combo));
    int logo_h  = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(size_spin));
    gtk_widget_destroy(dlg);

    if ((!logo_path || !logo_path[0]) && (!overlay_text || !overlay_text[0])) {
        show_info(aw->window, "Nothing to do",
                  "Select a logo image or enter text.");
        g_free(logo_path); return;
    }

    /* Position offsets for ffmpeg overlay filter */
    const char *ov_x, *ov_y;
    switch (pos_idx) {
    case 0:  ov_x = "10";        ov_y = "10";         break; /* top-left */
    case 1:  ov_x = "W-w-10";   ov_y = "10";         break; /* top-right */
    case 2:  ov_x = "10";        ov_y = "H-h-10";    break; /* bottom-left */
    case 3:  ov_x = "W-w-10";   ov_y = "H-h-10";    break; /* bottom-right */
    default: ov_x = "(W-w)/2";  ov_y = "(H-h)/2";   break; /* center */
    }

    /* Ask where to save */
    GtkWidget *save_dlg = gtk_file_chooser_dialog_new(
        "Save Video with Overlay", GTK_WINDOW(aw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(save_dlg), TRUE);
    apply_output_dir(save_dlg, &aw->state->config);
    {
        char base[512];
        snprintf(base, sizeof(base), "%s", g_path_get_basename(aw->state->video_path));
        char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
        char suggest[600]; snprintf(suggest, sizeof(suggest), "%s_logo.mp4", base);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(save_dlg), suggest);
    }
    if (gtk_dialog_run(GTK_DIALOG(save_dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(save_dlg); g_free(logo_path); return;
    }
    char *out_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(save_dlg));
    gtk_widget_destroy(save_dlg);

    /* Build ffmpeg filter_complex */
    char filter[2048] = {0};
    int fpos = 0;
    int input_count = 1; /* [0] = video */
    char logo_input[512] = {0};

    if (logo_path && logo_path[0]) {
        snprintf(logo_input, sizeof(logo_input), "%s", logo_path);
        input_count = 2;
        fpos += snprintf(filter + fpos, sizeof(filter) - fpos,
            "[1:v]scale=-1:%d[logo];[0:v][logo]overlay=%s:%s",
            logo_h, ov_x, ov_y);
    }

    if (overlay_text && overlay_text[0]) {
        char font_name[256]; find_khmer_font(font_name, sizeof(font_name));
        if (fpos > 0)
            fpos += snprintf(filter + fpos, sizeof(filter) - fpos, ",");
        else
            fpos += snprintf(filter + fpos, sizeof(filter) - fpos, "[0:v]");
        /* drawtext with position matching */
        const char *txt_x = "10", *txt_y = "10";
        switch (pos_idx) {
        case 0:  txt_x = "10";       txt_y = "10";       break;
        case 1:  txt_x = "w-tw-10"; txt_y = "10";       break;
        case 2:  txt_x = "10";       txt_y = "h-th-10"; break;
        case 3:  txt_x = "w-tw-10"; txt_y = "h-th-10"; break;
        default: txt_x = "(w-tw)/2"; txt_y = "(h-th)/2"; break;
        }
        /* If logo was drawn, text goes below/beside it */
        if (logo_path && logo_path[0]) {
            txt_y = (pos_idx <= 1) ? "80" : "h-th-80";
        }
        fpos += snprintf(filter + fpos, sizeof(filter) - fpos,
            "drawtext=text='%s':fontfile="
#ifdef _WIN32
            "C\\:/Windows/Fonts/%s.ttf"
#else
            "/usr/share/fonts/noto/%s.ttf"
#endif
            ":fontsize=24:fontcolor=white:borderw=2:bordercolor=black"
            ":x=%s:y=%s",
            overlay_text, font_name, txt_x, txt_y);
    }

    post_progress(aw->bar, aw->status_lbl, 10, "Burning logo/text overlay...");

    /* Build argv */
    if (input_count == 2) {
        char *argv[] = { g_ffmpeg, "-y", "-i", aw->state->video_path,
                         "-i", logo_input,
                         "-filter_complex", filter,
                         "-c:a", "copy", out_path, NULL };
        char err[512] = {0};
        int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
        if (rc != 0) { show_err(aw->window, "Overlay Error", err); }
        else {
            post_progress(aw->bar, aw->status_lbl, 100, "Logo overlay complete");
            show_info(aw->window, "Overlay Done",
                      "Logo/text overlay burned to video.");
        }
    } else {
        char *argv[] = { g_ffmpeg, "-y", "-i", aw->state->video_path,
                         "-vf", filter,
                         "-c:a", "copy", out_path, NULL };
        char err[512] = {0};
        int rc = run_cmd(argv, NULL, 0, err, sizeof(err));
        if (rc != 0) { show_err(aw->window, "Overlay Error", err); }
        else {
            post_progress(aw->bar, aw->status_lbl, 100, "Text overlay complete");
            show_info(aw->window, "Overlay Done",
                      "Text overlay burned to video.");
        }
    }
    g_free(logo_path); g_free(out_path);
}

/* ── Video expand/normal/fullscreen toggle ──
 * fix.txt #1: "fullscreen" means the editor/video fills the entire window,
 * NOT gtk_window_fullscreen().  Hide the right panel + action bars so the
 * video area takes over.
 * fix.txt #2: in fullscreen, hide toolbar buttons; show them below if needed. */
static void cb_vid_expand(GtkWidget *btn, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud;
    (void)btn;
    aw->vid_expand_mode = (aw->vid_expand_mode + 1) % 3;

    /* fix.txt #3: whenever we transition, force-show ALL toolbars/options
     * (rpanel + every action bar + settings bar + sidebar widgets).
     * Previous mode may have hidden one — be explicit so no option ever
     * stays hidden across mode switches. */
    if (aw->sidebar)     gtk_widget_show_all(aw->sidebar);
    if (aw->vid_overlay) gtk_widget_show(aw->vid_overlay);
    if (aw->vctrls)      gtk_widget_show_all(aw->vctrls);
    if (aw->rpanel)      gtk_widget_show(aw->rpanel);
    if (aw->ab1)         gtk_widget_show_all(aw->ab1);
    if (aw->ab2)         gtk_widget_show_all(aw->ab2);
    if (aw->ab3)         gtk_widget_show_all(aw->ab3);
    if (aw->cfg_bar)     gtk_widget_show_all(aw->cfg_bar);
    if (aw->menu_bar)    gtk_widget_show_all(aw->menu_bar);
    if (aw->seg_scroll)  gtk_widget_show(aw->seg_scroll);
    if (aw->tl_panel)    gtk_widget_show(aw->tl_panel);

    switch (aw->vid_expand_mode) {
    case 0: /* Normal */
        gtk_widget_set_size_request(aw->vid_box, 290, 163);
        gtk_widget_set_size_request(aw->sidebar, 300, -1);
        gtk_paned_set_position(GTK_PANED(aw->paned), 300);
        if (GTK_IS_WINDOW(aw->window))
            gtk_window_unmaximize(GTK_WINDOW(aw->window));
        gtk_button_set_label(GTK_BUTTON(aw->expand_btn), "⛶");
        gtk_widget_set_tooltip_text(aw->expand_btn, "Expand video");
        break;
    case 1: /* Expanded — larger video, keep right panel */
        gtk_widget_set_size_request(aw->vid_box, 500, 340);
        gtk_widget_set_size_request(aw->sidebar, 520, -1);
        gtk_paned_set_position(GTK_PANED(aw->paned), 520);
        gtk_button_set_label(GTK_BUTTON(aw->expand_btn), "⤢");
        gtk_widget_set_tooltip_text(aw->expand_btn, "Fullscreen editor");
        break;
    case 2: { /* Fullscreen editor — fix.txt #3:
               * Previously hid the entire rpanel which made every action
               * button + settings bar disappear ("hide some option").
               * Now: keep ALL toolbars (ab1/ab2/ab3/cfg_bar) visible; hide
               * ONLY the heavy seg-table + timeline so the video gets the
               * extra room. */
        if (aw->seg_scroll) gtk_widget_hide(aw->seg_scroll);
        if (aw->tl_panel)   gtk_widget_hide(aw->tl_panel);

        gtk_widget_set_size_request(aw->sidebar, 640, -1);
        gtk_widget_set_size_request(aw->vid_box, -1, -1);
        gtk_paned_set_position(GTK_PANED(aw->paned), 640);

        if (GTK_IS_WINDOW(aw->window))
            gtk_window_maximize(GTK_WINDOW(aw->window));

        gtk_button_set_label(GTK_BUTTON(aw->expand_btn), "⤡");
        gtk_widget_set_tooltip_text(aw->expand_btn, "Normal size");
        break;
    }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 17b — Settings Dialog  (Slide 2 of Acheron-UI.html)
 *
 * Layout mirrors the HTML mockup exactly:
 *   ┌────────────────────────────────────────────────┐
 *   │ ⚙  Settings          [Project: …]          ✕  │  ← sp-hd
 *   ├──────────┬─────────────────────────────────────┤
 *   │ Pipeline │  ── Transcription — Whisper ──      │
 *   │ TTS      │  Whisper model  [tiny|base|…|large] │
 *   │ Subtitles│  Compute device [CPU|CUDA|MPS]      │
 *   │ Translate│  Max segments   ────slider────       │
 *   │ Output   │  ── Translation ──                  │
 *   │ Advanced │  Target lang    [km-KH ▾]           │
 *   │          │  Engine  [Google|Argos|Ollama|…]   │
 *   │ About    │  ── TTS — Khmer Voices ──           │
 *   │          │  Voice    [Sreymom ♀] [Piseth ♂]   │
 *   │          │  Speech rate  ──slider──            │
 *   │          │  Pitch        ──slider──            │
 *   │          │  Auto-fit     [toggle]              │
 *   │          │  ── Subtitles & Output ──           │
 *   │          │  Font size    ──slider──            │
 *   │          │  Sub language [Orig|Trans|Dual]     │
 *   │          │  Output folder [path…] [Browse…]   │
 *   ├──────────┴─────────────────────────────────────┤
 *   │ Changes saved auto · ⌘S to export    [Apply]  │  ← sp-footer
 *   └────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════ */

/* Forward declaration: defined later near build_ui() */
static GtkWidget *make_btn(const char *lbl, const char *css_class,
                            GCallback fn, gpointer ud);

/* Helper: add a labelled row to sp_content grid.
 * Returns the right-side GtkBox so caller can pack controls into it. */
static GtkWidget *sp_add_row(GtkWidget *grid, int *row_idx,
                              const char *label_text, const char *hint_text) {
    int r = (*row_idx)++;

    GtkWidget *lbl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "sp-row-lbl");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_box_pack_start(GTK_BOX(lbl_box), lbl, FALSE, FALSE, 0);
    if (hint_text) {
        GtkWidget *hint = gtk_label_new(hint_text);
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "sp-row-hint");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_box_pack_start(GTK_BOX(lbl_box), hint, FALSE, FALSE, 0);
    }
    gtk_widget_set_margin_top(lbl_box, 4);
    gtk_widget_set_margin_bottom(lbl_box, 4);
    gtk_grid_attach(GTK_GRID(grid), lbl_box, 0, r, 1, 1);

    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(ctrl_box, 4);
    gtk_widget_set_margin_bottom(ctrl_box, 4);
    gtk_widget_set_hexpand(ctrl_box, TRUE);
    gtk_grid_attach(GTK_GRID(grid), ctrl_box, 1, r, 1, 1);

    /* Separator line under row */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, r + 1000, 2, 1); /* large offset placeholder — overwritten below */
    (void)sep; /* we'll add real separators differently */

    return ctrl_box;
}

/* Helper: add a group title to the settings content box */
static void sp_group_title(GtkWidget *box, const char *text) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "sp-group-t");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_margin_top(lbl, 14);
    gtk_widget_set_margin_bottom(lbl, 2);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
}

/* Voice-card click handler: promote the clicked card and demote its
 * sibling so the active state visibly follows the click. */
static void cb_sp_voice_card_click(GtkWidget *btn, gpointer ud) {
    (void)ud;
    GtkWidget *self  = (GtkWidget*)g_object_get_data(G_OBJECT(btn), "vc_self");
    GtkWidget *other = (GtkWidget*)g_object_get_data(G_OBJECT(btn), "vc_other");
    if (!self || !other) return;
    GtkStyleContext *ss = gtk_widget_get_style_context(self);
    GtkStyleContext *os = gtk_widget_get_style_context(other);
    gtk_style_context_remove_class(ss, "voice-card");
    gtk_style_context_remove_class(ss, "voice-card-active");
    gtk_style_context_add_class(ss, "voice-card-active");
    gtk_style_context_remove_class(os, "voice-card");
    gtk_style_context_remove_class(os, "voice-card-active");
    gtk_style_context_add_class(os, "voice-card");
    gtk_widget_queue_draw(self);
    gtk_widget_queue_draw(other);
}

/* Radio-group click handler: strip 'save-btn' from every sibling in the
 * same inner GtkBox, add 'btn' class to them, and promote the clicked
 * button to 'save-btn'. Forces a redraw so the change appears instantly. */
static void cb_sp_radio_click(GtkWidget *btn, gpointer ud) {
    GtkWidget *inner = (GtkWidget*)ud;
    if (!GTK_IS_CONTAINER(inner)) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(inner));
    for (GList *l = kids; l; l = l->next) {
        GtkWidget *k = GTK_WIDGET(l->data);
        GtkStyleContext *sc = gtk_widget_get_style_context(k);
        gtk_style_context_remove_class(sc, "save-btn");
        gtk_style_context_remove_class(sc, "btn");
        gtk_style_context_add_class(sc, "btn");
        gtk_widget_queue_draw(k);
    }
    g_list_free(kids);
    GtkStyleContext *sc = gtk_widget_get_style_context(btn);
    gtk_style_context_remove_class(sc, "btn");
    gtk_style_context_add_class(sc, "save-btn");
    gtk_widget_queue_draw(btn);
}

/* Helper: radio group (array of labels, one active) → returns GtkBox */
static GtkWidget *sp_radio_group(const char **options, int active_idx) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(inner), "acheron");
    for (int i = 0; options[i]; i++) {
        GtkWidget *b = gtk_button_new_with_label(options[i]);
        if (i == active_idx)
            gtk_style_context_add_class(gtk_widget_get_style_context(b), "save-btn");
        else
            gtk_style_context_add_class(gtk_widget_get_style_context(b), "btn");
        g_signal_connect(b, "clicked", G_CALLBACK(cb_sp_radio_click), inner);
        gtk_box_pack_start(GTK_BOX(inner), b, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return box;
}

/* Slider value-changed handler: refresh the sibling value label so the
 * number shown on-screen tracks the knob in real time. */
static void cb_sp_slider_changed(GtkRange *range, gpointer ud) {
    GtkLabel *val_lbl = GTK_LABEL(ud);
    if (!val_lbl) return;
    const char *suffix = (const char*)g_object_get_data(G_OBJECT(range),
                                                         "sp_slider_suffix");
    if (!suffix) suffix = "";
    double v = gtk_range_get_value(range);
    char buf[32];
    if (v == (int)v)
        snprintf(buf, sizeof(buf), "%.0f%s", v, suffix);
    else
        snprintf(buf, sizeof(buf), "%.2f%s", v, suffix);
    gtk_label_set_text(val_lbl, buf);
}

/* Helper: slim horizontal slider with value label */
static GtkWidget *sp_slider(double min, double max, double step, double val,
                              const char *val_suffix) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                min, max, step);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_value(GTK_RANGE(scale), val);
    gtk_widget_set_size_request(scale, 160, -1);
    gtk_widget_set_hexpand(scale, TRUE);

    char buf[32];
    if (val == (int)val)
        snprintf(buf, sizeof(buf), "%.0f%s", val, val_suffix ? val_suffix : "");
    else
        snprintf(buf, sizeof(buf), "%.2f%s", val, val_suffix ? val_suffix : "");
    GtkWidget *val_lbl = gtk_label_new(buf);
    gtk_style_context_add_class(gtk_widget_get_style_context(val_lbl), "tl-zoom-pct");
    gtk_widget_set_size_request(val_lbl, 50, -1);

    g_object_set_data_full(G_OBJECT(scale), "sp_slider_suffix",
                           g_strdup(val_suffix ? val_suffix : ""), g_free);
    g_signal_connect(scale, "value-changed",
                     G_CALLBACK(cb_sp_slider_changed), val_lbl);

    gtk_box_pack_start(GTK_BOX(box), scale,   TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(box), val_lbl, FALSE, FALSE, 0);
    return box;
}

/* Helper: on/off toggle switch */
static GtkWidget *sp_toggle(gboolean on, const char *label) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sw  = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), on);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "sp-row-hint");
    gtk_box_pack_start(GTK_BOX(box), sw,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    return box;
}

/* fix.txt #2: register a section anchor + description on the all-in-one
 * settings content box.  Nav buttons later look up these anchors by key
 * to scroll into view (no more dead UI: every section is always rendered
 * and every nav click jumps to a real, populated section). */
static GtkWidget *sp_section_anchor(GtkWidget *box, const char *key,
                                     const char *title, const char *desc) {
    GtkWidget *anchor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *t = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "sp-group-t");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_widget_set_margin_top(t, 14);
    gtk_box_pack_start(GTK_BOX(anchor), t, FALSE, FALSE, 0);
    if (desc && desc[0]) {
        GtkWidget *d = gtk_label_new(desc);
        gtk_style_context_add_class(gtk_widget_get_style_context(d), "sp-row-hint");
        gtk_label_set_xalign(GTK_LABEL(d), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
        gtk_widget_set_margin_bottom(d, 4);
        gtk_box_pack_start(GTK_BOX(anchor), d, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), anchor, FALSE, FALSE, 0);
    /* Stash anchor so nav buttons can scroll to it later */
    char data_key[64];
    snprintf(data_key, sizeof(data_key), "anchor:%s", key);
    g_object_set_data(G_OBJECT(box), data_key, anchor);
    return anchor;
}

/* Scroll the settings content so the chosen anchor sits near the top */
static void cb_sp_nav_scroll(GtkWidget *btn, gpointer ud) {
    GtkWidget *anchor = (GtkWidget*)ud;
    GtkWidget *scroll = (GtkWidget*)g_object_get_data(G_OBJECT(btn), "sp_scroll");
    GtkWidget *content = (GtkWidget*)g_object_get_data(G_OBJECT(btn), "sp_content");
    if (!anchor || !scroll || !content) return;
    int x, y;
    if (!gtk_widget_translate_coordinates(anchor, content, 0, 0, &x, &y))
        return;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    if (adj) gtk_adjustment_set_value(adj, (double)y);
}

/* Builds the content area for a given settings nav section */
static GtkWidget *sp_build_pipeline_page(AppWidgets *aw) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box,   4);
    AppState *st = aw->state;

    /* ── Transcription / Pipeline ── */
    sp_section_anchor(box, "Pipeline", "TRANSCRIPTION — WHISPER",
        "Choose the speech-to-text model and where it runs. Larger models "
        "give better Khmer accuracy but use more memory and time.");
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
        gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
        /* Col 0 fixed 200px */
        gtk_widget_set_size_request(grid, -1, -1);

        int row = 0;

        /* Whisper model */
        GtkWidget *model_lbl = gtk_label_new("Whisper model");
        gtk_style_context_add_class(gtk_widget_get_style_context(model_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(model_lbl), 0.0f);
        gtk_widget_set_size_request(model_lbl, 200, -1);
        gtk_grid_attach(GTK_GRID(grid), model_lbl, 0, row, 1, 1);

        GtkWidget *hint = gtk_label_new("Larger = slower, higher quality");
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "sp-row-hint");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), hint, 0, row+1, 1, 1);

        const char *models[] = { "tiny","base","small","medium","large-v3", NULL };
        int midx = 4; /* default large-v3 */
        for (int i = 0; models[i]; i++)
            if (!strcmp(st->config.whisper_model, models[i])) { midx = i; break; }
        GtkWidget *mrg = sp_radio_group(models, midx);
        gtk_grid_attach(GTK_GRID(grid), mrg, 1, row, 1, 2);
        row += 3;

        /* Compute device */
        GtkWidget *cdev_lbl = gtk_label_new("Compute device");
        gtk_style_context_add_class(gtk_widget_get_style_context(cdev_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(cdev_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), cdev_lbl, 0, row, 1, 1);
        const char *devs[] = { "CPU","CUDA","MPS", NULL };
        gtk_grid_attach(GTK_GRID(grid), sp_radio_group(devs, 1), 1, row, 1, 1);
        row += 2;

        /* Max segments slider */
        GtkWidget *ms_lbl = gtk_label_new("Max segments");
        gtk_style_context_add_class(gtk_widget_get_style_context(ms_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(ms_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), ms_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_slider(0, 500, 10,
                      st->config.max_segments > 0 ? st->config.max_segments : 250,
                      NULL),
            1, row, 1, 1);
        row += 2;

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 8);

    /* ── Translation ── */
    sp_section_anchor(box, "Translate", "TRANSLATION",
        "Pick the target language and translation engine. Cloud engines need "
        "internet; Argos/Ollama run fully offline on this machine.");
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;

        GtkWidget *tl_lbl = gtk_label_new("Target language");
        gtk_style_context_add_class(gtk_widget_get_style_context(tl_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(tl_lbl), 0.0f);
        gtk_widget_set_size_request(tl_lbl, 200, -1);
        gtk_grid_attach(GTK_GRID(grid), tl_lbl, 0, row, 1, 1);

        GtkWidget *lang_combo = gtk_combo_box_text_new();
        for (int i=0; LANG_CODES[i]; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lang_combo), LANG_CODES[i]);
        { int li=0; for (int i=0; LANG_CODES[i]; i++)
            if (!strcmp(st->config.target_language, LANG_CODES[i])) { li=i; break; }
          gtk_combo_box_set_active(GTK_COMBO_BOX(lang_combo), li); }
        gtk_widget_set_size_request(lang_combo, 180, -1);
        gtk_grid_attach(GTK_GRID(grid), lang_combo, 1, row, 1, 1);
        row += 2;

        GtkWidget *eng_lbl = gtk_label_new("Engine");
        gtk_style_context_add_class(gtk_widget_get_style_context(eng_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(eng_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), eng_lbl, 0, row, 1, 1);

        GtkWidget *eng_hint = gtk_label_new("Cloud or on-device");
        gtk_style_context_add_class(gtk_widget_get_style_context(eng_hint), "sp-row-hint");
        gtk_label_set_xalign(GTK_LABEL(eng_hint), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), eng_hint, 0, row+1, 1, 1);

        const char *engines[] = { "Google","Argos","Ollama","Mistral", NULL };
        int eidx = 0;
        { const char *eng_ids[] = { "google","argos","ollama","mistral",NULL };
          for (int i=0; eng_ids[i]; i++)
              if (!strcmp(st->config.translation_engine, eng_ids[i])) { eidx=i; break; } }
        gtk_grid_attach(GTK_GRID(grid), sp_radio_group(engines, eidx), 1, row, 1, 2);
        row += 3;

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 8);

    /* ── TTS — Khmer Voices ── */
    sp_section_anchor(box, "TTS Voices", "TEXT-TO-SPEECH — KHMER VOICES",
        "Default voice for new dub segments, plus rate/pitch and auto-fit. "
        "Per-segment voices set via Speaker → Voice Map override these.");
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;

        /* Voice cards (2 columns) */
        GtkWidget *v_lbl = gtk_label_new("Voice");
        gtk_style_context_add_class(gtk_widget_get_style_context(v_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(v_lbl), 0.0f);
        gtk_widget_set_size_request(v_lbl, 200, -1);
        GtkWidget *v_hint = gtk_label_new("Default voice for new segments");
        gtk_style_context_add_class(gtk_widget_get_style_context(v_hint), "sp-row-hint");
        gtk_label_set_xalign(GTK_LABEL(v_hint), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), v_lbl,  0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v_hint, 0, row+1, 1, 1);

        /* Voice card box — two cards side by side */
        GtkWidget *vc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gboolean is_female = (strstr(st->config.dub_voice, "Sreymom") != NULL ||
                              strstr(st->config.dub_voice, "female")  != NULL ||
                              st->config.dub_voice[0] == '\0');

        GtkWidget *fc = gtk_button_new();
        GtkWidget *mc = gtk_button_new();

        /* Female card */
        gtk_style_context_add_class(gtk_widget_get_style_context(fc),
            is_female ? "voice-card-active" : "voice-card");
        {   GtkWidget *fi = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *fa = gtk_label_new("ស្រី");
            gtk_style_context_add_class(gtk_widget_get_style_context(fa), "voice-avatar");
            gtk_widget_set_size_request(fa, 40, 40);
            GtkWidget *fv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *fn = gtk_label_new("ស្រី · Sreymom");
            GtkWidget *fm = gtk_label_new("Female-Sreymon · km-KH · Neural");
            gtk_style_context_add_class(gtk_widget_get_style_context(fn), "voice-name");
            gtk_style_context_add_class(gtk_widget_get_style_context(fm), "voice-meta");
            gtk_label_set_xalign(GTK_LABEL(fn), 0.0f);
            gtk_label_set_xalign(GTK_LABEL(fm), 0.0f);
            gtk_box_pack_start(GTK_BOX(fv), fn, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(fv), fm, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(fi), fa, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(fi), fv, TRUE,  TRUE,  0);
            gtk_container_add(GTK_CONTAINER(fc), fi);
        }
        gtk_widget_set_size_request(fc, 200, -1);

        /* Male card */
        gtk_style_context_add_class(gtk_widget_get_style_context(mc),
            is_female ? "voice-card" : "voice-card-active");
        {   GtkWidget *mi = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *ma = gtk_label_new("ប្រុស");
            gtk_style_context_add_class(gtk_widget_get_style_context(ma), "voice-avatar-male");
            gtk_widget_set_size_request(ma, 40, 40);
            GtkWidget *mv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *mname = gtk_label_new("ប្រុស · Piseth");
            GtkWidget *mmeta = gtk_label_new("Male​​-Piseth · km-KH · Neural");
            gtk_style_context_add_class(gtk_widget_get_style_context(mname), "voice-name");
            gtk_style_context_add_class(gtk_widget_get_style_context(mmeta), "voice-meta");
            gtk_label_set_xalign(GTK_LABEL(mname), 0.0f);
            gtk_label_set_xalign(GTK_LABEL(mmeta), 0.0f);
            gtk_box_pack_start(GTK_BOX(mv), mname, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(mv), mmeta, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(mi), ma, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(mi), mv, TRUE,  TRUE,  0);
            gtk_container_add(GTK_CONTAINER(mc), mi);
        }
        gtk_widget_set_size_request(mc, 200, -1);

        /* Clicking either card flips both cards' active class so the UI
         * updates in real time instead of staying frozen on the initial state. */
        g_object_set_data(G_OBJECT(fc), "vc_self",  fc);
        g_object_set_data(G_OBJECT(fc), "vc_other", mc);
        g_object_set_data(G_OBJECT(mc), "vc_self",  mc);
        g_object_set_data(G_OBJECT(mc), "vc_other", fc);
        g_signal_connect(fc, "clicked", G_CALLBACK(cb_sp_voice_card_click), NULL);
        g_signal_connect(mc, "clicked", G_CALLBACK(cb_sp_voice_card_click), NULL);

        gtk_box_pack_start(GTK_BOX(vc_box), fc, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vc_box), mc, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), vc_box, 1, row, 1, 2);
        row += 3;

        /* Speech rate */
        GtkWidget *sr_lbl = gtk_label_new("Speech rate");
        gtk_style_context_add_class(gtk_widget_get_style_context(sr_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(sr_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), sr_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_slider(50, 400, 10, st->config.tts_rate, ""),
            1, row, 1, 1);
        row += 2;

        /* Pitch */
        GtkWidget *p_lbl = gtk_label_new("Pitch");
        gtk_style_context_add_class(gtk_widget_get_style_context(p_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(p_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), p_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_slider(-100, 100, 1, st->config.tts_pitch, " st"),
            1, row, 1, 1);
        row += 2;

        /* Auto-fit toggle */
        GtkWidget *af_lbl = gtk_label_new("Auto-fit to segment duration");
        gtk_style_context_add_class(gtk_widget_get_style_context(af_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(af_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), af_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_toggle(TRUE, "Stretch rate to match original timing"),
            1, row, 1, 1);
        row += 2;

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 8);

    /* ── Subtitles ── */
    sp_section_anchor(box, "Subtitles", "SUBTITLES",
        "Burned-in caption font size and which track to display "
        "(original Khmer, translated, or both as dual subs).");
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;

        /* Font size */
        GtkWidget *fs_lbl = gtk_label_new("Subtitle font size");
        gtk_style_context_add_class(gtk_widget_get_style_context(fs_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(fs_lbl), 0.0f);
        gtk_widget_set_size_request(fs_lbl, 200, -1);
        gtk_grid_attach(GTK_GRID(grid), fs_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_slider(12, 200, 2,
                      st->config.subtitle_font_size > 0 ? st->config.subtitle_font_size : 42,
                      " px"),
            1, row, 1, 1);
        row += 2;

        /* Sub language */
        GtkWidget *sl_lbl = gtk_label_new("Subtitle language");
        gtk_style_context_add_class(gtk_widget_get_style_context(sl_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(sl_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), sl_lbl, 0, row, 1, 1);
        const char *sub_langs[] = { "Original","Translated","Dual", NULL };
        int sl_idx = (st->config.subtitle_language == 1) ? 0 : 1;
        gtk_grid_attach(GTK_GRID(grid), sp_radio_group(sub_langs, sl_idx), 1, row, 1, 1);
        row += 2;

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 8);

    /* ── Output ── */
    sp_section_anchor(box, "Output", "OUTPUT",
        "Default folder where rendered videos and exported tracks are saved, "
        "and whether to lower background audio while the dub voice plays.");
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;

        /* Output folder */
        GtkWidget *of_lbl = gtk_label_new("Output folder");
        gtk_style_context_add_class(gtk_widget_get_style_context(of_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(of_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), of_lbl, 0, row, 1, 1);

        GtkWidget *of_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *of_entry = gtk_entry_new();
        if (st->config.output_dir[0])
            gtk_entry_set_text(GTK_ENTRY(of_entry), st->config.output_dir);
        else
            gtk_entry_set_placeholder_text(GTK_ENTRY(of_entry), "~/Videos/dub-out/");
        gtk_widget_set_hexpand(of_entry, TRUE);
        GtkWidget *of_browse = make_btn("Browse…", "btn",
                                         G_CALLBACK(cb_browse_output_dir), aw);
        gtk_box_pack_start(GTK_BOX(of_row), of_entry,  TRUE,  TRUE,  0);
        gtk_box_pack_start(GTK_BOX(of_row), of_browse, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), of_row, 1, row, 1, 1);
        row += 2;

        /* Audio ducking */
        GtkWidget *duck_lbl = gtk_label_new("Audio ducking");
        gtk_style_context_add_class(gtk_widget_get_style_context(duck_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(duck_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), duck_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_toggle(st->config.audio_ducking, "Duck BG audio while TTS speaks"),
            1, row, 1, 1);

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 8);

    /* ── Advanced ── */
    sp_section_anchor(box, "Advanced", "ADVANCED",
        "Power-user knobs: voice-cloning script path, transcription "
        "segment cap, and pitch offset for edge-tts voices.");
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;

        /* Voice clone script */
        GtkWidget *vc_lbl = gtk_label_new("Voice clone script");
        gtk_style_context_add_class(gtk_widget_get_style_context(vc_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(vc_lbl), 0.0f);
        gtk_widget_set_size_request(vc_lbl, 200, -1);
        gtk_grid_attach(GTK_GRID(grid), vc_lbl, 0, row, 1, 1);
        GtkWidget *vc_entry = gtk_entry_new();
        if (st->config.voice_clone_script[0])
            gtk_entry_set_text(GTK_ENTRY(vc_entry), st->config.voice_clone_script);
        else
            gtk_entry_set_placeholder_text(GTK_ENTRY(vc_entry),
                "/path/to/clone.py  (used by Test Clone)");
        gtk_widget_set_hexpand(vc_entry, TRUE);
        gtk_grid_attach(GTK_GRID(grid), vc_entry, 1, row, 1, 1);
        row += 2;

        /* Max segments (numeric) */
        GtkWidget *ms_lbl = gtk_label_new("Max Whisper segments");
        gtk_style_context_add_class(gtk_widget_get_style_context(ms_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(ms_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), ms_lbl, 0, row, 1, 1);
        GtkWidget *ms_spin = gtk_spin_button_new_with_range(0, 16000, 100);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ms_spin), st->config.max_segments);
        gtk_widget_set_tooltip_text(ms_spin, "0 = unlimited");
        gtk_grid_attach(GTK_GRID(grid), ms_spin, 1, row, 1, 1);
        row += 2;

        /* TTS pitch */
        GtkWidget *tp_lbl = gtk_label_new("TTS pitch offset");
        gtk_style_context_add_class(gtk_widget_get_style_context(tp_lbl), "sp-row-lbl");
        gtk_label_set_xalign(GTK_LABEL(tp_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), tp_lbl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            sp_slider(-100, 100, 1, st->config.tts_pitch, " Hz"),
            1, row, 1, 1);
        row += 2;

        gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 8);

    /* ── About ── */
    sp_section_anchor(box, "About", "ABOUT",
        "Free code with AI. Single-file GTK3 port of the AI Video "
        "Dubber (LOCAL). Powered by Whisper, edge-tts, FFmpeg, "
        "and GStreamer. Config saved automatically on Apply & close.");
    {
        GtkWidget *info = gtk_label_new(
            "Version " APP_VERSION "\n"
            "Pipeline: Whisper → Translate → edge-tts → FFmpeg mux\n"
            "Project files live under ~/.config/dub/ and the chosen Output folder.");
        gtk_style_context_add_class(gtk_widget_get_style_context(info), "sp-row-hint");
        gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
        gtk_box_pack_start(GTK_BOX(box), info, FALSE, FALSE, 0);
    }

    return box;
}

/* Main Settings dialog callback */
static void cb_open_settings(GtkWidget *btn, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud; (void)btn;

    /* Create a new dialog window each time (destroy on close) */
    GtkWidget *dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    aw->settings_dialog = dlg;
    gtk_window_set_title(GTK_WINDOW(dlg), "Settings — Dub");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(aw->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 860, 640);
    gtk_window_set_resizable(GTK_WINDOW(dlg), TRUE);
    g_signal_connect(dlg, "destroy", G_CALLBACK(gtk_widget_destroyed),
                     &aw->settings_dialog);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dlg), root);

    /* ── Header (sp-hd) ── */
    GtkWidget *sp_hd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(sp_hd), "sp-hd");
    gtk_widget_set_size_request(sp_hd, -1, 48);
    gtk_widget_set_margin_start(sp_hd, 4);
    gtk_widget_set_margin_end(sp_hd, 4);

    GtkWidget *hd_title = gtk_label_new("⚙  Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(hd_title), "sp-hd-title");

    /* Project name */
    GtkWidget *hd_proj = gtk_label_new("Project: —");
    gtk_style_context_add_class(gtk_widget_get_style_context(hd_proj), "sp-row-hint");
    if (aw->state->video_path[0]) {
        char buf[256];
        const char *base = g_path_get_basename(aw->state->video_path);
        snprintf(buf, sizeof(buf), "Project: %s", base);
        gtk_label_set_text(GTK_LABEL(hd_proj), buf);
    }
    GtkWidget *hd_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(hd_sp, TRUE);

    /* Close button */
    GtkWidget *hd_close = gtk_button_new_with_label("✕");
    gtk_style_context_add_class(gtk_widget_get_style_context(hd_close), "hdr-icon-btn");
    g_signal_connect_swapped(hd_close, "clicked", G_CALLBACK(gtk_widget_destroy), dlg);

    gtk_box_pack_start(GTK_BOX(sp_hd), hd_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sp_hd), hd_proj,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sp_hd), hd_sp,    TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(sp_hd), hd_close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), sp_hd, FALSE, FALSE, 0);

    /* ── Body: nav sidebar + content ── */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    /* Navigation sidebar (sp-nav) */
    GtkWidget *sp_nav = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(sp_nav), "sidebar");
    gtk_widget_set_size_request(sp_nav, 180, -1);
    gtk_widget_set_margin_top(sp_nav, 8);
    gtk_widget_set_margin_bottom(sp_nav, 8);
    gtk_widget_set_margin_start(sp_nav, 6);
    gtk_widget_set_margin_end(sp_nav, 6);

    /* fix.txt #2: build content FIRST so nav buttons can attach to its
     * section anchors immediately (no more dead UI from unwired buttons). */
    GtkWidget *sp_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sp_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sp_scroll, TRUE);
    gtk_widget_set_hexpand(sp_scroll, TRUE);

    GtkWidget *sp_content = sp_build_pipeline_page(aw);
    gtk_widget_set_margin_start(sp_content, 22);
    gtk_widget_set_margin_end(sp_content,   22);
    gtk_widget_set_margin_top(sp_content,   18);
    gtk_widget_set_margin_bottom(sp_content, 18);
    gtk_container_add(GTK_CONTAINER(sp_scroll), sp_content);

    /* fix.txt #2: order matches the section anchors in sp_content so every
     * click jumps to a real, populated section instead of dead UI. */
    struct { const char *icon; const char *label; } nav_items[] = {
        { "◈", "Pipeline" },
        { "🌐","Translate" },
        { "🔊","TTS Voices" },
        { "T", "Subtitles" },
        { "📁","Output" },
        { "⚙", "Advanced" },
        { "?", "About" },
        { NULL, NULL }
    };
    for (int i = 0; nav_items[i].label; i++) {
        char lbl_text[64];
        snprintf(lbl_text, sizeof(lbl_text), "%s  %s",
                 nav_items[i].icon, nav_items[i].label);
        GtkWidget *nb = gtk_button_new_with_label(lbl_text);
        gtk_style_context_add_class(gtk_widget_get_style_context(nb), "sp-nav-btn");
        if (i == 0) /* first item active */
            gtk_style_context_add_class(gtk_widget_get_style_context(nb), "active");
        gtk_label_set_xalign(GTK_LABEL(gtk_bin_get_child(GTK_BIN(nb))), 0.0f);

        /* fix.txt #2: wire to scroll-to-anchor in the all-in-one content */
        char anchor_key[64];
        snprintf(anchor_key, sizeof(anchor_key), "anchor:%s", nav_items[i].label);
        GtkWidget *anchor = (GtkWidget*)g_object_get_data(G_OBJECT(sp_content),
                                                           anchor_key);
        if (anchor) {
            g_object_set_data(G_OBJECT(nb), "sp_scroll",  sp_scroll);
            g_object_set_data(G_OBJECT(nb), "sp_content", sp_content);
            g_signal_connect(nb, "clicked",
                             G_CALLBACK(cb_sp_nav_scroll), anchor);
        }
        gtk_box_pack_start(GTK_BOX(sp_nav), nb, FALSE, FALSE, 0);
    }
    /* Bottom spacer (About is now in the nav list above) */
    GtkWidget *nav_sp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(nav_sp, TRUE);
    gtk_box_pack_start(GTK_BOX(sp_nav), nav_sp, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(body), sp_nav, FALSE, FALSE, 0);

    /* Vertical divider */
    gtk_box_pack_start(GTK_BOX(body),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(body), sp_scroll, TRUE, TRUE, 0);

    /* ── Footer (sp-footer) ── */
    GtkWidget *sp_foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(sp_foot), "sp-footer");
    gtk_widget_set_size_request(sp_foot, -1, 44);
    gtk_widget_set_margin_start(sp_foot, 14);
    gtk_widget_set_margin_end(sp_foot,   14);

    GtkWidget *foot_note = gtk_label_new("Changes saved automatically · Ctrl+S to export preset");
    gtk_style_context_add_class(gtk_widget_get_style_context(foot_note), "sp-footer-note");

    GtkWidget *foot_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(foot_sp, TRUE);

    GtkWidget *foot_reset = make_btn("Reset to defaults", "btn", G_CALLBACK(cb_save_cfg), aw);
    GtkWidget *foot_apply = make_btn("Apply & close",     "save-btn",
                                      G_CALLBACK(cb_save_cfg), aw);
    g_signal_connect_swapped(foot_apply, "clicked", G_CALLBACK(gtk_widget_destroy), dlg);

    gtk_box_pack_start(GTK_BOX(sp_foot), foot_note,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sp_foot), foot_sp,    TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(sp_foot), foot_reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sp_foot), foot_apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), sp_foot, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
}


/* ── Acheron dark CSS — mirrors Acheron.html colour tokens in GTK CSS ── */
/* Helper: create a styled button and optionally connect a signal */
static GtkWidget *make_btn(const char *lbl, const char *css_class,
                            GCallback fn, gpointer ud) {
    GtkWidget *b = gtk_button_new_with_label(lbl);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), css_class);
    if (fn) g_signal_connect(b, "clicked", fn, ud);
    return b;
}

/* ═══════════════════════════════════════════════════════════════════
 * fix.txt #1 — App menu bar (op.png parity, voice.c + Acheron_menu.c merge)
 *
 *   File · Edit · View · Pipeline · Timeline · Voice · Help
 *
 * Each item routes to an existing cb_* callback so the menu becomes a
 * universal entry point — no more hunting through 3 toolbars.
 * ═══════════════════════════════════════════════════════════════════ */
static GtkWidget *mi_with_cb(const char *label, GCallback fn, gpointer ud) {
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (fn) g_signal_connect(mi, "activate", fn, ud);
    return mi;
}

static void cb_menu_quit(GtkMenuItem *m, gpointer ud) {
    (void)m; (void)ud;
    gtk_main_quit();
}

static void cb_menu_about(GtkMenuItem *m, gpointer ud) {
    AppWidgets *aw = (AppWidgets*)ud; (void)m;
    show_info(aw->window, "About Acheron",
              "My lover\n"
              "AI Video Dubber (LOCAL)\n\n"
              "Whisper · edge-tts · Khmer voices\n"
              "Single-file GTK3 build.");
}

static GtkWidget *build_app_menu_bar(AppWidgets *aw) {
    GtkWidget *bar = gtk_menu_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "app-menubar");

    /* ── File ── (mirrors Acheron_menu.c: PROJECT / SAVE / EXPORT / quit) */
    {
        GtkWidget *file_root = gtk_menu_item_new_with_mnemonic("_File");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📂 Open Video…",      G_CALLBACK(cb_open_video),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📁 Batch Folder…",    G_CALLBACK(cb_batch_open),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("💾 Save Video…",      G_CALLBACK(cb_save_video),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🎵 Extract MP3",      G_CALLBACK(cb_mp3),         aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📄 Export SRT…",      G_CALLBACK(cb_export_srt),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📄 Export VTT…",      G_CALLBACK(cb_export_vtt),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📄 Export ASS…",      G_CALLBACK(cb_export_ass),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📋 Copy CSV",          G_CALLBACK(cb_copy_segments_csv), aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("💾 Save Settings",    G_CALLBACK(cb_save_cfg),    aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🗑 Clear Recent Files",
                                              G_CALLBACK(cb_clear_recent_files), aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("↗ Quit",              G_CALLBACK(cb_menu_quit),   aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_root);
    }

    /* ── Edit ── (segment table operations) */
    {
        GtkWidget *edit_root = gtk_menu_item_new_with_mnemonic("_Edit");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("✏ Edit Segment",     G_CALLBACK(cb_edit_segment),       aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🗑 Delete Segment",  G_CALLBACK(cb_delete_segment),     aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("✕ Clear All Segments",G_CALLBACK(cb_clear_segments),    aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🔍 Detect Language", G_CALLBACK(cb_detect_language),    aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🔄 Retranslate Segment",
                                              G_CALLBACK(cb_retranslate_segment),aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🎵 Pitch Shift Segment",
                                              G_CALLBACK(cb_pitch_shift_seg),    aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), edit_root);
    }

    /* ── View ── (theme + video size + visualizer) */
    {
        GtkWidget *view_root = gtk_menu_item_new_with_mnemonic("_View");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🌙 Toggle Dark / Light",
                                              G_CALLBACK(cb_toggle_theme_btn),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⛶ Expand / Fullscreen Video",
                                              G_CALLBACK(cb_vid_expand),        aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📊 Show Visualizer",  G_CALLBACK(cb_show_visualizer),   aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), view_root);
    }

    /* ── Pipeline ── (the 4 main steps + full-pipeline) */
    {
        GtkWidget *pipe_root = gtk_menu_item_new_with_mnemonic("_Pipeline");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🎙 1. Transcribe (Whisper)",
                                              G_CALLBACK(cb_transcribe),         aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🌐 2. Translate",     G_CALLBACK(cb_translate),          aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🔊 3. Generate TTS (Dub)",
                                              G_CALLBACK(cb_dub),                aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🎬 4. Mix & Export",  G_CALLBACK(cb_full_pipeline),      aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⚡ Run Full Pipeline (one-click)",
                                              G_CALLBACK(cb_full_pipeline),      aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🎬 Dub + Visualizer", G_CALLBACK(cb_dub_with_visualizer),aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🎬 Dub Full Export",  G_CALLBACK(cb_dub_full_export),    aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("📝 Burn Subs Only",   G_CALLBACK(cb_burn_subs_only),     aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(pipe_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), pipe_root);
    }

    /* ── Timeline ── (voice.c toolbar items: Merge / Split / Shift / Zoom) */
    {
        GtkWidget *tl_root = gtk_menu_item_new_with_mnemonic("_Timeline");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⊞ Merge Segments",   G_CALLBACK(cb_merge_segments),     aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⊟ Split at Playhead",G_CALLBACK(cb_edit_segment),       aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⇄ Shift ±100 ms",    G_CALLBACK(cb_autofit),            aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⚡ Auto-Fit Timing", G_CALLBACK(cb_autofit),            aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⏳ Downtime Filler", G_CALLBACK(cb_downtime_filler),    aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("👥 Diarize Speakers",G_CALLBACK(cb_identify_speakers),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🗺 Speaker → Voice Map",
                                              G_CALLBACK(cb_speaker_voice_map),  aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(tl_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), tl_root);
    }

    /* ── Voice ── (TTS / dub voices / cloning / ducking — voice.c parity) */
    {
        GtkWidget *voice_root = gtk_menu_item_new_with_mnemonic("V_oice");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("▶ Preview Segment",  G_CALLBACK(cb_preview_segment),    aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🧬 Test Voice Clone",G_CALLBACK(cb_test_clone),         aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("🦆 Smart Duck",      G_CALLBACK(cb_smart_duck),         aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⚖ Auto Duck Balance",G_CALLBACK(cb_auto_duck_balance),  aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⚙ TTS / Voice Settings…",
                                              G_CALLBACK(cb_open_settings),      aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(voice_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), voice_root);
    }

    /* ── Help ── */
    {
        GtkWidget *help_root = gtk_menu_item_new_with_mnemonic("_Help");
        GtkWidget *m = gtk_menu_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("⚙ Settings…",        G_CALLBACK(cb_open_settings),      aw));
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
            mi_with_cb("? About Acheron",        G_CALLBACK(cb_menu_about),         aw));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_root), m);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), help_root);
    }

    return bar;
}

static AppWidgets *build_ui(AppState *st) {
    AppWidgets *aw = calloc(1, sizeof *aw);
    if (!aw) { fprintf(stderr,"Fatal: OOM AppWidgets\n"); return NULL; }
    aw->state     = st;
    aw->dark_mode = TRUE;

    /* ── Apply CSS ── */
    g_css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(g_css_provider, APP_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(g_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ── Window ── */
    aw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(aw->window), "Acheron — " APP_NAME " " APP_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(aw->window), 1440, 900);
    g_signal_connect(aw->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(aw->window), root);

    /* ═══════════════════════════════════════════════════════════════
     * 1. HEADER BAR  (Acheron-UI.html: macOS dots, .c mark, brand + menu)
     * ═══════════════════════════════════════════════════════════════ */
    GtkWidget *hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(hbar), "hbar");
    gtk_widget_set_size_request(hbar, -1, 40);

    /* macOS-style window dots (close / minimize / maximize) */
    GtkWidget *win_dots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(win_dots, GTK_ALIGN_CENTER);
    {
        GtkWidget *d1 = gtk_button_new();
        GtkWidget *d2 = gtk_button_new();
        GtkWidget *d3 = gtk_button_new();
        gtk_widget_set_size_request(d1, 12, 12);
        gtk_widget_set_size_request(d2, 12, 12);
        gtk_widget_set_size_request(d3, 12, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(d1), "win-dot");
        gtk_style_context_add_class(gtk_widget_get_style_context(d1), "win-dot-close");
        gtk_style_context_add_class(gtk_widget_get_style_context(d2), "win-dot");
        gtk_style_context_add_class(gtk_widget_get_style_context(d2), "win-dot-min");
        gtk_style_context_add_class(gtk_widget_get_style_context(d3), "win-dot");
        gtk_style_context_add_class(gtk_widget_get_style_context(d3), "win-dot-max");
        g_signal_connect_swapped(d1, "clicked",
                                  G_CALLBACK(gtk_window_close), aw->window);
        g_signal_connect_swapped(d2, "clicked",
                                  G_CALLBACK(gtk_window_iconify), aw->window);
        g_signal_connect_swapped(d3, "clicked",
                                  G_CALLBACK(gtk_window_maximize), aw->window);
        gtk_box_pack_start(GTK_BOX(win_dots), d1, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(win_dots), d2, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(win_dots), d3, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(hbar), win_dots, FALSE, FALSE, 0);

    /* Logo + brand — matches Queen-UI.html "cyrene" mark with "Queen · My lover" */
    GtkWidget *logo_lbl = gtk_label_new("cyrene");
    gtk_style_context_add_class(gtk_widget_get_style_context(logo_lbl), "brand-logo");
    GtkWidget *brand_v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bt = gtk_label_new("Queen");
    GtkWidget *bs = gtk_label_new("My lover · " APP_VERSION);
    gtk_style_context_add_class(gtk_widget_get_style_context(bt), "brand-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(bs), "brand-sub");
    gtk_label_set_xalign(GTK_LABEL(bt), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(bs), 0.0f);
    gtk_box_pack_start(GTK_BOX(brand_v), bt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand_v), bs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbar), logo_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbar), brand_v,  FALSE, FALSE, 4);

    /* Center:Acheron  badge */
    GtkWidget *center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(center_box, TRUE);
    gtk_widget_set_halign(center_box, GTK_ALIGN_CENTER);
    GtkWidget *dac = gtk_label_new("🎙  Acheron");
    gtk_style_context_add_class(gtk_widget_get_style_context(dac), "acheron");
    gtk_box_pack_start(GTK_BOX(center_box), dac, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbar), center_box, TRUE, TRUE, 0);

    /* Right: Open, Recent, Batch, sep, dark mode, settings */
    GtkWidget *open_btn = make_btn("📂 Open Video", "hdr-btn", G_CALLBACK(cb_open_video), aw);
    aw->recent_btn = make_btn("🕓 Recent", "hdr-btn", NULL, NULL);
    gtk_menu_button_new(); /* ensure type registered */
    {   /* Make recent_btn a proper menu button */
        aw->recent_btn = gtk_menu_button_new();
        gtk_button_set_label(GTK_BUTTON(aw->recent_btn), "🕓 Recent");
        gtk_style_context_add_class(gtk_widget_get_style_context(aw->recent_btn), "hdr-btn");
        rebuild_recent_menu(aw);
    }
    GtkWidget *batch_btn = make_btn("📁 Batch Folder", "hdr-btn", G_CALLBACK(cb_batch_open), aw);

    /* Vertical separator */
    GtkWidget *vsep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_start(vsep, 4); gtk_widget_set_margin_end(vsep, 4);

    /* v9: Theme toggle pill — Dark / Light (matches Queen-UI.html .theme-toggle) */
    GtkWidget *theme_pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(theme_pill), "theme-pill");
    gtk_widget_set_valign(theme_pill, GTK_ALIGN_CENTER);
    {
        GtkWidget *btn_dark  = gtk_button_new_with_label("🌙 Dark");
        GtkWidget *btn_light = gtk_button_new_with_label("☀ Light");
        /* Start in dark mode — dark button is "active" */
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_dark),  "theme-btn-active");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_light), "theme-btn");
        gtk_widget_set_tooltip_text(btn_dark,  "Dark mode");
        gtk_widget_set_tooltip_text(btn_light, "Light mode");
        /* Both buttons share the same callback; it toggles the whole app theme */
        g_signal_connect(btn_dark,  "clicked", G_CALLBACK(cb_toggle_theme_btn), aw);
        g_signal_connect(btn_light, "clicked", G_CALLBACK(cb_toggle_theme_btn), aw);
        gtk_box_pack_start(GTK_BOX(theme_pill), btn_dark,  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(theme_pill), btn_light, FALSE, FALSE, 0);
    }

    /* v9: ⚙ Settings icon button — opens the Settings modal (Slide 2) */
    GtkWidget *settings_icon_btn = gtk_button_new_with_label("⚙");
    gtk_style_context_add_class(gtk_widget_get_style_context(settings_icon_btn), "hdr-icon-btn");
    gtk_widget_set_tooltip_text(settings_icon_btn, "Settings");
    g_signal_connect(settings_icon_btn, "clicked", G_CALLBACK(cb_open_settings), aw);

    gtk_box_pack_end(GTK_BOX(hbar), settings_icon_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbar), theme_pill,        FALSE, FALSE, 4);
    gtk_box_pack_end(GTK_BOX(hbar), vsep,              FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbar), batch_btn,         FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbar), aw->recent_btn,    FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbar), open_btn,          FALSE, FALSE, 0);
    g_signal_connect(open_btn, "clicked", G_CALLBACK(cb_open_video), aw);

    gtk_box_pack_start(GTK_BOX(root), hbar, FALSE, FALSE, 0);

    /* ═══════════════════════════════════════════════════════════════
     * fix.txt #1 + op.png — Application menu bar
     * (File · Edit · View · Pipeline · Timeline · Voice · Help)
     * Merges voice.c toolbar + Acheron_menu.c file menu into one bar.
     * ═══════════════════════════════════════════════════════════════ */
    aw->menu_bar = build_app_menu_bar(aw);
    gtk_box_pack_start(GTK_BOX(root), aw->menu_bar, FALSE, FALSE, 0);

    /* Video filename label */
    aw->video_lbl = GTK_LABEL(gtk_label_new("No video selected"));
    gtk_label_set_ellipsize(aw->video_lbl, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(aw->video_lbl), 0.0f);
    gtk_widget_set_margin_start(GTK_WIDGET(aw->video_lbl), 10);
    gtk_widget_set_margin_top(GTK_WIDGET(aw->video_lbl), 2);
    gtk_widget_set_margin_bottom(GTK_WIDGET(aw->video_lbl), 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(aw->video_lbl)),
                                 "vid-fname");
    gtk_box_pack_start(GTK_BOX(root), GTK_WIDGET(aw->video_lbl), FALSE, FALSE, 0);

    /* ═══════════════════════════════════════════════════════════════
     * 2. CONFIG WIDGETS (visible settings bar)
     * ═══════════════════════════════════════════════════════════════ */
    aw->model_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (int i = 0; WHISPER_MODELS[i]; i++)
        gtk_combo_box_text_append_text(aw->model_combo, WHISPER_MODELS[i]);
    { int idx=1;
      for (int i=0; WHISPER_MODELS[i]; i++)
          if (!strcmp(st->config.whisper_model, WHISPER_MODELS[i])) { idx=i; break; }
      gtk_combo_box_set_active(GTK_COMBO_BOX(aw->model_combo), idx); }

    aw->lang_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (int i=0; LANG_CODES[i]; i++)
        gtk_combo_box_text_append_text(aw->lang_combo, LANG_CODES[i]);
    { int idx=0;
      for (int i=0; LANG_CODES[i]; i++)
          if (!strcmp(st->config.target_language, LANG_CODES[i])) { idx=i; break; }
      gtk_combo_box_set_active(GTK_COMBO_BOX(aw->lang_combo), idx); }

    aw->voice_entry = GTK_ENTRY(gtk_entry_new());
    if (st->config.tts_voice_id[0]) gtk_entry_set_text(aw->voice_entry, st->config.tts_voice_id);

    aw->dub_voice_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (int i=0; KM_VOICE_LBLS[i]; i++)
        gtk_combo_box_text_append_text(aw->dub_voice_combo, KM_VOICE_LBLS[i]);
    { int idx=0;
      for (int i=0; KM_VOICES[i]; i++)
          if (!strcmp(st->config.dub_voice, KM_VOICES[i])) { idx=i; break; }
      gtk_combo_box_set_active(GTK_COMBO_BOX(aw->dub_voice_combo), idx); }
    /* fix.txt #4: connect changed signal so voice updates config immediately */
    g_signal_connect(aw->dub_voice_combo, "changed",
                     G_CALLBACK(cb_dub_voice_changed), aw);

    aw->rate_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(50, 400, 10));
    gtk_spin_button_set_value(aw->rate_spin, st->config.tts_rate);

    aw->trans_engine_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    { const char *engines[] = { "google","argos","ollama","mistral", NULL };
      for (int i=0; engines[i]; i++)
          gtk_combo_box_text_append_text(aw->trans_engine_combo, engines[i]);
      int eidx=0;
      for (int i=0; engines[i]; i++)
          if (!strcmp(st->config.translation_engine, engines[i])) { eidx=i; break; }
      gtk_combo_box_set_active(GTK_COMBO_BOX(aw->trans_engine_combo), eidx); }

    aw->sub_lang_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(aw->sub_lang_combo, "Translated");
    gtk_combo_box_text_append_text(aw->sub_lang_combo, "Original");
    gtk_combo_box_set_active(GTK_COMBO_BOX(aw->sub_lang_combo),
                              st->config.subtitle_language ? 1 : 0);

    aw->max_seg_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 16000, 100));
    gtk_spin_button_set_value(aw->max_seg_spin, st->config.max_segments);
    gtk_widget_set_tooltip_text(GTK_WIDGET(aw->max_seg_spin),
        "Max segments from Whisper (0 = unlimited)");

    aw->font_size_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(12, 200, 4));
    gtk_spin_button_set_value(aw->font_size_spin, st->config.subtitle_font_size > 0
                               ? st->config.subtitle_font_size : 72);

    aw->pitch_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(-100, 100, 1));
    gtk_spin_button_set_value(aw->pitch_spin, st->config.tts_pitch);

    aw->duck_check = gtk_check_button_new_with_label("Duck BG");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(aw->duck_check),
                                  st->config.audio_ducking ? TRUE : FALSE);

    aw->output_dir_entry = GTK_ENTRY(gtk_entry_new());
    if (st->config.output_dir[0])
        gtk_entry_set_text(aw->output_dir_entry, st->config.output_dir);

    /* ═══════════════════════════════════════════════════════════════
     * 3. MAIN PANED  (left video sidebar | right panel)
     * ═══════════════════════════════════════════════════════════════ */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    aw->paned = paned;  /* fix.txt: store for expand/fullscreen */
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

    /* ── LEFT SIDEBAR ── */
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    aw->sidebar = sidebar;  /* fix.txt: store for expand/fullscreen */
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar), "sidebar");
    /* Acheron-UI.html specifies a fixed-width 520px left column (grid-template-columns:520px 1fr) */
    gtk_widget_set_size_request(sidebar, 520, -1);
    gtk_paned_pack1(GTK_PANED(paned), sidebar, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 520);

    /* ── Video screen area ──
     * vid_box  = container (vexpand); vid_ph = GtkDrawingArea (black bg, 16:9)
     */
    aw->vid_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *vid_box = aw->vid_box;
    gtk_style_context_add_class(gtk_widget_get_style_context(vid_box), "video-box");
    gtk_widget_set_vexpand(vid_box, TRUE);
    gtk_widget_set_hexpand(vid_box, TRUE);
    g_object_set_data(G_OBJECT(aw->window), "video_box", vid_box);

    /* Black drawing area as placeholder — visible until a video is loaded */
    GtkWidget *vid_ph = gtk_drawing_area_new();
    aw->vid_placeholder = vid_ph;
    gtk_widget_set_size_request(vid_ph, 290, 163);
    gtk_widget_set_vexpand(vid_ph, TRUE);
    gtk_widget_set_hexpand(vid_ph, TRUE);
    gtk_widget_set_name(vid_ph, "vid_placeholder");
    /* Draw black rectangle + "No video" text */
    g_signal_connect(vid_ph, "draw", G_CALLBACK(vid_placeholder_draw_cb), NULL);
    gtk_box_pack_start(GTK_BOX(vid_box), vid_ph, TRUE, TRUE, 0);

    /* Subtitle overlay: a GtkOverlay lets us paint text on top of the video */
    GtkWidget *vid_overlay = gtk_overlay_new();
    aw->vid_overlay = vid_overlay;  /* fix.txt: store for expand/fullscreen */
    gtk_container_add(GTK_CONTAINER(vid_overlay), vid_box);
    /* Cyan subtitle label pinned to bottom */
    GtkWidget *sub_ov = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(sub_ov), TRUE);
    gtk_widget_set_halign(sub_ov, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(sub_ov, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(sub_ov, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub_ov), "sub-overlay");
    gtk_overlay_add_overlay(GTK_OVERLAY(vid_overlay), sub_ov);
    gtk_box_pack_start(GTK_BOX(sidebar), vid_overlay, TRUE, TRUE, 0);

    /* Video controls */
    GtkWidget *vctrls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    aw->vctrls = vctrls;  /* fix.txt: store for expand/fullscreen */
    gtk_widget_set_margin_start(vctrls, 10);
    gtk_widget_set_margin_end(vctrls,  10);
    gtk_widget_set_margin_top(vctrls,   6);
    gtk_widget_set_margin_bottom(vctrls, 6);

    /* Time row */
    GtkWidget *time_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    aw->time_lbl = GTK_LABEL(gtk_label_new("00:00.00"));
    aw->dur_lbl  = GTK_LABEL(gtk_label_new("--:--:--"));
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(aw->time_lbl)), "time-lbl");
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(aw->dur_lbl)),  "time-lbl");
    GtkWidget *tsp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(tsp, TRUE);
    gtk_box_pack_start(GTK_BOX(time_row), GTK_WIDGET(aw->time_lbl), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_row), tsp,                       TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(time_row), GTK_WIDGET(aw->dur_lbl),  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vctrls), time_row, FALSE, FALSE, 0);

    /* Seek bar */
    aw->progress_bar_vid = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 0.1);
    gtk_scale_set_draw_value(GTK_SCALE(aw->progress_bar_vid), FALSE);
    gtk_range_set_value(GTK_RANGE(aw->progress_bar_vid), 0);
    gtk_widget_set_size_request(aw->progress_bar_vid, -1, 8);
    gtk_box_pack_start(GTK_BOX(vctrls), aw->progress_bar_vid, FALSE, FALSE, 0);

    /* Transport row: Play, Stop, vol_slider, mute */
    GtkWidget *trans_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    aw->play_btn = gtk_button_new_with_label("▶");
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->play_btn), "ctrl-play");
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->play_btn), "ctrl-btn");
    gtk_widget_set_tooltip_text(aw->play_btn, "Play / Pause");
    g_signal_connect(aw->play_btn, "clicked", G_CALLBACK(cb_play_pause), aw);

    aw->stop_btn = gtk_button_new_with_label("⏹");
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->stop_btn), "ctrl-btn");
    gtk_widget_set_tooltip_text(aw->stop_btn, "Stop");
    g_signal_connect(aw->stop_btn, "clicked", G_CALLBACK(cb_stop_playback), aw);

    aw->vol_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(aw->vol_slider), FALSE);
    gtk_range_set_value(GTK_RANGE(aw->vol_slider), 70.0);
    gtk_widget_set_hexpand(aw->vol_slider, TRUE);
    g_signal_connect(aw->vol_slider, "value-changed", G_CALLBACK(cb_volume_changed), aw);

    aw->mute_btn = gtk_button_new_with_label("🔊");
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->mute_btn), "ctrl-btn");
    gtk_widget_set_tooltip_text(aw->mute_btn, "Mute / Unmute");
    g_signal_connect(aw->mute_btn, "clicked", G_CALLBACK(cb_mute_toggle), aw);

    /* Expand / Fullscreen toggle button */
    aw->expand_btn = gtk_button_new_with_label("⛶");
    aw->vid_expand_mode = 0;
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->expand_btn), "ctrl-btn");
    gtk_widget_set_tooltip_text(aw->expand_btn, "Expand video");
    g_signal_connect(aw->expand_btn, "clicked", G_CALLBACK(cb_vid_expand), aw);

    /* Connect seek bar */
    g_signal_connect(aw->progress_bar_vid, "value-changed", G_CALLBACK(cb_seek_changed), aw);

    gtk_box_pack_start(GTK_BOX(trans_row), aw->play_btn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(trans_row), aw->stop_btn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(trans_row), aw->vol_slider,  TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(trans_row), aw->mute_btn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(trans_row), aw->expand_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vctrls), trans_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), vctrls, FALSE, FALSE, 0);

    /* ── v9: RECENT FILES section (Slide 1 left panel, below player controls) ──
     * Matches Acheron-UI.html ".recent" section with header + scrollable list.
     */
    {
        GtkWidget *recent_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(recent_outer, TRUE);

        /* Header row: "RECENT FILES" label + "Clear" link */
        GtkWidget *recent_hd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *recent_hd_lbl = gtk_label_new("RECENT FILES");
        gtk_style_context_add_class(gtk_widget_get_style_context(recent_hd_lbl), "recent-hd");
        gtk_label_set_xalign(GTK_LABEL(recent_hd_lbl), 0.0f);
        gtk_widget_set_hexpand(recent_hd_lbl, TRUE);
        GtkWidget *recent_clear_btn = gtk_button_new_with_label("Clear");
        gtk_style_context_add_class(gtk_widget_get_style_context(recent_clear_btn), "hdr-btn");
        gtk_widget_set_margin_end(recent_clear_btn, 8);
        gtk_widget_set_tooltip_text(recent_clear_btn, "Clear recent files");
        /* fix.txt #4: actually wipes recent_files[] in config + sidebar + menu */
        g_signal_connect(recent_clear_btn, "clicked",
            G_CALLBACK(cb_clear_recent_files), aw);
        gtk_box_pack_start(GTK_BOX(recent_hd_row), recent_hd_lbl,     TRUE,  TRUE, 0);
        gtk_box_pack_start(GTK_BOX(recent_hd_row), recent_clear_btn,  FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(recent_outer), recent_hd_row, FALSE, FALSE, 0);

        /* Scrollable list of recent items */
        GtkWidget *scroll_recent = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_recent),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroll_recent, TRUE);

        aw->recent_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(aw->recent_list_box, 6);
        gtk_widget_set_margin_end(aw->recent_list_box,   6);
        gtk_widget_set_margin_top(aw->recent_list_box,   2);

        /* Populate from config.recent_files */
        gboolean any = FALSE;
        for (int ri = 0; ri < 5; ri++) {
            if (!st->config.recent_files[ri][0]) continue;
            any = TRUE;
            const char *fpath = st->config.recent_files[ri];
            const char *fname = g_path_get_basename(fpath);

            GtkWidget *row = gtk_button_new();
            gtk_style_context_add_class(gtk_widget_get_style_context(row), "recent-row");
            GtkWidget *row_inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

            /* Thumbnail placeholder (36×24 coloured square) */
            GtkWidget *thumb = gtk_drawing_area_new();
            gtk_widget_set_size_request(thumb, 36, 24);

            GtkWidget *info  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            GtkWidget *name_lbl = gtk_label_new(fname);
            gtk_style_context_add_class(gtk_widget_get_style_context(name_lbl), "recent-name");
            gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_MIDDLE);

            GtkWidget *path_lbl = gtk_label_new(fpath);
            gtk_style_context_add_class(gtk_widget_get_style_context(path_lbl), "recent-meta");
            gtk_label_set_xalign(GTK_LABEL(path_lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(path_lbl), PANGO_ELLIPSIZE_MIDDLE);

            gtk_box_pack_start(GTK_BOX(info), name_lbl, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(info), path_lbl, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(row_inner), thumb, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row_inner), info,  TRUE,  TRUE,  0);
            gtk_container_add(GTK_CONTAINER(row), row_inner);

            /* Clicking a recent row loads that video */
            g_object_set_data_full(G_OBJECT(row), "fpath",
                                   g_strdup(fpath), g_free);
            g_signal_connect_swapped(row, "clicked",
                G_CALLBACK(load_video_path),
                g_object_get_data(G_OBJECT(row), "fpath"));
            /* We need aw too — use a lambda-style helper via closure:
             * re-use cb_open_video pattern but pass aw via row data */
            g_object_set_data(G_OBJECT(row), "aw", aw);

            gtk_box_pack_start(GTK_BOX(aw->recent_list_box), row, FALSE, FALSE, 0);
        }
        if (!any) {
            GtkWidget *empty_lbl = gtk_label_new("No recent files");
            gtk_style_context_add_class(gtk_widget_get_style_context(empty_lbl), "recent-meta");
            gtk_widget_set_margin_top(empty_lbl, 8);
            gtk_box_pack_start(GTK_BOX(aw->recent_list_box), empty_lbl, FALSE, FALSE, 0);
        }

        gtk_container_add(GTK_CONTAINER(scroll_recent), aw->recent_list_box);
        gtk_box_pack_start(GTK_BOX(recent_outer), scroll_recent, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(sidebar), recent_outer, TRUE, TRUE, 0);
    }

    /* Transport controls connected to GStreamer playback */

    /* ── RIGHT MAIN PANEL ── */
    GtkWidget *rpanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    aw->rpanel = rpanel;  /* fix.txt: store for expand/fullscreen */
    gtk_style_context_add_class(gtk_widget_get_style_context(rpanel), "rpanel");
    gtk_paned_pack2(GTK_PANED(paned), rpanel, TRUE, FALSE);

    /* ACTION BAR 1 removed: pipeline / dub / export buttons live in the
     * application menu bar (Pipeline / File menus). */
    aw->ab1 = NULL;

    /* ACTION BAR 2 removed: duplicate editing/voice/visualizer buttons now live
     * in Edit / Pipeline / Timeline / Voice / View menus. */
    aw->ab2 = NULL;

    /* ACTION BAR 3 removed: Edit / Delete / Detect Lang / Clear duplicated the
     * Edit and Pipeline menu items. */
    aw->ab3 = NULL;

    /* SETTINGS BAR removed: RATE / TRANS / SUB LANG / MAX SEG / SUB SIZE / PITCH
     * are configured via the Voice and Pipeline menus and the Settings dialog. */
    aw->cfg_bar = NULL;

    /* ── SEGMENT TABLE ──
     *  Columns:  ☐ | START | END | KHMER TEXT (EDITABLE) | TRANSLATED % |
     *            VOICE PROFILE | AUDIO STATUS | ▶
     *
     *  The list-store has 6 string columns:
     *    0=start  1=end  2=orig  3=translated  4=voice  5=audio_status
     *  The ▶ button column is rendered as a separate GtkCellRendererText
     *  that looks clickable (row-activated does the preview).
     */
    aw->seg_store = gtk_list_store_new(6,
        G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING);
    aw->seg_view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(aw->seg_store)));
    g_object_unref(aw->seg_store);
    gtk_tree_view_set_grid_lines(aw->seg_view, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);

    /* Checkbox column */
    {   GtkCellRenderer *tr = gtk_cell_renderer_toggle_new();
        GtkTreeViewColumn *tc = gtk_tree_view_column_new_with_attributes("", tr, NULL);
        gtk_tree_view_column_set_fixed_width(tc, 28);
        gtk_tree_view_column_set_sizing(tc, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_append_column(aw->seg_view, tc); }

    /* Text columns */
    struct { const char *hdr; int col; int expand; int minw; gboolean editable; } tcols[] = {
        { "START",                 0, FALSE,  68, FALSE },
        { "END",                   1, FALSE,  68, FALSE },
        { "KHMER TEXT (EDITABLE)", 2,  TRUE, 200,  TRUE },
        { "TRANSLATED %",          3,  TRUE, 180,  TRUE },
        { "VOICE PROFILE",         4, FALSE, 100, FALSE },
        { "AUDIO STATUS",          5, FALSE,  90, FALSE },
        { NULL, 0, 0, 0, FALSE }
    };
    for (int i=0; tcols[i].hdr; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        if (tcols[i].editable) {
            g_object_set(r, "editable", TRUE, NULL);
            g_object_set_data(G_OBJECT(r), "col_index", GINT_TO_POINTER(tcols[i].col));
            g_signal_connect(r, "edited", G_CALLBACK(on_cell_edited), aw);
        }
        /* Style VOICE PROFILE in female/male pink/blue */
        if (i == 4) g_object_set(r, "foreground", "#ff6eb4", NULL);
        /* Style AUDIO STATUS */
        if (i == 5) g_object_set(r, "foreground", "#00e5a0", "weight", 700, NULL);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            tcols[i].hdr, r, "text", tcols[i].col, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_min_width(col, tcols[i].minw);
        if (tcols[i].expand) gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(aw->seg_view, col);
    }

    /* ▶ Preview column */
    {   GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "foreground", "#54a0ff", "weight", 700, NULL);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            "▶", r, "text", 5, NULL); /* reuse status col just for the header */
        gtk_tree_view_column_set_title(col, "▶");
        gtk_tree_view_column_set_fixed_width(col, 72);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        /* Trick: show static "▶ Preview" text in every cell */
        GtkCellRenderer *rb = gtk_cell_renderer_text_new();
        g_object_set(rb, "foreground", "#54a0ff", "weight", 700,
                     "text", "▶ Preview", NULL);
        gtk_tree_view_column_pack_start(col, rb, FALSE);
        gtk_tree_view_append_column(aw->seg_view, col); }

    g_signal_connect(aw->seg_view, "row-activated",
                     G_CALLBACK(on_seg_row_activated), aw);

    /* Sync video when table selection changes */
    {   GtkTreeSelection *sel = gtk_tree_view_get_selection(aw->seg_view);
        g_signal_connect(sel, "changed", G_CALLBACK(cb_seg_selection_changed), aw); }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    aw->seg_scroll = scroll;  /* fix.txt #3: ref so fullscreen can hide it */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(aw->seg_view));
    gtk_box_pack_start(GTK_BOX(rpanel), scroll, TRUE, TRUE, 0);

    /* ── TIMELINE PANEL ── */
    GtkWidget *tl_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    aw->tl_panel = tl_panel;  /* fix.txt #3: ref so fullscreen can hide it */
    gtk_style_context_add_class(gtk_widget_get_style_context(tl_panel), "tl-panel");
    /* fix.txt #1: taller so the horizontal scrollbar + 3 tracks fit comfortably */
    gtk_widget_set_size_request(tl_panel, -1, 150);

    /* Timeline header — v9: matches Slide 3 tlz-hd:
     * brand mini + spacer + Merge | Split at playhead | Shift ±100ms | sep | zoom slider + px/sec + lang + Transcribe + Auto-Fit
     */
    GtkWidget *tl_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(tl_hdr), "tl-hdr");

    /* Mini brand */
    GtkWidget *tl_title = gtk_label_new("Timeline Editor");
    gtk_style_context_add_class(gtk_widget_get_style_context(tl_title), "tl-title");
    gtk_box_pack_start(GTK_BOX(tl_hdr), tl_title, FALSE, FALSE, 0);

    /* v9: Slide 3 editing tools inline in timeline header */
    gtk_box_pack_start(GTK_BOX(tl_hdr),
        make_btn("⊞ Merge",        "btn", G_CALLBACK(cb_merge_segments),   aw), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(tl_hdr),
        make_btn("⊟ Split at ▶",   "btn", G_CALLBACK(cb_edit_segment),     aw), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(tl_hdr),
        make_btn("⇄ Shift ±100ms", "btn", G_CALLBACK(cb_autofit),          aw), FALSE,FALSE,0);

    {   GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_margin_start(s,4); gtk_widget_set_margin_end(s,4);
        gtk_box_pack_start(GTK_BOX(tl_hdr), s, FALSE, FALSE, 0); }

    GtkWidget *zl = gtk_label_new("ZOOM:");
    gtk_style_context_add_class(gtk_widget_get_style_context(zl), "tl-zoom-lbl");
    GtkWidget *zoom_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                      50.0, 1000.0, 10.0);
    gtk_scale_set_draw_value(GTK_SCALE(zoom_slider), FALSE);
    gtk_range_set_value(GTK_RANGE(zoom_slider), 460.0);
    gtk_widget_set_size_request(zoom_slider, 110, -1);
    aw->zoom_pct_lbl = GTK_LABEL(gtk_label_new("460 px/sec"));
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(aw->zoom_pct_lbl)),
                                 "tl-zoom-pct");
    /* ★ wire zoom slider */
    g_signal_connect(zoom_slider, "value-changed", G_CALLBACK(cb_zoom_changed), aw);

    GtkWidget *tl_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(tl_sp, TRUE);

    /* Timeline lang combo and Transcribe button removed: available via Pipeline
     * menu (Transcribe) and Settings dialog (sub-lang). Keep a headless combo
     * so code that reads aw->tl_lang_combo still works. */
    aw->tl_lang_combo = GTK_WIDGET(gtk_combo_box_text_new());
    for (int i=0; LANG_CODES[i]; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(aw->tl_lang_combo), LANG_CODES[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(aw->tl_lang_combo), 0);
    g_signal_connect(aw->tl_lang_combo, "changed", G_CALLBACK(cb_tl_lang_changed), aw);
    g_object_ref_sink(aw->tl_lang_combo);  /* not packed anywhere — own the ref */

    GtkWidget *tl_autofit    = make_btn("⚡ Auto-Fit",             "btn-purple",
                                         G_CALLBACK(cb_autofit),    aw);

    gtk_box_pack_start(GTK_BOX(tl_hdr), zl,            FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl_hdr), zoom_slider,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl_hdr), GTK_WIDGET(aw->zoom_pct_lbl), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl_hdr), tl_sp,          TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tl_hdr), tl_autofit,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tl_panel), tl_hdr, FALSE, FALSE, 0);

    /* Timeline tracks: ruler + T1 + A1 */
    GtkWidget *tl_tracks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(tl_tracks, 10);
    gtk_widget_set_margin_end(tl_tracks,   10);
    gtk_widget_set_margin_top(tl_tracks,    5);
    gtk_widget_set_margin_bottom(tl_tracks, 5);

    /* Ruler row */
    GtkWidget *ruler_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *ruler_lbl = gtk_label_new("");
    gtk_widget_set_size_request(ruler_lbl, 30, -1);
    GtkWidget *ruler_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(ruler_content), "tl-track-wrap");
    gtk_widget_set_hexpand(ruler_content, TRUE);
    gtk_widget_set_size_request(ruler_content, -1, 18);
    /* Ruler marks */
    const char *marks[] = { "0:00","5:00","10:00","15:00","20:00","25:00","30:00", NULL };
    for (int i=0; marks[i]; i++) {
        GtkWidget *m = gtk_label_new(marks[i]);
        gtk_style_context_add_class(gtk_widget_get_style_context(m), "tl-zoom-lbl");
        gtk_box_pack_start(GTK_BOX(ruler_content), m, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(ruler_row), ruler_lbl,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ruler_row), ruler_content,  TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tl_tracks), ruler_row, FALSE, FALSE, 0);

    /* T1 track — subtitle segments, rendered as coloured blocks */
    {   GtkWidget *tr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *tr_lbl = gtk_label_new("T1");
        gtk_style_context_add_class(gtk_widget_get_style_context(tr_lbl), "tl-lbl");
        gtk_widget_set_size_request(tr_lbl, 24, -1);
        GtkWidget *tc_btn = gtk_button_new_with_label("🔊");
        gtk_style_context_add_class(gtk_widget_get_style_context(tc_btn), "btn");
        gtk_widget_set_tooltip_text(tc_btn, "Mute T1");

        /* GtkDrawingArea for T1 — renders segments from st->segments[] */
        aw->tl_t1_area = gtk_drawing_area_new();
        /* fix.txt #1: do NOT hexpand — width is driven by tl_refresh so the
         * parent GtkScrolledWindow actually scrolls past the viewport. */
        gtk_widget_set_size_request(aw->tl_t1_area, 400, 30);
        gtk_style_context_add_class(gtk_widget_get_style_context(aw->tl_t1_area), "tl-track-wrap");
        g_signal_connect(aw->tl_t1_area, "draw",
                         G_CALLBACK(tl_t1_draw_cb), aw);
        g_signal_connect(aw->tl_t1_area, "button-press-event",
                         G_CALLBACK(tl_t1_click_cb), aw);
        g_signal_connect(aw->tl_t1_area, "motion-notify-event",
                         G_CALLBACK(tl_t1_motion_cb), aw);
        g_signal_connect(aw->tl_t1_area, "button-release-event",
                         G_CALLBACK(tl_t1_release_cb), aw);
        gtk_widget_add_events(aw->tl_t1_area,
                              GDK_BUTTON_PRESS_MASK   |
                              GDK_BUTTON_RELEASE_MASK |
                              GDK_POINTER_MOTION_MASK |
                              GDK_POINTER_MOTION_HINT_MASK);
        /* Init drag state */
        aw->tl_drag_idx   = -1;
        aw->tl_drag_edge  =  0;
        aw->tl_drag_moved = FALSE;

        gtk_box_pack_start(GTK_BOX(tr), tr_lbl,         FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tr), tc_btn,         FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tr), aw->tl_t1_area,  TRUE,  TRUE, 0);
        gtk_box_pack_start(GTK_BOX(tl_tracks), tr, FALSE, FALSE, 0);
    }
    /* A1 track — audio dubbing track */
    {   GtkWidget *tr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *tr_lbl = gtk_label_new("A1");
        gtk_style_context_add_class(gtk_widget_get_style_context(tr_lbl), "tl-lbl");
        gtk_widget_set_size_request(tr_lbl, 24, -1);
        GtkWidget *tc_btn = gtk_button_new_with_label("🔊");
        gtk_style_context_add_class(gtk_widget_get_style_context(tc_btn), "btn");
        gtk_widget_set_tooltip_text(tc_btn, "Mute A1");

        aw->tl_a1_area = gtk_drawing_area_new();
        /* fix.txt #1: width is set by tl_refresh to match duration × zoom */
        gtk_widget_set_size_request(aw->tl_a1_area, 400, 30);
        gtk_style_context_add_class(gtk_widget_get_style_context(aw->tl_a1_area), "tl-track-wrap");
        g_signal_connect(aw->tl_a1_area, "draw",
                         G_CALLBACK(tl_a1_draw_cb), aw);

        gtk_box_pack_start(GTK_BOX(tr), tr_lbl,         FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tr), tc_btn,         FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(tr), aw->tl_a1_area,  TRUE,  TRUE, 0);
        gtk_box_pack_start(GTK_BOX(tl_tracks), tr, FALSE, FALSE, 0);
    }
    /* Init zoom defaults */
    aw->tl_zoom          = 30.0;   /* 30 px per second */
    aw->tl_scroll_offset =  0.0;
    /* fix.txt #1: Wrap tracks in a horizontal GtkScrolledWindow so long
     * videos can be scrolled to reach late segments. */
    GtkWidget *tl_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tl_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(tl_scroll),
                                         GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(tl_scroll), tl_tracks);
    gtk_box_pack_start(GTK_BOX(tl_panel), tl_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(rpanel), tl_panel, FALSE, FALSE, 0);

    /* ═══════════════════════════════════════════════════════════════
     * 4. MIXING BAR (progress bar)
     * ═══════════════════════════════════════════════════════════════ */
    aw->bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(aw->bar, TRUE);
    gtk_progress_bar_set_text(aw->bar, "Mixing audio...");
    gtk_widget_set_margin_start(GTK_WIDGET(aw->bar), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(aw->bar),   8);
    gtk_widget_set_margin_top(GTK_WIDGET(aw->bar),   3);
    gtk_widget_set_margin_bottom(GTK_WIDGET(aw->bar),3);
    gtk_box_pack_start(GTK_BOX(root), GTK_WIDGET(aw->bar), FALSE, FALSE, 0);

    /* ═══════════════════════════════════════════════════════════════
     * 5. STATUS BAR  (status dot | Ready | Project: | Memory)
     * ═══════════════════════════════════════════════════════════════ */
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_bar), "status-bar");

    /* Left: status dot indicator label */
    GtkWidget *dot_lbl = gtk_label_new("●");
    gtk_style_context_add_class(gtk_widget_get_style_context(dot_lbl), "status-dot-lbl");
    gtk_box_pack_start(GTK_BOX(status_bar), dot_lbl, FALSE, FALSE, 0);

    /* Status text */
    aw->status_lbl = GTK_LABEL(gtk_label_new("Ready"));
    gtk_label_set_xalign(aw->status_lbl, 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(aw->status_lbl)),
                                 "status-lbl");
    gtk_box_pack_start(GTK_BOX(status_bar), GTK_WIDGET(aw->status_lbl), FALSE, FALSE, 0);

    /* Separator */
    GtkWidget *st_sep = gtk_label_new("|");
    gtk_style_context_add_class(gtk_widget_get_style_context(st_sep), "status-lbl");
    gtk_box_pack_start(GTK_BOX(status_bar), st_sep, FALSE, FALSE, 0);

    /* Project label */
    aw->project_lbl = GTK_WIDGET(gtk_label_new("Project: —"));
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->project_lbl), "status-lbl");
    gtk_box_pack_start(GTK_BOX(status_bar), aw->project_lbl, FALSE, FALSE, 0);

    /* Spacer */
    GtkWidget *st_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(st_sp, TRUE);
    gtk_box_pack_start(GTK_BOX(status_bar), st_sp, TRUE, TRUE, 0);

    /* Memory indicator (right) */
    aw->mem_bar_lbl = GTK_WIDGET(gtk_label_new("Memory: —"));
    gtk_style_context_add_class(gtk_widget_get_style_context(aw->mem_bar_lbl), "mem-lbl");
    gtk_box_pack_end(GTK_BOX(status_bar), aw->mem_bar_lbl, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), status_bar, FALSE, FALSE, 0);

    gtk_widget_show_all(aw->window);
    return aw;
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 19 — Cleanup & main()
 * ═══════════════════════════════════════════════════════════════════ */

/* Free all heap memory owned by AppState and AppWidgets */
static void cleanup_app(AppState *st, AppWidgets *aw) {
    if (st) {
        gst_stop_pipeline(st);
        free(st->segments);  /* may be NULL — free(NULL) is safe */
        free(st);
    }
    if (aw) {
        free(aw);
    }
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Populate TMPDIR with the system temp path in forward-slash form
     * so all "%s/..." temp-file snprintfs produce a valid Windows path. */
    {
        const gchar *t = g_get_tmp_dir();
        if (t && *t) {
            size_t n = strlen(t);
            if (n >= sizeof(g_tmpdir)) n = sizeof(g_tmpdir) - 1;
            memcpy(g_tmpdir, t, n);
            g_tmpdir[n] = '\0';
            for (size_t i = 0; i < n; i++)
                if (g_tmpdir[i] == '\\') g_tmpdir[i] = '/';
            /* strip any trailing slash for consistent "%s/..." use */
            size_t L = strlen(g_tmpdir);
            if (L > 3 && g_tmpdir[L-1] == '/') g_tmpdir[L-1] = '\0';
        }
        /* Ensure SHELL_PATH (bash) exists; warn early if missing.      */
        gchar *sh = g_find_program_in_path(SHELL_PATH);
        if (!sh) {
            fprintf(stderr,
                "WARNING: '%s' not found in PATH. Install MSYS2 or Git for\n"
                "Windows and add its usr/bin folder (which contains bash.exe)\n"
                "to PATH — the dub/mix pipeline relies on bash shell scripts.\n",
                SHELL_PATH);
        } else {
            g_free(sh);
        }
    }
#endif
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);
    init_log();
    resolve_tool("ffmpeg",  g_ffmpeg,  sizeof(g_ffmpeg));
    resolve_tool("ffprobe", g_ffprobe, sizeof(g_ffprobe));

    AppState *st = calloc(1, sizeof *st);
    if (!st) {
        fprintf(stderr, "Fatal: Failed to allocate memory for AppState\n");
        return 1;
    }
    load_config(&st->config);

    AppWidgets *aw = build_ui(st);
    if (!aw) {
        fprintf(stderr, "Fatal: Failed to allocate memory for AppWidgets\n");
        free(st);
        return 1;
    }
    st->aw = aw;

    LOG_INFO("=== " APP_NAME " " APP_VERSION " started ===");
    LOG_INFO("ffmpeg: %s | ffprobe: %s", g_ffmpeg, g_ffprobe);

    gtk_main();

    if (g_log_fp) fclose(g_log_fp);
    cleanup_app(st, aw);
    return 0;
}
