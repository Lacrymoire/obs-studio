#include <inttypes.h>
#include <math.h>
#include <obs-module.h>
#include <obs-hotkey.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/windows/window-helpers.h>
#include <windows.h>
#include <dxgi.h>
#include <util/sse-intrin.h>
#include <util/util_uint64.h>
#include <ipc-util/pipe.h>
#include <util/windows/obfuscate.h>
#include "inject-library.h"
#include "compat-helpers.h"
#include "graphics-hook-info.h"
#include "graphics-hook-ver.h"
#include "cursor-capture.h"
#include "app-helpers.h"
#include "audio-helpers.h"
#include "nt-stuff.h"

#define do_log(level, format, ...) \
	blog(level, "[game-capture: '%s'] " format, obs_source_get_name(gc->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

/* clang-format off */

#define SETTING_MODE                 "capture_mode"
#define SETTING_CAPTURE_WINDOW       "window"
#define SETTING_WINDOW_PRIORITY      "priority"
#define SETTING_COMPATIBILITY        "sli_compatibility"
#define SETTING_CURSOR               "capture_cursor"
#define SETTING_TRANSPARENCY         "allow_transparency"
#define SETTING_PREMULTIPLIED_ALPHA  "premultiplied_alpha"
#define SETTING_LIMIT_FRAMERATE      "limit_framerate"
#define SETTING_CAPTURE_OVERLAYS     "capture_overlays"
#define SETTING_ANTI_CHEAT_HOOK      "anti_cheat_hook"
#define SETTING_HOOK_RATE            "hook_rate"
#define SETTING_RGBA10A2_SPACE       "rgb10a2_space"
#define SETTINGS_COMPAT_INFO         "compat_info"
#define SETTING_FRAME_LOG            "framelock_log"
#define SETTING_FRAME_RING           "framelock_ring"
#define SETTING_BUFFERING            "framelock_buffering"
#define SETTING_BUFFER_DELAY         "framelock_delay"
#define SETTING_AUDIO_INFO           "framelock_audio_info"

/* deprecated */
#define SETTING_ANY_FULLSCREEN   "capture_any_fullscreen"

#define SETTING_MODE_ANY         "any_fullscreen"
#define SETTING_MODE_WINDOW      "window"
#define SETTING_MODE_HOTKEY      "hotkey"

#define HOTKEY_START             "hotkey_start"
#define HOTKEY_STOP              "hotkey_stop"

#define TEXT_MODE                  obs_module_text("Mode")
#define TEXT_GAME_CAPTURE          obs_module_text("GameCapture")
#define TEXT_ANY_FULLSCREEN        obs_module_text("GameCapture.AnyFullscreen")
#define TEXT_SLI_COMPATIBILITY     obs_module_text("SLIFix")
#define TEXT_ALLOW_TRANSPARENCY    obs_module_text("AllowTransparency")
#define TEXT_PREMULTIPLIED_ALPHA   obs_module_text("PremultipliedAlpha")
#define TEXT_WINDOW                obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY        obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE           obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS           obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE             obs_module_text("WindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR        obs_module_text("CaptureCursor")
#define TEXT_LIMIT_FRAMERATE       obs_module_text("GameCapture.LimitFramerate")
#define TEXT_CAPTURE_OVERLAYS      obs_module_text("GameCapture.CaptureOverlays")
#define TEXT_ANTI_CHEAT_HOOK       obs_module_text("GameCapture.AntiCheatHook")
#define TEXT_HOOK_RATE             obs_module_text("GameCapture.HookRate")
#define TEXT_HOOK_RATE_SLOW        obs_module_text("GameCapture.HookRate.Slow")
#define TEXT_HOOK_RATE_NORMAL      obs_module_text("GameCapture.HookRate.Normal")
#define TEXT_HOOK_RATE_FAST        obs_module_text("GameCapture.HookRate.Fast")
#define TEXT_HOOK_RATE_FASTEST     obs_module_text("GameCapture.HookRate.Fastest")
#define TEXT_RGBA10A2_SPACE        obs_module_text("GameCapture.Rgb10a2Space")
#define TEXT_RGBA10A2_SPACE_SRGB   obs_module_text("GameCapture.Rgb10a2Space.Srgb")
#define TEXT_RGBA10A2_SPACE_2100PQ obs_module_text("GameCapture.Rgb10a2Space.2100PQ")
#define TEXT_FRAME_LOG             obs_module_text("GameCapture.FrameLog")
#define TEXT_FRAME_LOG_TT          obs_module_text("GameCapture.FrameLog.Description")
#define TEXT_FRAME_RING            obs_module_text("GameCapture.FrameRing")
#define TEXT_FRAME_RING_TT         obs_module_text("GameCapture.FrameRing.Description")
#define TEXT_BUFFERING             obs_module_text("GameCapture.Buffering")
#define TEXT_BUFFERING_TT          obs_module_text("GameCapture.Buffering.Description")
#define TEXT_BUFFER_DELAY          obs_module_text("GameCapture.BufferDelay")
#define TEXT_BUFFER_DELAY_TT       obs_module_text("GameCapture.BufferDelay.Description")

#define TEXT_MODE_ANY            TEXT_ANY_FULLSCREEN
#define TEXT_MODE_WINDOW         obs_module_text("GameCapture.CaptureWindow")
#define TEXT_MODE_HOTKEY         obs_module_text("GameCapture.UseHotkey")

#define TEXT_HOTKEY_START        obs_module_text("GameCapture.HotkeyStart")
#define TEXT_HOTKEY_STOP         obs_module_text("GameCapture.HotkeyStop")

/* clang-format on */

#define DEFAULT_RETRY_INTERVAL 2.0f
#define ERROR_RETRY_INTERVAL 4.0f

enum capture_mode { CAPTURE_MODE_ANY, CAPTURE_MODE_WINDOW, CAPTURE_MODE_HOTKEY };

enum hook_rate { HOOK_RATE_SLOW, HOOK_RATE_NORMAL, HOOK_RATE_FAST, HOOK_RATE_FASTEST };

#define RGBA10A2_SPACE_SRGB "srgb"
#define RGBA10A2_SPACE_2100PQ "2100pq"

struct game_capture_config {
	char *title;
	char *class;
	char *executable;
	enum window_priority priority;
	enum capture_mode mode;
	bool cursor;
	bool force_shmem;
	bool allow_transparency;
	bool premultiplied_alpha;
	bool limit_framerate;
	bool capture_overlays;
	bool anticheat_hook;
	enum hook_rate hook_rate;
	bool is_10a2_2100pq;
	bool capture_audio;
	bool frame_log;
	uint32_t frame_ring;
	bool buffering;
	uint32_t buffer_delay;
};

/* frame-locked capture, Phase 3 constants */
#define LOCK_FIT_N 128          /* present records in the least-squares window (about 2 s at 60 fps) */
#define LOCK_FIT_MIN 16         /* records before the fitted clock is trusted */
#define LOCK_MAX_DELAY (FRAME_RING_MAX - 3)
#define LOCK_HYSTERESIS 0.1     /* frames; a repeat or skip needs the timeline this far past the edge */
#define LOCK_MAX_COUNTER_GAP 16 /* presents missing in a row: the game paused, refit from scratch */
#define LOCK_MAX_TIME_GAP_NS 250000000ULL
#define LOCK_HARD_GAP 3.0       /* periods; a present gap this long re-anchors the clock at once */
#define LOCK_SETTLE 0.33        /* fraction of the period: consecutive gaps this close to it mean the clock settled */
#define LOCK_PENDING_MAX 9      /* presents off the line kept back before the clock counts as lost */
#define LOCK_SHIFT 0.75         /* of the scale: a settled offset this large moves the anchor */
#define LOCK_ENGAGE_SPAN_NS 1000000000ULL /* a fresh fit must span this long before a cadence is chosen */
/* Phase 6 grid slots (lock_add): a late present that lands on the game's own
 * grid (a whole number of periods late, within LOCK_MISS_TOL of the period,
 * at least LOCK_MISS_TOL_MIN_NS) skipped vblanks; the game's clock did not
 * move.  Only trusted when the presents sit on the line tightly (jitter at
 * most half the tolerance) and the gap is under LOCK_MISS_MAX_NS. */
#define LOCK_MISS_TOL 0.15
#define LOCK_MISS_TOL_MIN_NS 1000000.0
#define LOCK_MISS_MAX_NS 1000000000ULL
#define LOCK_JITTER_N 32        /* window records before the jitter estimate is trusted */
/* Phase 6: re-anchors this close together mean the carried period is wrong (the game's rate moved
 * by more than the window follows): the clock is refitted from scratch instead */
#define LOCK_REANCHOR_BURST 3
#define LOCK_REANCHOR_BURST_NS 1000000000ULL
/* Phase 6 cadence (see cadence_choose): m output ticks per n game frames */
#define CADENCE_BAND 0.02      /* residual (paced tick rate / output rate - 1) a cadence may have to be chosen */
#define CADENCE_KEEP 0.03      /* residual the current cadence may drift to before it is re-chosen */
#define CADENCE_MAX_E 0.05     /* beyond this nothing is chosen: stock constant frame rate */
#define CADENCE_MAX_N 64       /* longest cycle in game frames */
#define CADENCE_CHANGE_NS 2000000000ULL /* the rate must be outside the band this long before a change */

typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_GetThreadDpiAwarenessContext)(VOID);
typedef DPI_AWARENESS_CONTEXT(WINAPI *PFN_GetWindowDpiAwarenessContext)(HWND);

struct game_capture {
	obs_source_t *source;
	obs_source_t *audio_source;

	struct cursor_data cursor_data;
	HANDLE injector_process;
	uint32_t cx;
	uint32_t cy;
	uint32_t pitch;
	DWORD process_id;
	DWORD thread_id;
	HWND next_window;
	HWND window;
	float retry_time;
	float fps_reset_time;
	float retry_interval;
	struct dstr title;
	struct dstr class;
	struct dstr executable;
	enum window_priority priority;
	obs_hotkey_pair_id hotkey_pair;
	volatile long hotkey_window;
	volatile bool deactivate_hook;
	volatile bool activate_hook_now;
	bool wait_for_target_startup;
	bool showing;
	bool active;
	bool capturing;
	bool activate_hook;
	bool process_is_64bit;
	bool error_acquiring;
	bool dwm_capture;
	bool initial_config;
	bool convert_16bit;
	bool is_app;
	bool cursor_hidden;

	struct game_capture_config config;

	ipc_pipe_server_t pipe;
	gs_texture_t *texture;
	gs_texture_t *extra_texture;
	gs_texrender_t *extra_texrender;
	bool is_10a2_2100pq;
	bool linear_sample;
	struct hook_info *global_hook_info;
	HANDLE keepalive_mutex;
	HANDLE hook_init;
	HANDLE hook_restart;
	HANDLE hook_stop;
	HANDLE hook_ready;
	HANDLE hook_exit;
	HANDLE hook_data_map;
	HANDLE global_hook_info_map;
	HANDLE target_process;
	HANDLE texture_mutexes[2];
	wchar_t *app_sid;
	int retrying;
	float cursor_check_time;

	union {
		struct {
			struct shmem_data *shmem_data;
			uint8_t *texture_buffers[2];
		};

		struct shtex_data *shtex_data;
		void *data;
	};

	void (*copy_texture)(struct game_capture *);

	/* frame-locked capture: Phase 2 frame ring (see ring_tick).  size is 0
	 * when the hook gave us a single texture (stock behaviour). */
	struct {
		gs_texture_t *textures[FRAME_RING_MAX];
		uint32_t size;
		uint32_t shown_slot;
		uint64_t shown_counter;
		uint64_t shown_time_ns;
		uint64_t read; /* present records consumed from the hook's log */
		uint64_t lost; /* records overwritten before they were read */
	} ring;

	/* frame-locked capture: Phase 3 smoothed game clock and buffered
	 * selection (see lock_fit / lock_select) */
	struct {
		uint64_t counters[LOCK_FIT_N];
		uint64_t times[LOCK_FIT_N];
		uint32_t n, head;
		bool ready;
		double period_ns;        /* fitted present period */
		double anchor_ns;        /* fitted present time of frame anchor_counter */
		uint64_t anchor_counter;
		uint64_t last_counter;
		uint64_t last_time_ns;
		uint64_t last_gap_ns;    /* gap before the last record (cold start: a slow game is not a pause) */
		uint64_t span_ns;        /* time the fit window covers */
		bool fresh;              /* the period comes from a fit spanning less than LOCK_ENGAGE_SPAN_NS (and not a full window) */
		uint64_t last_reanchor_ns;
		uint32_t reanchor_burst;
		/* presents off the fitted line, kept out of the window until the
		 * clock settles (lock_add) */
		uint64_t pending_counters[LOCK_PENDING_MAX];
		uint64_t pending_times[LOCK_PENDING_MAX];
		uint32_t pending_n;
		uint64_t resets, reanchors, outliers;
		bool active;             /* a frame has been chosen by the timeline */
		uint64_t shown;          /* the frame chosen last tick */
		double pos;              /* position of the chosen frame within its tick window */
		double smooth_ns;        /* smoothed present time of the shown frame */
		uint64_t repeats, skips, starved, lagged;
		bool starved_now;        /* this tick's choice was clamped to the newest frame */
		/* Phase 5 game-clock output (see game_clock_tick) */
		bool vfr;                /* engaged: the game is the slower clock */
		bool vfr_now;            /* this tick's frame carries a game-clock stamp */
		bool cad_refused;        /* no cadence within CADENCE_MAX_E for this game (logged once) */
		bool ring_short_logged;  /* the ring cannot hold the buffer delay at this rate (logged once) */
		uint64_t vfr_frames, vfr_paced, vfr_starved, vfr_repeats;
		uint64_t vfr_fresh;      /* ticks run nominally while the fit was fresh (after a refit) */
		/* Phase 6 cadence (see game_clock_tick): cad_m output ticks per cad_n game frames */
		uint32_t cad_m, cad_n;
		double cad_e;            /* its residual: paced tick rate / output rate - 1 */
		uint64_t cad_out_ns;     /* since when the game's rate has been outside CADENCE_KEEP (0 = within) */
		uint64_t cad_changes;
		uint64_t delay_ns;       /* buffer delay used this tick: the setting, or less when the ring is short */
		/* Phase 6 grid slots (lock_add): the clock runs over slots, not
		 * presents; a game on a vblank grid that misses a vblank has left a
		 * slot empty, not shifted its clock.  slot = counter + skew. */
		uint64_t skew;           /* slots the game skipped so far in this counter space */
		uint64_t misses;         /* the same, kept over counter restarts (statistics) */
		double jitter_ns;        /* running mean of |present - line| over the window's records */
		uint32_t jitter_n;       /* records in that mean since the last refit */
		uint64_t shown_v;        /* slot of the frame chosen last tick */
		struct {
			uint64_t counter, v;
		} vlog[PRESENT_LOG_SIZE]; /* slot of each present the ring read, by counter */
	} lock;

	/* frame-locked capture: Phase 1 debug log state (see flog_tick) */
	struct {
		FILE *file;
		char *path;
		obs_weak_output_t *output;
		uint64_t presents_read;
		uint64_t last_counter;
		uint64_t lost;
		uint64_t ticks;
		uint32_t check_countdown;
		bool waiting_logged;
		bool blocked_logged;
		/* lock counters when the log opened, so the close line reports this recording only */
		uint64_t repeats0, skips0, starved0, lagged0, resets0, reanchors0, misses0;
	} flog;

	PFN_SetThreadDpiAwarenessContext set_thread_dpi_awareness_context;
	PFN_GetThreadDpiAwarenessContext get_thread_dpi_awareness_context;
	PFN_GetWindowDpiAwarenessContext get_window_dpi_awareness_context;
};

struct graphics_offsets offsets32 = {0};
struct graphics_offsets offsets64 = {0};

static inline bool use_anticheat(struct game_capture *gc)
{
	return gc->config.anticheat_hook && !gc->is_app;
}

static inline HANDLE open_mutex_plus_id(struct game_capture *gc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return gc->is_app ? open_app_mutex(gc->app_sid, new_name) : open_mutex(new_name);
}

static inline HANDLE open_mutex_gc(struct game_capture *gc, const wchar_t *name)
{
	return open_mutex_plus_id(gc, name, gc->process_id);
}

static inline HANDLE open_event_plus_id(struct game_capture *gc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return gc->is_app ? open_app_event(gc->app_sid, new_name) : open_event(new_name);
}

static inline HANDLE open_event_gc(struct game_capture *gc, const wchar_t *name)
{
	return open_event_plus_id(gc, name, gc->process_id);
}

static inline HANDLE open_map_plus_id(struct game_capture *gc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	swprintf(new_name, 64, L"%s%lu", name, id);

	debug("map id: %S", new_name);

	return gc->is_app ? open_app_map(gc->app_sid, new_name) : OpenFileMappingW(GC_MAPPING_FLAGS, false, new_name);
}

static inline HANDLE open_hook_info(struct game_capture *gc)
{
	return open_map_plus_id(gc, SHMEM_HOOK_INFO, gc->process_id);
}

static inline enum gs_color_format convert_format(uint32_t format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return GS_RGBA;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return GS_BGRX;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return GS_BGRA;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return GS_RGBA16;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return GS_RGBA16F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return GS_RGBA32F;
	}

	return GS_UNKNOWN;
}

static void close_handle(HANDLE *p_handle)
{
	HANDLE handle = *p_handle;
	if (handle) {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		*p_handle = NULL;
	}
}

static inline HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleW(L"kernel32");
	return kernel32_handle;
}

static inline HANDLE open_process(DWORD desired_access, bool inherit_handle, DWORD process_id)
{
	typedef HANDLE(WINAPI * PFN_OpenProcess)(DWORD, BOOL, DWORD);
	static PFN_OpenProcess open_process_proc = NULL;
	if (!open_process_proc)
		open_process_proc =
			(PFN_OpenProcess)ms_get_obfuscated_func(kernel32(), "NuagUykjcxr", 0x1B694B59451ULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

static inline float hook_rate_to_float(enum hook_rate rate)
{
	switch (rate) {
	case HOOK_RATE_SLOW:
		return 2.0f;
	case HOOK_RATE_FAST:
		return 0.5f;
	case HOOK_RATE_FASTEST:
		return 0.1f;
	case HOOK_RATE_NORMAL:
		/* FALLTHROUGH */
	default:
		return 1.0f;
	}
}

/* ------------------------------------------------------------------------- */
/* Frame-locked capture, Phase 3: smoothed game clock and buffered selection.
 *
 * lock_add() feeds every present record (counter, raw present time) into a
 * window of the last LOCK_FIT_N records; lock_fit() fits a straight line
 * through them (least squares), which is the game's steady clock: a period
 * and the smoothed present time of any frame.  Raw times wobble by a couple
 * of milliseconds, the fitted line by a few hundredths.
 *
 * lock_select() runs once per OBS video tick at nominal tick time T.  It maps
 * T - delay * tick into a fractional frame position x on the fitted clock and
 * normally shows the frame after the previous one.  Only when x says that
 * frame is more than LOCK_HYSTERESIS frames early does it repeat the previous
 * frame, and only when x is more than LOCK_HYSTERESIS frames past the next
 * one does it skip.  A clock difference between game and OBS then produces one
 * single repeat or skip per beat period, never a burst, because the decision
 * follows the smooth line and the hysteresis is far wider than its noise.
 *
 * Phase 4 (patch 0006): the fitted period survives a jump in the present
 * times; only the anchor moves.  A hard stall (a gap of LOCK_HARD_GAP periods
 * or more, a counter restart) re-anchors at once, so a loading screen or an
 * engine hitch is one hold that ends when the buffer delay is rebuilt.  A
 * shorter hitch is held back, because the game often catches up with a burst
 * of quick presents: its points stay out of the window until two consecutive
 * normal gaps show where the clock settled, and the anchor moves there only
 * if that is more than half a period off the old line.  Only a cold start, or
 * a clock that never settles, refits from scratch; until that fit is trusted
 * ring_tick() steps one frame per tick. */

static void lock_reset(struct game_capture *gc, const char *why)
{
	if (gc->lock.n && why)
		gc->lock.resets++;
	gc->lock.n = 0;
	gc->lock.head = 0;
	gc->lock.pending_n = 0;
	gc->lock.ready = false;
	gc->lock.active = false;
	gc->lock.jitter_n = 0;
	gc->lock.fresh = true;
	if (why && gc->config.frame_log)
		info("frame lock: refit (%s)", why);
}

/* a new hook (ring_open): the window, the pending points and the frame on
 * screen mean nothing any more; the fitted period is carried over and the
 * statistics, reported per recording, are kept */
static void lock_reopen(struct game_capture *gc)
{
	const uint64_t repeats = gc->lock.repeats, skips = gc->lock.skips, starved = gc->lock.starved,
		       lagged = gc->lock.lagged, resets = gc->lock.resets, reanchors = gc->lock.reanchors,
		       outliers = gc->lock.outliers, misses = gc->lock.misses;
	const double period_ns = gc->lock.period_ns;
	const bool ready = gc->lock.ready;

	const uint64_t vfr_frames = gc->lock.vfr_frames, vfr_paced = gc->lock.vfr_paced,
		       vfr_starved = gc->lock.vfr_starved, vfr_repeats = gc->lock.vfr_repeats,
		       cad_changes = gc->lock.cad_changes;

	memset(&gc->lock, 0, sizeof(gc->lock));
	gc->lock.cad_changes = cad_changes;
	gc->lock.vfr_frames = vfr_frames;
	gc->lock.vfr_paced = vfr_paced;
	gc->lock.vfr_starved = vfr_starved;
	gc->lock.vfr_repeats = vfr_repeats;
	gc->lock.repeats = repeats;
	gc->lock.skips = skips;
	gc->lock.starved = starved;
	gc->lock.lagged = lagged;
	gc->lock.resets = resets;
	gc->lock.reanchors = reanchors;
	gc->lock.outliers = outliers;
	gc->lock.misses = misses;
	gc->lock.period_ns = period_ns;
	gc->lock.ready = ready;
	gc->lock.fresh = true;
}

/* the window and the pending points hold slots (counter + skew); last_counter
 * is the present counter, so a counter restart is still seen */
static inline void lock_window_add(struct game_capture *gc, uint64_t counter, uint64_t v, uint64_t time_ns)
{
	gc->lock.counters[gc->lock.head] = v;
	gc->lock.times[gc->lock.head] = time_ns;
	gc->lock.head = (gc->lock.head + 1) % LOCK_FIT_N;
	if (gc->lock.n < LOCK_FIT_N)
		gc->lock.n++;
	if (gc->lock.last_time_ns && time_ns > gc->lock.last_time_ns)
		gc->lock.last_gap_ns = time_ns - gc->lock.last_time_ns;
	gc->lock.last_counter = counter;
	gc->lock.last_time_ns = time_ns;
}

static inline void lock_pending_add(struct game_capture *gc, uint64_t counter, uint64_t v, uint64_t time_ns)
{
	if (gc->lock.pending_n < LOCK_PENDING_MAX) {
		gc->lock.pending_counters[gc->lock.pending_n] = v;
		gc->lock.pending_times[gc->lock.pending_n] = time_ns;
		gc->lock.pending_n++;
	}
	gc->lock.last_counter = counter;
	gc->lock.last_time_ns = time_ns;
}

/* how far the presents sit from the line, a running mean over the window */
static inline void lock_note_err(struct game_capture *gc, double err)
{
	if (gc->lock.jitter_n == 0)
		gc->lock.jitter_ns = fabs(err);
	else
		gc->lock.jitter_ns += (fabs(err) - gc->lock.jitter_ns) / 16.0;
	if (gc->lock.jitter_n < LOCK_JITTER_N)
		gc->lock.jitter_n++;
}

/* the slot of a present the ring read (lock_add gave it one); a counter the
 * ring has not seen gets the current skew */
static inline uint64_t lock_v_of(const struct game_capture *gc, uint64_t counter)
{
	if (counter) {
		const uint32_t i = (uint32_t)((counter - 1) % PRESENT_LOG_SIZE);
		if (gc->lock.vlog[i].counter == counter)
			return gc->lock.vlog[i].v;
	}
	return counter + gc->lock.skew;
}

/* smoothed present time of slot v on the fitted clock */
static inline double lock_time_of(const struct game_capture *gc, uint64_t v)
{
	return gc->lock.anchor_ns + (double)(int64_t)(v - gc->lock.anchor_counter) * gc->lock.period_ns;
}

/* the same for a fractional position on the timeline (Phase 6 cadence ticks) */
static inline double lock_time_of_x(const struct game_capture *gc, double x)
{
	return gc->lock.anchor_ns + (x - (double)(int64_t)gc->lock.anchor_counter) * gc->lock.period_ns;
}

/* Phase 6: the scale of the hitch and shift thresholds.  A game faster than
 * the output (120 Hz vsync) misses a vblank now and then; that shifts its
 * clock by one period, half an output tick, which the output cannot resolve
 * and the buffer delay absorbs.  Measured against the output tick instead of
 * the game period, such misses are noise inside the least-squares window
 * (the line drifts with them) and not re-anchors that restart the window.
 * For a game at or below the output rate the scale is the period, as before. */
static inline double lock_scale(const struct game_capture *gc)
{
	const double interval = (double)obs_get_frame_interval_ns();
	return gc->lock.period_ns > interval ? gc->lock.period_ns : interval;
}

/* the clock moved: keep its period, start the window again from this record */
static void lock_reanchor(struct game_capture *gc, uint64_t counter, uint64_t time_ns, const char *why, bool new_space)
{
	/* Phase 6: re-anchors in a burst mean the carried period no longer fits
	 * (the game's rate moved: a cutscene, a menu, an uncapped game under
	 * load); refit from scratch so the cadence can follow */
	if (time_ns - gc->lock.last_reanchor_ns < LOCK_REANCHOR_BURST_NS) {
		if (++gc->lock.reanchor_burst >= LOCK_REANCHOR_BURST) {
			gc->lock.reanchor_burst = 0;
			gc->lock.last_reanchor_ns = time_ns;
			lock_reset(gc, "clock drifting");
			return;
		}
	} else {
		gc->lock.reanchor_burst = 1;
	}
	gc->lock.last_reanchor_ns = time_ns;
	gc->lock.reanchors++;
	gc->lock.n = 0;
	gc->lock.head = 0;
	gc->lock.pending_n = 0;
	gc->lock.anchor_counter = counter;
	gc->lock.anchor_ns = (double)time_ns;
	if (new_space) {
		/* the counters restarted: the frame on screen cannot be compared */
		gc->lock.active = false;
		gc->lock.shown = 0;
	}
	if (gc->config.frame_log)
		info("frame lock: re-anchor (%s)", why);
}

/* feeds one present to the clock and returns its slot.  Phase 6: the clock
 * runs over grid slots.  A present that arrives a whole number of periods
 * late, on the game's own grid, is a game that skipped vblanks (a 120 Hz game
 * under load, a 60 Hz game missing one): the skipped slots stay empty, the
 * clock stands.  A present off the grid is a hitch or a clock shift, handled
 * as before (pending points, re-anchor). */
static uint64_t lock_add(struct game_capture *gc, uint64_t counter, uint64_t time_ns)
{
	uint64_t v;

	if (gc->lock.ready && (gc->lock.n || gc->lock.pending_n)) {
		const uint64_t gap = time_ns - gc->lock.last_time_ns;
		double err;

		if (counter <= gc->lock.last_counter) {
			/* the counters restarted: a new slot space */
			gc->lock.skew = 0;
			v = counter;
			lock_reanchor(gc, v, time_ns, "counter went backwards", true);
			lock_window_add(gc, counter, v, time_ns);
			return v;
		}
		v = counter + gc->lock.skew;
		err = (double)time_ns - lock_time_of(gc, v);

		if (gc->lock.pending_n == 0 && err > 0.5 * gc->lock.period_ns && gap < LOCK_MISS_MAX_NS &&
		    gc->lock.jitter_n >= LOCK_JITTER_N) {
			double tol = LOCK_MISS_TOL * gc->lock.period_ns;

			if (tol < LOCK_MISS_TOL_MIN_NS)
				tol = LOCK_MISS_TOL_MIN_NS;
			if (gc->lock.jitter_ns <= 0.5 * tol) {
				const double k = (double)llround(err / gc->lock.period_ns);

				if (k >= 1.0 && fabs(err - k * gc->lock.period_ns) <= tol) {
					/* on the grid, k slots late: the game skipped them */
					gc->lock.skew += (uint64_t)k;
					gc->lock.misses += (uint64_t)k;
					v += (uint64_t)k;
					lock_note_err(gc, err - k * gc->lock.period_ns);
					lock_window_add(gc, counter, v, time_ns);
					return v;
				}
			}
		}

		/* a hard gap is counted in game periods (Phase 6: no fixed cap, so a
		 * 5 fps game is not re-anchored on every frame) */
		if ((double)gap >= LOCK_HARD_GAP * lock_scale(gc)) {
			lock_reanchor(gc, v, time_ns, "long pause", false);
			lock_window_add(gc, counter, v, time_ns);
			return v;
		}

		if (gc->lock.pending_n == 0) {
			if (fabs(err) <= lock_scale(gc)) {
				lock_note_err(gc, err);
				lock_window_add(gc, counter, v, time_ns);
			} else {
				lock_pending_add(gc, counter, v, time_ns);
			}
			return v;
		}

		lock_pending_add(gc, counter, v, time_ns);
		{
			const uint32_t n = gc->lock.pending_n;
			const uint64_t *pc = gc->lock.pending_counters;
			const uint64_t *pt = gc->lock.pending_times;
			bool settled = false;

			if (n >= 3) {
				const double g1 = (double)(pt[n - 1] - pt[n - 2]) - gc->lock.period_ns;
				const double g2 = (double)(pt[n - 2] - pt[n - 3]) - gc->lock.period_ns;
				settled = fabs(g1) <= LOCK_SETTLE * lock_scale(gc) &&
					  fabs(g2) <= LOCK_SETTLE * lock_scale(gc);
			}
			if (settled) {
				const uint64_t c1 = pc[n - 2], t1 = pt[n - 2], c2 = pc[n - 1], t2 = pt[n - 1];
				const double off = (((double)t1 - lock_time_of(gc, c1)) + ((double)t2 - lock_time_of(gc, c2))) / 2.0;

				if (fabs(off) > LOCK_SHIFT * lock_scale(gc)) {
					lock_reanchor(gc, c1, t1, "clock shifted", false);
				} else {
					/* a hitch the game caught up from: the line stands */
					gc->lock.outliers += n - 2;
					gc->lock.pending_n = 0;
				}
				/* (no slots were skipped while points were pending) */
				lock_window_add(gc, c1 - gc->lock.skew, c1, t1);
				lock_window_add(gc, counter, c2, t2);
			} else if (n >= LOCK_PENDING_MAX) {
				/* nothing like the old clock any more: start over */
				lock_reset(gc, "clock lost");
				lock_window_add(gc, counter, v, time_ns);
			}
		}
		return v;
	}

	/* cold start: no trusted clock yet */
	if (gc->lock.n) {
		const uint64_t dc = counter - gc->lock.last_counter;
		const uint64_t dt = time_ns - gc->lock.last_time_ns;

		if (counter <= gc->lock.last_counter) {
			gc->lock.skew = 0;
			lock_reset(gc, "counter went backwards");
		} else if (dc > LOCK_MAX_COUNTER_GAP) {
			lock_reset(gc, "presents missing");
		} else if (dt > LOCK_MAX_TIME_GAP_NS && dt > 3 * gc->lock.last_gap_ns) {
			lock_reset(gc, "long pause"); /* (a slow game's ordinary gap is not a pause) */
		}
	}
	v = counter + gc->lock.skew;
	lock_window_add(gc, counter, v, time_ns);
	return v;
}

/* least squares of present time over counter, in coordinates relative to the
 * newest record so the doubles stay exact */
static void lock_fit(struct game_capture *gc)
{
	const uint32_t n = gc->lock.n;
	const uint32_t newest = (gc->lock.head + LOCK_FIT_N - 1) % LOCK_FIT_N;
	const uint64_t c0 = gc->lock.counters[newest];
	const uint64_t t0 = gc->lock.times[newest];
	double sx = 0, sy = 0, sxx = 0, sxy = 0, mx, my, vxx, vxy;

	if (n == 0)
		return;
	if (n < LOCK_FIT_MIN && !gc->lock.ready)
		return;

	for (uint32_t i = 0; i < n; i++) {
		const uint32_t k = (newest + LOCK_FIT_N - i) % LOCK_FIT_N;
		const double x = -(double)(int64_t)(c0 - gc->lock.counters[k]);
		const double y = -(double)(int64_t)(t0 - gc->lock.times[k]);
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	mx = sx / n;
	my = sy / n;
	{
		const uint32_t oldest = (newest + LOCK_FIT_N - (n - 1)) % LOCK_FIT_N;
		gc->lock.span_ns = t0 - gc->lock.times[oldest];
	}
	if (n < LOCK_FIT_MIN) {
		/* re-anchored: the carried period, the anchor on the mean of what
		 * the window holds so far */
		gc->lock.anchor_counter = c0;
		gc->lock.anchor_ns = (double)t0 + (my - gc->lock.period_ns * mx);
		return;
	}
	vxx = sxx - n * mx * mx;
	vxy = sxy - n * mx * my;
	if (vxx <= 0.0) {
		gc->lock.ready = false;
		return;
	}

	gc->lock.period_ns = vxy / vxx;
	gc->lock.anchor_counter = c0;
	gc->lock.anchor_ns = (double)t0 + (my - gc->lock.period_ns * mx);
	gc->lock.ready = gc->lock.period_ns > 1000000.0; /* a sane clock runs slower than 1000 fps */
	/* mature: a window spanning a second, or a full one (a fast game fills
	 * it sooner) */
	if (gc->lock.ready && (gc->lock.span_ns >= LOCK_ENGAGE_SPAN_NS || n >= LOCK_FIT_N))
		gc->lock.fresh = false;
}

/* chooses the frame counter to show at nominal tick time tick_ns; newest is the
 * newest copied frame, oldest the oldest one that is safe to switch to (the
 * hook may be writing the slot of the frame before it) */
static uint64_t lock_select(struct game_capture *gc, uint64_t tick_ns, uint64_t newest, uint64_t oldest)
{
	uint64_t delay_ns = (uint64_t)gc->config.buffer_delay * obs_get_frame_interval_ns();
	double target;
	/* Phase 6: the ring holds ring - 2 safe frames; at a high game rate the
	 * delay would reach past them, so it is shortened to what the ring holds
	 * (the video is then that much less delayed for this game) */
	if (gc->ring.size >= 4) {
		const double fit = (double)(gc->ring.size - 3) * gc->lock.period_ns;
		if ((double)delay_ns > fit) {
			delay_ns = (uint64_t)fit;
			if (!gc->lock.ring_short_logged) {
				gc->lock.ring_short_logged = true;
				info("frame lock: the ring of %" PRIu32 " cannot hold the buffer delay at %.1f fps; video "
				     "delay %.1f ms instead of %.1f ms for this game (a larger ring setting fixes it)",
				     gc->ring.size, 1e9 / gc->lock.period_ns, fit / 1000000.0,
				     (double)gc->config.buffer_delay * obs_get_frame_interval_ns() / 1000000.0);
			}
		}
	}
	gc->lock.delay_ns = delay_ns;
	target = (double)(int64_t)(tick_ns - delay_ns);
	/* Phase 6: the timeline runs over grid slots (lock_add); the choice is
	 * made in slots and mapped back to the newest present at or before the
	 * chosen slot, so a slot the game skipped shows the frame before it */
	const double x = (double)(int64_t)gc->lock.anchor_counter + (target - gc->lock.anchor_ns) / gc->lock.period_ns;
	const bool was_active = gc->lock.active;
	const uint64_t v_newest = lock_v_of(gc, newest), v_oldest = lock_v_of(gc, oldest);
	/* the frame on screen: chosen by the timeline last tick, or whatever
	 * ring_tick showed while the clock was not trusted */
	const uint64_t prev_c = was_active ? gc->lock.shown : gc->ring.shown_counter;
	const uint64_t prev = was_active ? gc->lock.shown_v : lock_v_of(gc, prev_c);
	uint64_t v, c;

	gc->lock.starved_now = false;

	if (!was_active) {
		/* first choice on this clock: the frame due at the target time, but
		 * never a step back from the frame already on screen (after a refit
		 * that would play motion in reverse); repeating it catches up */
		v = x > 0.0 ? (uint64_t)x : 0;
		if (v < prev && prev <= v_newest)
			v = prev; /* prev > newest: the counters restarted */
		if (v < v_oldest)
			v = v_oldest;
		gc->lock.active = true;
	} else if (gc->lock.vfr) {
		/* Phase 6 cadence: game_clock_tick places the ticks on the timeline
		 * 1/(2m) frame clear of every frame boundary, so the frame due is
		 * simply floor(x) (planned repeats and planned skips included);
		 * never a step back.  After a stall the backlog is shown briefly
		 * and the timeline is caught up, as the paced ticks did before. */
		v = x > 0.0 ? (uint64_t)x : 0;
		if (v < prev && prev <= v_newest)
			v = prev;
	} else {
		const uint64_t next = prev + 1;
		const double e = x - (double)(int64_t)next;

		if (e < -LOCK_HYSTERESIS)
			v = prev; /* the next frame is not due yet: repeat */
		else if (e >= 2.0 + LOCK_HYSTERESIS)
			v = (uint64_t)x; /* far behind (the game paused and resumed): resync */
		else if (e >= 1.0 + LOCK_HYSTERESIS)
			v = next + 1; /* the frame after next is due as well: skip one */
		else
			v = next;
	}

	if (v > v_newest) {
		v = v_newest; /* the game has not presented it yet */
		gc->lock.starved++;
		gc->lock.starved_now = true;
	}
	if (v < v_oldest && v != prev) {
		/* switching to a frame whose slot the hook may be rewriting; the
		 * frame on screen is safe, its slot is read_slot */
		v = v_oldest;
		gc->lock.lagged++;
	}

	if (v == prev) {
		c = prev_c; /* the frame on screen stays (it may be older than oldest) */
	} else {
		/* the newest present at or before slot v */
		for (c = newest; c > oldest; c--) {
			if (lock_v_of(gc, c) <= v)
				break;
		}
	}
	if (prev_c) {
		/* count what the viewer gets, after the clamps (a slot the game
		 * skipped is the game's drop, not a skip of ours) */
		if (c == prev_c)
			gc->lock.repeats++;
		else if (c > prev_c + 1)
			gc->lock.skips++;
	}

	gc->lock.shown = c;
	gc->lock.shown_v = lock_v_of(gc, c);
	gc->lock.pos = x - (double)(int64_t)gc->lock.shown_v;
	gc->lock.smooth_ns = lock_time_of(gc, gc->lock.shown_v);
	return c;
}

/* ------------------------------------------------------------------------- */
/* Frame-locked capture, Phase 2: the frame ring.
 *
 * With a ring the hook copies every present into one of ring_size shared
 * textures and records (counter, present time, slot) in its present log.  On
 * each OBS video tick ring_tick() reads the new records and points gc->texture
 * at the slot to draw; read_slot in the shared block tells the hook which slot
 * that is, so it steps over it instead of overwriting it.
 * Phase 2 still shows the newest copied frame, exactly like the single shared
 * texture did; Phase 3 replaces that choice with timestamp smoothing. */

static void ring_free(struct game_capture *gc)
{
	/* slot 0 is the texture init_shtex_capture opened; every slot may be
	 * aliased by gc->texture, so drop that alias here and let the caller's
	 * gs_texture_destroy(gc->texture) see NULL */
	for (uint32_t i = 0; i < gc->ring.size; i++) {
		if (gc->ring.textures[i] == gc->texture)
			gc->texture = NULL;
		gs_texture_destroy(gc->ring.textures[i]);
	}
	memset(&gc->ring, 0, sizeof(gc->ring));
}

/* called from init_shtex_capture inside obs_enter_graphics(); slot0 is the
 * texture it already opened from tex_handle (== tex_handles[0]) */
static bool ring_open(struct game_capture *gc, gs_texture_t *slot0)
{
	const volatile struct shtex_data *shtex = gc->shtex_data;
	const uint32_t n = shtex->log_version == SHTEX_LOG_VERSION ? shtex->ring_size : 1;

	memset(&gc->ring, 0, sizeof(gc->ring));

	if (n <= 1)
		return true;
	if (n > FRAME_RING_MAX) {
		warn("frame ring: hook reports %u slots, more than the %d this build supports", n, FRAME_RING_MAX);
		return false;
	}

	gc->ring.textures[0] = slot0;
	for (uint32_t i = 1; i < n; i++) {
		gc->ring.textures[i] = gs_texture_open_shared(shtex->tex_handles[i]);
		if (!gc->ring.textures[i]) {
			warn("frame ring: failed to open shared handle of slot %u", i);
			for (uint32_t j = 1; j < i; j++)
				gs_texture_destroy(gc->ring.textures[j]);
			memset(&gc->ring, 0, sizeof(gc->ring));
			return false;
		}
	}

	gc->ring.size = n;
	gc->ring.shown_slot = 0;
	gc->shtex_data->read_slot = 0;
	lock_reopen(gc);
	info("frame ring: %u slots of %" PRIu32 "x%" PRIu32 "; buffering %s, delay %" PRIu32 " frame%s", n, gc->cx,
	     gc->cy, gc->config.buffering ? "on" : "off", gc->config.buffer_delay,
	     gc->config.buffer_delay == 1 ? "" : "s");
	return true;
}

/* Frame-locked capture, Phase 5/6: game-clock output with a cadence.
 *
 * Called once per tick after the frame for this tick was chosen.  While the
 * smoothed clock is trusted, the recording follows the game's clock with a
 * cadence of m output ticks per n game frames (cadence_choose): 1/1 for a
 * 60 fps lock, 2/1 for a 30 fps lock, 1/2 for 120 Hz vsync, 5/12 for 144,
 * 3/2 for 40 fps, 60/1 for 1 fps.  The ticks of a cycle sit on the game's
 * timeline at x = (1/2 + j*n)/m, spaced n/m frames and 1/(2m) frame clear of
 * every frame boundary, so the pattern is exact (every frame of a 30 fps game
 * is shown twice, never once or three times) and clock jitter cannot flip it.
 * The next video tick is asked for at the time of the next lattice point plus
 * the buffer delay; the encoder stamps the frame with this tick's time.  The
 * paced tick rate is the game's rate times m/n, within CADENCE_BAND of the
 * output rate; in retimed mode the audio follows that residual (libobs,
 * patch 0012), in VFR mode the stamps carry it.
 *   - while the game is stalled (this tick was starved) the nominal tick runs
 *     and the repeated frame is stamped with the tick time, so the stall is
 *     a run of ordinary repeats as in CFR.  Repeats are always encoded: the
 *     muxer only releases audio behind the newest video packet and NVENC
 *     holds the last B-frames until later frames arrive, so a tick without a
 *     frame would stall both for the length of a loading screen.
 *   - a game whose rate moves (an uncapped game, a 24 fps cutscene, a menu)
 *     drifts out of CADENCE_KEEP; after CADENCE_CHANGE_NS the cadence is
 *     chosen again (logged).  A rate the lock cannot follow refits the clock
 *     (lock_reanchor burst) and the cadence re-engages on the new fit.
 * Everything here is ignored by libobs unless game-clock output is enabled. */

static inline uint32_t cadence_gcd(uint32_t a, uint32_t b)
{
	while (b) {
		const uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

/* r = output ticks per game frame (game period over tick interval).  For
 * n = 1, 2, ... the nearest m is tried; the first cadence whose residual is
 * within band wins (the shortest cycle: fewest, most evenly placed planned
 * repeats or skips), else the closest one within CADENCE_MAX_E.  false when
 * nothing fits. */
static bool cadence_choose(double r, double band, uint32_t *out_m, uint32_t *out_n, double *out_e)
{
	uint32_t best_m = 0, best_n = 0;
	double best_e = 1.0;

	if (!(r > 0.0))
		return false;
	for (uint32_t n = 1; n <= CADENCE_MAX_N; n++) {
		uint32_t m = (uint32_t)lround((double)n * r);
		double e;

		if (m < 1)
			m = 1;
		if (cadence_gcd(m, n) != 1)
			continue;
		e = ((double)m / (double)n) / r - 1.0;
		/* a whole- or half-frame cadence (n <= 2) is the smoothest
		 * pattern there is, so it is taken up to CADENCE_KEEP: the
		 * band the lock would hold it in anyway */
		if (fabs(e) <= (n <= 2 ? CADENCE_KEEP : band)) {
			best_m = m;
			best_n = n;
			best_e = e;
			break;
		}
		if (fabs(e) < fabs(best_e)) {
			best_m = m;
			best_n = n;
			best_e = e;
		}
	}
	if (!best_m || fabs(best_e) > CADENCE_MAX_E)
		return false;
	*out_m = best_m;
	*out_n = best_n;
	*out_e = best_e;
	return true;
}

static void game_clock_tick(struct game_capture *gc, bool repeat)
{
	uint64_t frame_ts = 0, next_tick = 0;
	const bool skip = false;
	bool vfr = false;

	if (gc->config.buffering && gc->lock.ready && gc->lock.active && obs_get_game_clock_enabled()) {
		const uint64_t interval = obs_get_frame_interval_ns();
		const double r = gc->lock.period_ns / (double)interval; /* output ticks per game frame */
		const uint64_t now = os_gettime_ns();
		uint32_t m = gc->lock.cad_m, n = gc->lock.cad_n;
		double e = gc->lock.cad_e;

		if (gc->lock.fresh) {
			/* a fresh fit over a few presents is too rough to choose or
			 * judge a cadence by (a 16-point fit across a hitch once read
			 * a 120 fps game as 35 fps): the cadence in force stays and
			 * the ticks run nominally until the fit spans a second */
		} else if (m) {
			e = ((double)m / (double)n) / r - 1.0;
			if (fabs(e) > CADENCE_KEEP) {
				/* outside the keep band the cadence is re-chosen after
				 * CADENCE_CHANGE_NS (a rate wobbling about a boundary does
				 * not hop); beyond CADENCE_MAX_E the tick rate is already
				 * further off than the audio can follow: at once */
				if (!gc->lock.cad_out_ns)
					gc->lock.cad_out_ns = now;
				if (fabs(e) > CADENCE_MAX_E || now - gc->lock.cad_out_ns >= CADENCE_CHANGE_NS) {
					uint32_t m2, n2;
					double e2;

					if (cadence_choose(r, CADENCE_BAND, &m2, &n2, &e2)) {
						info("game-clock output: cadence %" PRIu32 "/%" PRIu32 " -> %" PRIu32 "/%" PRIu32
						     " (game %.2f fps, residual %+.2f %%)",
						     m, n, m2, n2, 1e9 / gc->lock.period_ns, e2 * 100.0);
						m = m2;
						n = n2;
						e = e2;
						gc->lock.cad_changes++;
					} else {
						m = n = 0;
					}
					gc->lock.cad_out_ns = 0;
				}
			} else {
				gc->lock.cad_out_ns = 0;
			}
		} else {
			if (!cadence_choose(r, CADENCE_BAND, &m, &n, &e))
				m = n = 0;
			gc->lock.cad_out_ns = 0;
		}
		if (!m && !gc->lock.fresh && !gc->lock.cad_refused) {
			info("game-clock output: no cadence within %.0f %% for a game at %.2f fps; constant frame rate for "
			     "this source",
			     CADENCE_MAX_E * 100.0, 1e9 / gc->lock.period_ns);
			gc->lock.cad_refused = true;
		} else if (m) {
			gc->lock.cad_refused = false;
		}
		gc->lock.cad_m = m;
		gc->lock.cad_n = n;
		gc->lock.cad_e = m ? e : 0.0;
		vfr = m != 0;

		if (vfr) {
			if (gc->lock.starved_now) {
				gc->lock.vfr_starved++;
			} else if (gc->lock.fresh) {
				gc->lock.vfr_fresh++;
			} else {
				/* this tick's position and the next lattice point n/m on:
				 * m*x = 1/2 + j*n; round(j) snaps a tick that was not
				 * placed (the first paced one, the one after a stall) */
				const double x = (double)(int64_t)gc->lock.shown_v + gc->lock.pos;
				const double step = (double)n / (double)m;
				const double j = (double)llround(((double)m * (x + step) - 0.5) / (double)n);
				const double xn = (0.5 + j * (double)n) / (double)m;
				const double delay = (double)gc->lock.delay_ns;

				frame_ts = (uint64_t)(lock_time_of_x(gc, x) + delay);
				next_tick = (uint64_t)(lock_time_of_x(gc, xn) + delay);
				gc->lock.vfr_paced++;
			}
			gc->lock.vfr_frames++;
			if (repeat)
				gc->lock.vfr_repeats++;
		}
	}

	if (vfr != gc->lock.vfr) {
		if (vfr)
			info("game-clock output: engaged (game %.2f fps, period %.3f ms, tick %.3f ms, cadence %" PRIu32 "/%" PRIu32
			     ", residual %+.2f %%)",
			     1e9 / gc->lock.period_ns, gc->lock.period_ns / 1000000.0,
			     obs_get_frame_interval_ns() / 1000000.0, gc->lock.cad_m, gc->lock.cad_n, gc->lock.cad_e * 100.0);
		else
			info("game-clock output: released (%" PRIu64 " ticks, %" PRIu64 " paced, %" PRIu64
			     " starved, %" PRIu64 " after refits, %" PRIu64 " repeats, %" PRIu64 " cadence changes)",
			     gc->lock.vfr_frames, gc->lock.vfr_paced, gc->lock.vfr_starved, gc->lock.vfr_fresh,
			     gc->lock.vfr_repeats, gc->lock.cad_changes);
		gc->lock.vfr = vfr;
		if (!vfr) {
			gc->lock.cad_m = gc->lock.cad_n = 0;
			gc->lock.cad_out_ns = 0;
		}
	}
	gc->lock.vfr_now = vfr && !gc->lock.starved_now && !gc->lock.fresh;
	obs_video_set_game_clock_tick(frame_ts, next_tick, skip);
}

static void ring_tick(struct game_capture *gc)
{
	volatile struct shtex_data *shtex = gc->shtex_data;
	const volatile struct present_record *rec;
	uint64_t count, read;

	if (!gc->ring.size || !gc->capturing || !shtex)
		return;

	count = shtex->present_count;
	read = gc->ring.read;

	if (count < read) {
		/* the hook re-initialized its capture and restarted the counter */
		read = 0;
	}
	if (count - read > PRESENT_LOG_SIZE) {
		gc->ring.lost += count - read - PRESENT_LOG_SIZE;
		read = count - PRESENT_LOG_SIZE;
	}
	if (count == read && !gc->config.buffering)
		return; /* no new frame since the last tick: keep showing the same slot */

	/* Phase 3: feed the new records to the smoothed clock (Phase 6: and
	 * remember the slot each one got, for the choice and the frame log) */
	for (uint64_t i = read; i < count; i++) {
		const uint64_t counter = shtex->present_log[i % PRESENT_LOG_SIZE].counter;
		const uint64_t time_ns = shtex->present_log[i % PRESENT_LOG_SIZE].time_ns;
		const uint64_t v = lock_add(gc, counter, time_ns);

		if (counter) {
			gc->lock.vlog[(counter - 1) % PRESENT_LOG_SIZE].counter = counter;
			gc->lock.vlog[(counter - 1) % PRESENT_LOG_SIZE].v = v;
		}
	}
	if (count != read)
		lock_fit(gc);
	gc->ring.read = count;

	if (count == 0)
		return;

	{
		/* a frame is safe to switch to while it is newer than
		 * newest - (ring - 2): the hook writes the slot after the newest */
		const uint64_t keep = gc->ring.size > 2 ? gc->ring.size - 2 : 0;
		const uint64_t oldest = count > keep ? count - keep : 1;
		uint64_t c = count;

		if (gc->config.buffering && gc->lock.ready) {
			/* Phase 3 choice: the frame the game's timeline puts at this
			 * tick, delay frames back */
			c = lock_select(gc, obs_get_video_frame_time(), count, oldest);
		} else {
			if (gc->config.buffering && gc->ring.shown_counter && gc->ring.shown_counter < count) {
				/* Phase 4: the clock is not trusted yet (start of capture,
				 * after a pause or a jump in the present times): advance
				 * one frame per tick from the frame on screen instead of
				 * jumping to the newest one, so the timeline resumes close
				 * to where it left off */
				c = gc->ring.shown_counter + 1;
				if (c < oldest)
					c = oldest;
			}
			/* otherwise the Phase 2 choice: the newest copied frame */
			gc->lock.active = false;
		}

		rec = &shtex->present_log[(c - 1) % PRESENT_LOG_SIZE];
		if (rec->counter != c)
			rec = &shtex->present_log[(count - 1) % PRESENT_LOG_SIZE];
	}

	if (rec->counter == gc->ring.shown_counter) {
		/* same frame as last tick: nothing to switch */
		game_clock_tick(gc, true);
		return;
	}
	if (rec->slot < gc->ring.size) {
		gc->ring.shown_slot = rec->slot;
		gc->ring.shown_counter = rec->counter;
		gc->ring.shown_time_ns = rec->time_ns;
		gc->texture = gc->ring.textures[rec->slot];
		shtex->read_slot = rec->slot;
	}
	game_clock_tick(gc, false);
}

/* ------------------------------------------------------------------------- */
/* Frame-locked capture, Phase 1 instrumentation.
 *
 * While a recording output is active and the "Frame-lock debug log" setting is
 * on, the source writes <recording file>.frames.csv next to the recording:
 *   present rows: every frame the hook copied into the shared texture (counter
 *                 and present time), read from the ring in the shared data block;
 *   tick rows:    one per OBS video tick: the tick's nominal time, the wall time
 *                 the tick ran, the newest copied frame at that moment (what the
 *                 single shared texture holds, so what the render pass draws),
 *                 the recording's frame count, and the counter change since the
 *                 previous tick (1 = one new game frame, 0 = repeat, 2+ = skip).
 * All *_ns values are os_gettime_ns() nanoseconds, the clock libobs uses for
 * obs_get_video_frame_time(); the hook stamps frames with the same clock.
 * Diagnostics only: nothing here changes what is captured or drawn. */

#define FLOG_CHECK_TICKS 15

static bool flog_find_recording(void *param, obs_output_t *output)
{
	obs_weak_output_t **found = param;
	const char *id = obs_output_get_id(output);

	if (!id || !obs_output_active(output))
		return true;

	if (strcmp(id, "ffmpeg_muxer") == 0 || strcmp(id, "mp4_output") == 0 || strcmp(id, "mov_output") == 0 ||
	    strcmp(id, "ffmpeg_output") == 0) {
		*found = obs_output_get_weak_output(output);
		return false;
	}

	return true;
}

static void flog_close(struct game_capture *gc, const char *reason)
{
	if (gc->flog.file) {
		fprintf(gc->flog.file, "end,%" PRIu64 ",%" PRIu64 ",,,,,%s,,,,\n", obs_get_video_frame_time(),
			os_gettime_ns(), reason);
		fclose(gc->flog.file);
		gc->flog.file = NULL;
		info("frame log closed (%s): %s, %" PRIu64 " ticks, %" PRIu64 " lost log entries; frame lock: %" PRIu64
		     " repeats, %" PRIu64 " skips, %" PRIu64 " starved, %" PRIu64 " lagged, %" PRIu64 " refits, %" PRIu64
		     " re-anchors, %" PRIu64 " slots the game skipped",
		     reason, gc->flog.path, gc->flog.ticks, gc->flog.lost, gc->lock.repeats - gc->flog.repeats0,
		     gc->lock.skips - gc->flog.skips0, gc->lock.starved - gc->flog.starved0,
		     gc->lock.lagged - gc->flog.lagged0, gc->lock.resets - gc->flog.resets0,
		     gc->lock.reanchors - gc->flog.reanchors0, gc->lock.misses - gc->flog.misses0);
	}

	if (gc->flog.output) {
		obs_weak_output_release(gc->flog.output);
		gc->flog.output = NULL;
	}

	bfree(gc->flog.path);
	gc->flog.path = NULL;
	gc->flog.presents_read = 0;
	gc->flog.last_counter = 0;
	gc->flog.lost = 0;
	gc->flog.ticks = 0;
	gc->flog.check_countdown = 0;
	gc->flog.waiting_logged = false;
	gc->flog.blocked_logged = false;
}

static bool flog_list_active_output(void *param, obs_output_t *output)
{
	struct dstr *list = param;

	if (obs_output_active(output)) {
		if (!dstr_is_empty(list))
			dstr_cat(list, ", ");
		dstr_catf(list, "%s ('%s')", obs_output_get_id(output), obs_output_get_name(output));
	}

	return true;
}

static void flog_open(struct game_capture *gc, obs_weak_output_t *weak, obs_output_t *output)
{
	const volatile struct shtex_data *shtex = gc->shtex_data;
	struct obs_video_info ovi = {0};
	obs_data_t *settings = obs_output_get_settings(output);
	const char *id = obs_output_get_id(output);
	struct dstr rec = {0};
	struct dstr path = {0};
	uint64_t count, latest;

	dstr_copy(&rec, obs_data_get_string(settings, strcmp(id, "ffmpeg_output") == 0 ? "url" : "path"));
	obs_data_release(settings);

	if (dstr_is_empty(&rec)) {
		dstr_free(&rec);
		return;
	}

	dstr_copy_dstr(&path, &rec);
	dstr_cat(&path, ".frames.csv");

	gc->flog.file = os_fopen(path.array, "wb");
	if (!gc->flog.file) {
		warn("frame log: could not open '%s'", path.array);
		dstr_free(&path);
		dstr_free(&rec);
		return;
	}

	setvbuf(gc->flog.file, NULL, _IOFBF, 1 << 16);
	gc->flog.path = path.array;
	gc->flog.output = weak;
	obs_weak_output_addref(weak);

	count = shtex->present_count;
	latest = count ? shtex->present_log[(count - 1) % PRESENT_LOG_SIZE].time_ns : 0;
	gc->flog.presents_read = count;
	gc->flog.last_counter = gc->ring.size ? gc->ring.shown_counter : count;
	gc->flog.lost = 0;
	gc->flog.ticks = 0;
	gc->flog.repeats0 = gc->lock.repeats;
	gc->flog.skips0 = gc->lock.skips;
	gc->flog.starved0 = gc->lock.starved;
	gc->flog.lagged0 = gc->lock.lagged;
	gc->flog.resets0 = gc->lock.resets;
	gc->flog.reanchors0 = gc->lock.reanchors;
	gc->flog.misses0 = gc->lock.misses;

	obs_get_video_info(&ovi);
	fprintf(gc->flog.file, "# obs-frame-lock log v3\n");
	fprintf(gc->flog.file,
		"# obs=%s fps=%" PRIu32 "/%" PRIu32 " base=%" PRIu32 "x%" PRIu32 " output=%" PRIu32 "x%" PRIu32
		" capture=%" PRIu32 "x%" PRIu32 " ring=%" PRIu32 " buffering=%s delay=%" PRIu32
		" gameclock=%s source=%s recording=%s\n",
		obs_get_version_string(), ovi.fps_num, ovi.fps_den, ovi.base_width, ovi.base_height,
		ovi.output_width, ovi.output_height, gc->cx, gc->cy, gc->ring.size,
		gc->config.buffering && gc->ring.size ? "on" : "off", gc->config.buffer_delay,
		obs_get_game_clock_enabled() ? (obs_get_game_clock_retime() ? "retime" : "on") : "off",
		obs_source_get_name(gc->source), rec.array);
	fprintf(gc->flog.file,
		"# present: one row per frame the hook copied (counter, present time, ring slot)\n"
		"# tick: one row per OBS video tick: nominal tick time, wall time, the frame shown (counter, present\n"
		"#       time, slot), recording frame count, counter change since the previous tick (1 normal,\n"
		"#       0 repeat, 2+ skip), the hook's running count of slot conflicts, and with buffering the\n"
		"#       smoothed present time of the shown frame and its position on the timeline (frames);\n"
		"#       note 'gc m/n' = game-clock stamp, paced with a cadence of m ticks per n game frames, 'paused' = not recorded\n"
		"# all *_ns values are os_gettime_ns() (QueryPerformanceCounter) nanoseconds\n");
	fprintf(gc->flog.file,
		"kind,tick_ns,now_ns,counter,present_ns,out_frames,delta,note,slot,conflicts,smooth_ns,pos\n");
	fprintf(gc->flog.file, "start,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,,,,,,\n",
		obs_get_video_frame_time(), os_gettime_ns(), count, latest, obs_output_get_total_frames(output));

	info("frame log opened: %s", path.array);
	dstr_free(&rec);
}

static void flog_tick(struct game_capture *gc)
{
	const bool capturing = gc->config.frame_log && gc->capturing && gc->shtex_data;
	const bool enabled = capturing && gc->global_hook_info->type == CAPTURE_TYPE_TEXTURE &&
			     gc->shtex_data->log_version == SHTEX_LOG_VERSION;
	const volatile struct shtex_data *shtex;
	obs_output_t *output = NULL;
	uint64_t count, read, shown, latest, tick_ns, now_ns, conflicts;
	uint32_t slot;
	int64_t delta;
	char note[64];
	char lockcols[64];

	if (!enabled) {
		if (gc->flog.file)
			flog_close(gc, "capture stopped");

		/* say once per capture why the log cannot run */
		if (capturing && !gc->flog.blocked_logged) {
			gc->flog.blocked_logged = true;
			if (gc->global_hook_info->type != CAPTURE_TYPE_TEXTURE)
				warn("frame log: capture runs in shared-memory mode (SLI/Crossfire compatibility or no "
				     "shared-texture support); the present log needs shared-texture capture");
			else
				warn("frame log: the hook's shared data block has no present log (log_version=%" PRIu32
				     "); the graphics-hook DLL loaded in the game is not this build's (see the "
				     "'injecting graphics hook' line above); restart the game",
				     gc->shtex_data->log_version);
		}
		return;
	}

	if (gc->flog.output) {
		output = obs_weak_output_get_output(gc->flog.output);
		if (output && !obs_output_active(output)) {
			obs_output_release(output);
			output = NULL;
		}
		if (!output)
			flog_close(gc, "recording stopped");
	}

	if (!gc->flog.file) {
		obs_weak_output_t *weak = NULL;

		if (gc->flog.check_countdown > 0) {
			gc->flog.check_countdown--;
			return;
		}
		gc->flog.check_countdown = FLOG_CHECK_TICKS;

		obs_enum_outputs(flog_find_recording, &weak);
		if (!weak) {
			if (!gc->flog.waiting_logged) {
				struct dstr active_outputs = {0};

				gc->flog.waiting_logged = true;
				obs_enum_outputs(flog_list_active_output, &active_outputs);
				info("frame log: on, waiting for a recording to start (active outputs now: %s)",
				     dstr_is_empty(&active_outputs) ? "none" : active_outputs.array);
				dstr_free(&active_outputs);
			}
			return;
		}

		output = obs_weak_output_get_output(weak);
		if (output)
			flog_open(gc, weak, output);
		obs_weak_output_release(weak);

		if (!gc->flog.file) {
			if (output)
				obs_output_release(output);
			return;
		}
	}

	shtex = gc->shtex_data;
	count = shtex->present_count;
	read = gc->flog.presents_read;

	if (count < read) {
		/* the hook re-initialized its capture and restarted the counter */
		read = 0;
		gc->flog.last_counter = 0;
	}
	if (count - read > PRESENT_LOG_SIZE) {
		gc->flog.lost += count - read - PRESENT_LOG_SIZE;
		read = count - PRESENT_LOG_SIZE;
	}
	if (gc->ring.size && count > gc->ring.read)
		count = gc->ring.read; /* only presents the ring has given a slot */
	for (; read < count; read++) {
		const volatile struct present_record *rec = &shtex->present_log[read % PRESENT_LOG_SIZE];
		const uint64_t counter = rec->counter;
		uint64_t miss = 0;

		if (gc->ring.size && counter > 1) {
			/* Phase 6: slots the game skipped before this present */
			const uint64_t v = lock_v_of(gc, counter), vp = lock_v_of(gc, counter - 1);
			if (v > vp + 1)
				miss = v - vp - 1;
		}
		if (miss)
			fprintf(gc->flog.file, "present,,,%" PRIu64 ",%" PRIu64 ",,,miss %" PRIu64 ",%" PRIu32 ",,,\n", counter,
				rec->time_ns, miss, rec->slot);
		else
			fprintf(gc->flog.file, "present,,,%" PRIu64 ",%" PRIu64 ",,,,%" PRIu32 ",,,\n", counter,
				rec->time_ns, rec->slot);
	}

	tick_ns = obs_get_video_frame_time();
	now_ns = os_gettime_ns();
	if (gc->ring.size) {
		/* with a ring the drawn frame is whatever ring_tick chose this tick */
		shown = gc->ring.shown_counter;
		latest = gc->ring.shown_time_ns;
		slot = gc->ring.shown_slot;
		conflicts = shtex->conflicts;
	} else {
		/* a single shared texture always holds the newest copied frame */
		shown = count;
		latest = count ? shtex->present_log[(count - 1) % PRESENT_LOG_SIZE].time_ns : 0;
		slot = 0;
		conflicts = 0;
	}
	delta = (int64_t)(shown - gc->flog.last_counter);
	note[0] = 0;
	/* an active output stays active while paused; the ticks of a pause are
	 * not in the recording, so mark them for the analysis */
	if (obs_output_paused(output))
		snprintf(note, sizeof(note), "paused");
	if (gc->lock.vfr_now)
		snprintf(note + strlen(note), sizeof(note) - strlen(note), "%sgc %" PRIu32 "/%" PRIu32, note[0] ? " " : "",
			 gc->lock.cad_m, gc->lock.cad_n);
	if (gc->flog.lost || gc->ring.lost)
		snprintf(note + strlen(note), sizeof(note) - strlen(note), "%slost=%" PRIu64 "/%" PRIu64,
			 note[0] ? " " : "", gc->flog.lost, gc->ring.lost);

	if (gc->ring.size && gc->config.buffering && gc->lock.active)
		snprintf(lockcols, sizeof(lockcols), "%.0f,%.3f", gc->lock.smooth_ns, gc->lock.pos);
	else
		lockcols[0] = 0;

	fprintf(gc->flog.file,
		"tick,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%" PRId64 ",%s,%" PRIu32 ",%" PRIu64 ",%s\n",
		tick_ns, now_ns, shown, latest, obs_output_get_total_frames(output), delta, note, slot, conflicts,
		lockcols);

	gc->flog.presents_read = count;
	gc->flog.last_counter = shown;
	gc->flog.ticks++;

	obs_output_release(output);
}

/* ------------------------------------------------------------------------- */

static void stop_capture(struct game_capture *gc)
{
	flog_close(gc, "capture stopped");

	ipc_pipe_server_free(&gc->pipe);

	if (gc->hook_stop) {
		SetEvent(gc->hook_stop);
	}
	if (gc->global_hook_info) {
		UnmapViewOfFile(gc->global_hook_info);
		gc->global_hook_info = NULL;
	}
	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	if (gc->app_sid) {
		LocalFree(gc->app_sid);
		gc->app_sid = NULL;
	}

	close_handle(&gc->hook_restart);
	close_handle(&gc->hook_stop);
	close_handle(&gc->hook_ready);
	close_handle(&gc->hook_exit);
	close_handle(&gc->hook_init);
	close_handle(&gc->hook_data_map);
	close_handle(&gc->keepalive_mutex);
	close_handle(&gc->global_hook_info_map);
	close_handle(&gc->target_process);
	close_handle(&gc->texture_mutexes[0]);
	close_handle(&gc->texture_mutexes[1]);

	obs_enter_graphics();
	gs_texrender_destroy(gc->extra_texrender);
	gc->extra_texrender = NULL;
	gs_texture_destroy(gc->extra_texture);
	gc->extra_texture = NULL;
	ring_free(gc);
	gs_texture_destroy(gc->texture);
	gc->texture = NULL;
	obs_leave_graphics();

	if (gc->active)
		info("capture stopped");

	// if it was previously capturing, send an unhooked signal
	if (gc->capturing) {
		gc->capturing = false;
		signal_handler_t *sh = obs_source_get_signal_handler(gc->source);
		calldata_t data = {0};
		calldata_set_ptr(&data, "source", gc->source);
		signal_handler_signal(sh, "unhooked", &data);
		calldata_free(&data);

		// Also update audio source to stop capturing
		if (gc->audio_source)
			reconfigure_audio_source(gc->audio_source, NULL);
	}

	gc->copy_texture = NULL;
	gc->wait_for_target_startup = false;
	gc->active = false;

	if (gc->retrying)
		gc->retrying--;
}

static inline void free_config(struct game_capture_config *config)
{
	bfree(config->title);
	bfree(config->class);
	bfree(config->executable);
	memset(config, 0, sizeof(*config));
}

static void game_capture_destroy(void *data)
{
	struct game_capture *gc = data;
	stop_capture(gc);

	if (gc->audio_source)
		destroy_audio_source(gc->source, &gc->audio_source);

	signal_handler_t *sh = obs_source_get_signal_handler(gc->source);
	signal_handler_disconnect(sh, "rename", rename_audio_source, &gc->audio_source);

	if (gc->hotkey_pair)
		obs_hotkey_pair_unregister(gc->hotkey_pair);

	obs_enter_graphics();
	cursor_data_free(&gc->cursor_data);
	obs_leave_graphics();

	dstr_free(&gc->title);
	dstr_free(&gc->class);
	dstr_free(&gc->executable);
	free_config(&gc->config);
	bfree(gc);
}

static inline bool using_older_non_mode_format(obs_data_t *settings)
{
	return obs_data_has_user_value(settings, SETTING_ANY_FULLSCREEN) &&
	       !obs_data_has_user_value(settings, SETTING_MODE);
}

static inline void get_config(struct game_capture_config *cfg, obs_data_t *settings, const char *window)
{
	const char *mode_str = NULL;

	ms_build_window_strings(window, &cfg->class, &cfg->title, &cfg->executable);

	if (using_older_non_mode_format(settings)) {
		bool any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
		mode_str = any ? SETTING_MODE_ANY : SETTING_MODE_WINDOW;
	} else {
		mode_str = obs_data_get_string(settings, SETTING_MODE);
	}

	if (mode_str && strcmp(mode_str, SETTING_MODE_WINDOW) == 0)
		cfg->mode = CAPTURE_MODE_WINDOW;
	else if (mode_str && strcmp(mode_str, SETTING_MODE_HOTKEY) == 0)
		cfg->mode = CAPTURE_MODE_HOTKEY;
	else
		cfg->mode = CAPTURE_MODE_ANY;

	cfg->priority = (enum window_priority)obs_data_get_int(settings, SETTING_WINDOW_PRIORITY);
	cfg->force_shmem = obs_data_get_bool(settings, SETTING_COMPATIBILITY);
	cfg->cursor = obs_data_get_bool(settings, SETTING_CURSOR);
	cfg->allow_transparency = obs_data_get_bool(settings, SETTING_TRANSPARENCY);
	cfg->premultiplied_alpha = obs_data_get_bool(settings, SETTING_PREMULTIPLIED_ALPHA);
	cfg->limit_framerate = obs_data_get_bool(settings, SETTING_LIMIT_FRAMERATE);
	cfg->capture_overlays = obs_data_get_bool(settings, SETTING_CAPTURE_OVERLAYS);
	cfg->anticheat_hook = obs_data_get_bool(settings, SETTING_ANTI_CHEAT_HOOK);
	cfg->hook_rate = (enum hook_rate)obs_data_get_int(settings, SETTING_HOOK_RATE);
	cfg->is_10a2_2100pq = strcmp(obs_data_get_string(settings, SETTING_RGBA10A2_SPACE), "2100pq") == 0;
	cfg->capture_audio = obs_data_get_bool(settings, SETTING_CAPTURE_AUDIO);
	cfg->frame_log = obs_data_get_bool(settings, SETTING_FRAME_LOG);
	cfg->frame_ring = (uint32_t)obs_data_get_int(settings, SETTING_FRAME_RING);
	if (cfg->frame_ring < 1)
		cfg->frame_ring = 1;
	if (cfg->frame_ring > FRAME_RING_MAX)
		cfg->frame_ring = FRAME_RING_MAX;
	cfg->buffering = obs_data_get_bool(settings, SETTING_BUFFERING);
	cfg->buffer_delay = (uint32_t)obs_data_get_int(settings, SETTING_BUFFER_DELAY);
	if (cfg->buffer_delay < 1)
		cfg->buffer_delay = 1;
	if (cfg->buffer_delay > LOCK_MAX_DELAY)
		cfg->buffer_delay = LOCK_MAX_DELAY;
	/* the shown frame sits delay (+1) frames behind the newest one and the
	 * hook writes one slot ahead, so the ring must hold delay + 3 slots */
	if (cfg->buffering && cfg->frame_ring < cfg->buffer_delay + 3)
		cfg->frame_ring = cfg->buffer_delay + 3;
}

static inline int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}

static inline bool capture_needs_reset(struct game_capture_config *cfg1, struct game_capture_config *cfg2)
{
	if (cfg1->mode != cfg2->mode) {
		return true;

	} else if (cfg1->mode == CAPTURE_MODE_WINDOW &&
		   (s_cmp(cfg1->class, cfg2->class) != 0 || s_cmp(cfg1->title, cfg2->title) != 0 ||
		    s_cmp(cfg1->executable, cfg2->executable) != 0 || cfg1->priority != cfg2->priority)) {
		return true;

	} else if (cfg1->force_shmem != cfg2->force_shmem) {
		return true;

	} else if (cfg1->limit_framerate != cfg2->limit_framerate) {
		return true;

	} else if (cfg1->capture_overlays != cfg2->capture_overlays) {
		return true;

	} else if (cfg1->frame_ring != cfg2->frame_ring) {
		return true;
	}

	return false;
}

static bool hotkey_start(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct game_capture *gc = data;

	if (pressed && gc->config.mode == CAPTURE_MODE_HOTKEY) {
		info("Activate hotkey pressed");
		os_atomic_set_long(&gc->hotkey_window, (long)(uintptr_t)GetForegroundWindow());
		os_atomic_set_bool(&gc->deactivate_hook, true);
		os_atomic_set_bool(&gc->activate_hook_now, true);
	}

	return true;
}

static bool hotkey_stop(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct game_capture *gc = data;

	if (pressed && gc->config.mode == CAPTURE_MODE_HOTKEY) {
		info("Deactivate hotkey pressed");
		os_atomic_set_bool(&gc->deactivate_hook, true);
	}

	return true;
}

static void game_capture_get_hooked(void *data, calldata_t *cd)
{
	struct game_capture *gc = data;
	if (!gc)
		return;

	calldata_set_bool(cd, "hooked", gc->capturing);

	if (gc->capturing) {
		calldata_set_string(cd, "title", gc->title.array);
		calldata_set_string(cd, "class", gc->class.array);
		calldata_set_string(cd, "executable", gc->executable.array);
	} else {
		calldata_set_string(cd, "title", "");
		calldata_set_string(cd, "class", "");
		calldata_set_string(cd, "executable", "");
	}
}

static void game_capture_update(void *data, obs_data_t *settings)
{
	struct game_capture *gc = data;
	struct game_capture_config cfg;
	bool reset_capture = false;
	const char *window = obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);

	get_config(&cfg, settings, window);
	reset_capture = capture_needs_reset(&cfg, &gc->config);

	gc->error_acquiring = false;

	if (cfg.mode == CAPTURE_MODE_HOTKEY && gc->config.mode != CAPTURE_MODE_HOTKEY) {
		gc->activate_hook = false;
	} else {
		gc->activate_hook = !!window && !!*window;
	}

	free_config(&gc->config);
	gc->config = cfg;
	{
		const double delay_ms = gc->config.buffering ? (double)gc->config.buffer_delay *
								       (double)obs_get_frame_interval_ns() / 1000000.0
							     : 0.0;
		info("frame-lock buffering: %s, delay %" PRIu32 " frame%s (video delay %.1f ms; matching audio sync offset "
		     "+%.0f ms), ring %" PRIu32 ", debug log: %s",
		     gc->config.buffering ? "on" : "off", gc->config.buffer_delay, gc->config.buffer_delay == 1 ? "" : "s",
		     delay_ms, delay_ms, gc->config.frame_ring, gc->config.frame_log ? "on" : "off");
	}
	if (!gc->config.frame_log)
		flog_close(gc, "log disabled");
	gc->retry_interval = DEFAULT_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
	gc->wait_for_target_startup = false;
	gc->is_10a2_2100pq = gc->config.is_10a2_2100pq;

	dstr_free(&gc->title);
	dstr_free(&gc->class);
	dstr_free(&gc->executable);

	if (cfg.mode == CAPTURE_MODE_WINDOW) {
		dstr_copy(&gc->title, gc->config.title);
		dstr_copy(&gc->class, gc->config.class);
		dstr_copy(&gc->executable, gc->config.executable);
		gc->priority = gc->config.priority;
	}

	if (!gc->initial_config) {
		if (reset_capture) {
			stop_capture(gc);
		}
	} else {
		gc->initial_config = false;
	}

	/* Linked audio capture source stuff */
	setup_audio_source(gc->source, &gc->audio_source, cfg.mode == CAPTURE_MODE_WINDOW ? window : NULL,
			   cfg.capture_audio, cfg.priority);
}

extern void wait_for_hook_initialization(void);

static void *game_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct game_capture *gc = bzalloc(sizeof(*gc));

	wait_for_hook_initialization();

	gc->source = source;
	gc->initial_config = true;
	gc->retry_interval = DEFAULT_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
	gc->hotkey_pair = obs_hotkey_pair_register_source(gc->source, HOTKEY_START, TEXT_HOTKEY_START, HOTKEY_STOP,
							  TEXT_HOTKEY_STOP, hotkey_start, hotkey_stop, gc, gc);

	const HMODULE hModuleUser32 = GetModuleHandle(L"User32.dll");
	if (hModuleUser32) {
		PFN_SetThreadDpiAwarenessContext set_thread_dpi_awareness_context =
			(PFN_SetThreadDpiAwarenessContext)GetProcAddress(hModuleUser32, "SetThreadDpiAwarenessContext");
		PFN_GetThreadDpiAwarenessContext get_thread_dpi_awareness_context =
			(PFN_GetThreadDpiAwarenessContext)GetProcAddress(hModuleUser32, "GetThreadDpiAwarenessContext");
		PFN_GetWindowDpiAwarenessContext get_window_dpi_awareness_context =
			(PFN_GetWindowDpiAwarenessContext)GetProcAddress(hModuleUser32, "GetWindowDpiAwarenessContext");
		if (set_thread_dpi_awareness_context && get_thread_dpi_awareness_context &&
		    get_window_dpi_awareness_context) {
			gc->set_thread_dpi_awareness_context = set_thread_dpi_awareness_context;
			gc->get_thread_dpi_awareness_context = get_thread_dpi_awareness_context;
			gc->get_window_dpi_awareness_context = get_window_dpi_awareness_context;
		}
	}

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_add(sh, "void unhooked(ptr source)");
	signal_handler_add(sh, "void hooked(ptr source, string title, string class, string executable)");

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph,
			 "void get_hooked(out bool hooked, out string title, out string class, out string executable)",
			 game_capture_get_hooked, gc);

	signal_handler_connect(sh, "rename", rename_audio_source, &gc->audio_source);

	game_capture_update(gc, settings);
	return gc;
}

#define STOP_BEING_BAD                                                      \
	"  This is most likely due to security software. Please make sure " \
	"that the OBS installation folder is excluded/ignored in the "      \
	"settings of the security software you are using."

static bool check_file_integrity(struct game_capture *gc, const char *file, const char *name)
{
	DWORD error;
	HANDLE handle;
	wchar_t *w_file = NULL;

	if (!file || !*file) {
		warn("Game capture %s not found." STOP_BEING_BAD, name);
		return false;
	}

	if (!os_utf8_to_wcs_ptr(file, 0, &w_file)) {
		warn("Could not convert file name to wide string");
		return false;
	}

	handle = CreateFileW(w_file, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	bfree(w_file);

	if (handle != INVALID_HANDLE_VALUE) {
		CloseHandle(handle);
		return true;
	}

	error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND) {
		warn("Game capture file '%s' not found." STOP_BEING_BAD, file);
	} else if (error == ERROR_ACCESS_DENIED) {
		warn("Game capture file '%s' could not be loaded." STOP_BEING_BAD, file);
	} else {
		warn("Game capture file '%s' could not be loaded: %lu." STOP_BEING_BAD, file, error);
	}

	return false;
}

static inline bool is_64bit_windows(void)
{
#ifdef _WIN64
	return true;
#else
	BOOL x86 = false;
	bool success = !!IsWow64Process(GetCurrentProcess(), &x86);
	return success && !!x86;
#endif
}

static inline bool is_64bit_process(HANDLE process)
{
	BOOL x86 = true;
	if (is_64bit_windows()) {
		bool success = !!IsWow64Process(process, &x86);
		if (!success) {
			return false;
		}
	}

	return !x86;
}

static inline bool open_target_process(struct game_capture *gc)
{
	gc->target_process = open_process(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, false, gc->process_id);
	if (!gc->target_process) {
		warn("could not open process: %s", gc->config.executable);
		return false;
	}

	gc->process_is_64bit = is_64bit_process(gc->target_process);
	gc->is_app = is_app(gc->target_process);
	if (gc->is_app) {
		gc->app_sid = get_app_sid(gc->target_process);
	}
	return true;
}

static inline bool init_keepalive(struct game_capture *gc)
{
	wchar_t new_name[64];
	swprintf(new_name, 64, WINDOW_HOOK_KEEPALIVE L"%lu", gc->process_id);

	gc->keepalive_mutex = gc->is_app ? create_app_mutex(gc->app_sid, new_name)
					 : CreateMutexW(NULL, false, new_name);
	if (!gc->keepalive_mutex) {
		warn("Failed to create keepalive mutex: %lu", GetLastError());
		return false;
	}

	return true;
}

static inline bool init_texture_mutexes(struct game_capture *gc)
{
	gc->texture_mutexes[0] = open_mutex_gc(gc, MUTEX_TEXTURE1);
	gc->texture_mutexes[1] = open_mutex_gc(gc, MUTEX_TEXTURE2);

	if (!gc->texture_mutexes[0] || !gc->texture_mutexes[1]) {
		DWORD error = GetLastError();
		if (error == 2) {
			if (!gc->retrying) {
				gc->retrying = 2;
				info("hook not loaded yet, retrying..");
			}
		} else {
			warn("failed to open texture mutexes: %lu", GetLastError());
		}
		return false;
	}

	return true;
}

/* if there's already a hook in the process, then signal and start */
static inline bool attempt_existing_hook(struct game_capture *gc)
{
	gc->hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (gc->hook_restart) {
		debug("existing hook found, signaling process: %s", gc->config.executable);
		SetEvent(gc->hook_restart);
		return true;
	}

	return false;
}

static inline void reset_frame_interval(struct game_capture *gc)
{
	struct obs_video_info ovi;
	uint64_t interval = 0;

	if (obs_get_video_info(&ovi)) {
		interval = util_mul_div64(ovi.fps_den, 1000000000ULL, ovi.fps_num);

		/* Always limit capture framerate to some extent.  If a game
		 * running at 900 FPS is being captured without some sort of
		 * limited capture interval, it will dramatically reduce
		 * performance. */
		if (!gc->config.limit_framerate)
			interval /= 2;
	}

	gc->global_hook_info->frame_interval = interval;
}

static inline bool init_hook_info(struct game_capture *gc)
{
	gc->global_hook_info_map = open_hook_info(gc);
	if (!gc->global_hook_info_map) {
		warn("init_hook_info: get_hook_info failed: %lu", GetLastError());
		return false;
	}

	gc->global_hook_info =
		MapViewOfFile(gc->global_hook_info_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*gc->global_hook_info));
	if (!gc->global_hook_info) {
		warn("init_hook_info: failed to map data view: %lu", GetLastError());
		return false;
	}

	if (gc->config.force_shmem) {
		warn("init_hook_info: user is forcing shared memory "
		     "(multi-adapter compatibility mode)");
	}

	gc->global_hook_info->offsets = gc->process_is_64bit ? offsets64 : offsets32;
	gc->global_hook_info->capture_overlay = gc->config.capture_overlays;
	gc->global_hook_info->force_shmem = gc->config.force_shmem;
	gc->global_hook_info->UNUSED_use_scale = false;
	gc->global_hook_info->allow_srgb_alias = true;
	gc->global_hook_info->frame_ring_size = gc->config.frame_ring;
	reset_frame_interval(gc);

	obs_enter_graphics();
	if (!gs_shared_texture_available()) {
		warn("init_hook_info: shared texture capture unavailable");
		gc->global_hook_info->force_shmem = true;
	}
	obs_leave_graphics();

	return true;
}

static void pipe_log(void *param, uint8_t *data, size_t size)
{
	struct game_capture *gc = param;
	if (data && size)
		info("%s", data);
}

static inline bool init_pipe(struct game_capture *gc)
{
	char name[64];
	snprintf(name, sizeof(name), "%s%lu", PIPE_NAME, gc->process_id);

	if (!ipc_pipe_server_start(&gc->pipe, name, pipe_log, gc)) {
		warn("init_pipe: failed to start pipe");
		return false;
	}

	return true;
}

static inline int inject_library(HANDLE process, const wchar_t *dll)
{
	return inject_library_obf(process, dll, "D|hkqkW`kl{k\\osofj", 0xa178ef3655e5ade7, "[uawaRzbhh{tIdkj~~",
				  0x561478dbd824387c, "[fr}pboIe`dlN}", 0x395bfbc9833590fd, "\\`zs}gmOzhhBq",
				  0x12897dd89168789a, "GbfkDaezbp~X", 0x76aff7238788f7db);
}

static inline bool hook_direct(struct game_capture *gc, const char *hook_path_rel)
{
	wchar_t hook_path_abs_w[MAX_PATH];
	wchar_t *hook_path_rel_w;
	wchar_t *path_ret;
	HANDLE process;
	int ret;

	os_utf8_to_wcs_ptr(hook_path_rel, 0, &hook_path_rel_w);
	if (!hook_path_rel_w) {
		warn("hook_direct: could not convert string");
		return false;
	}

	path_ret = _wfullpath(hook_path_abs_w, hook_path_rel_w, MAX_PATH);
	bfree(hook_path_rel_w);

	if (path_ret == NULL) {
		warn("hook_direct: could not make absolute path");
		return false;
	}

	process = open_process(PROCESS_ALL_ACCESS, false, gc->process_id);
	if (!process) {
		warn("hook_direct: could not open process: %s (%lu)", gc->config.executable, GetLastError());
		return false;
	}

	ret = inject_library(process, hook_path_abs_w);
	CloseHandle(process);

	if (ret != 0) {
		warn("hook_direct: inject failed: %d", ret);
		return false;
	}

	return true;
}

static inline bool create_inject_process(struct game_capture *gc, const char *inject_path, const char *hook_dll)
{
	wchar_t *command_line_w = malloc(4096 * sizeof(wchar_t));
	wchar_t *inject_path_w;
	wchar_t *hook_dll_w;
	bool anti_cheat = use_anticheat(gc);
	PROCESS_INFORMATION pi = {0};
	STARTUPINFO si = {0};
	bool success = false;

	char *abs_inject_path = os_get_abs_path_ptr(inject_path);
	char *abs_hook_dll = os_get_abs_path_ptr(hook_dll);

	os_utf8_to_wcs_ptr(abs_inject_path, 0, &inject_path_w);
	os_utf8_to_wcs_ptr(abs_hook_dll, 0, &hook_dll_w);

	bfree(abs_inject_path);
	bfree(abs_hook_dll);

	si.cb = sizeof(si);

	swprintf(command_line_w, 4096, L"\"%s\" \"%s\" %lu %lu", inject_path_w, hook_dll_w, (unsigned long)anti_cheat,
		 anti_cheat ? gc->thread_id : gc->process_id);

	success = !!CreateProcessW(inject_path_w, command_line_w, NULL, NULL, false, CREATE_NO_WINDOW, NULL, NULL, &si,
				   &pi);
	if (success) {
		CloseHandle(pi.hThread);
		gc->injector_process = pi.hProcess;
	} else {
		warn("Failed to create inject helper process: %lu", GetLastError());
	}

	free(command_line_w);
	bfree(inject_path_w);
	bfree(hook_dll_w);
	return success;
}

extern char *get_hook_path(bool b64);

static inline bool inject_hook(struct game_capture *gc)
{
	bool matching_architecture;
	bool success = false;
	char *inject_path;
	char *hook_path;

	if (gc->process_is_64bit) {
		inject_path = obs_module_file("inject-helper64.exe");
	} else {
		inject_path = obs_module_file("inject-helper32.exe");
	}

	hook_path = get_hook_path(gc->process_is_64bit);

	if (!check_file_integrity(gc, inject_path, "inject helper")) {
		goto cleanup;
	}
	if (!check_file_integrity(gc, hook_path, "graphics hook")) {
		goto cleanup;
	}

	info("injecting graphics hook: %s", hook_path);

#ifdef _WIN64
	matching_architecture = gc->process_is_64bit;
#else
	matching_architecture = !gc->process_is_64bit;
#endif

	if (matching_architecture && !use_anticheat(gc)) {
		info("using direct hook");
		success = hook_direct(gc, hook_path);
	} else {
		info("using helper (%s hook)", use_anticheat(gc) ? "compatibility" : "direct");
		success = create_inject_process(gc, inject_path, hook_path);
	}

cleanup:
	bfree(inject_path);
	bfree(hook_path);
	return success;
}

static const char *blacklisted_exes[] = {
	"explorer",
	"steam",
	"battle.net",
	"galaxyclient",
	"skype",
	"uplay",
	"origin",
	"devenv",
	"taskmgr",
	"chrome",
	"discord",
	"firefox",
	"systemsettings",
	"applicationframehost",
	"cmd",
	"shellexperiencehost",
	"winstore.app",
	"searchui",
	"lockapp",
	"windowsinternal.composableshell.experiences.textinput.inputapp",
	NULL,
};

static bool is_blacklisted_exe(const char *exe)
{
	char cur_exe[MAX_PATH];

	if (!exe)
		return false;

	for (const char **vals = blacklisted_exes; *vals; vals++) {
		strcpy(cur_exe, *vals);
		strcat(cur_exe, ".exe");

		if (strcmpi(cur_exe, exe) == 0)
			return true;
	}

	return false;
}

static bool target_suspended(struct game_capture *gc)
{
	return thread_is_suspended(gc->process_id, gc->thread_id);
}

static bool init_events(struct game_capture *gc);

static bool init_hook(struct game_capture *gc)
{
	struct dstr exe = {0};
	bool blacklisted_process = false;

	if (gc->config.mode == CAPTURE_MODE_ANY) {
		if (ms_get_window_exe(&exe, gc->next_window)) {
			info("attempting to hook fullscreen process: %s", exe.array);
		}
	} else {
		if (ms_get_window_exe(&exe, gc->next_window)) {
			info("attempting to hook process: %s", exe.array);
		}
	}

	blacklisted_process = is_blacklisted_exe(exe.array);
	if (blacklisted_process)
		info("cannot capture %s due to being blacklisted", exe.array);
	dstr_free(&exe);

	if (blacklisted_process) {
		return false;
	}
	if (target_suspended(gc)) {
		return false;
	}
	if (!open_target_process(gc)) {
		return false;
	}
	if (!init_keepalive(gc)) {
		return false;
	}
	if (!init_pipe(gc)) {
		return false;
	}
	if (!attempt_existing_hook(gc)) {
		if (!inject_hook(gc)) {
			return false;
		}
	}
	if (!init_texture_mutexes(gc)) {
		return false;
	}
	if (!init_hook_info(gc)) {
		return false;
	}
	if (!init_events(gc)) {
		return false;
	}

	SetEvent(gc->hook_init);

	gc->window = gc->next_window;
	gc->next_window = NULL;
	gc->active = true;
	gc->retrying = 0;
	return true;
}

static void setup_window(struct game_capture *gc, HWND window)
{
	HANDLE hook_restart;
	HANDLE process;

	GetWindowThreadProcessId(window, &gc->process_id);
	if (gc->process_id) {
		process = open_process(PROCESS_QUERY_INFORMATION, false, gc->process_id);
		if (process) {
			gc->is_app = is_app(process);
			if (gc->is_app) {
				gc->app_sid = get_app_sid(process);
			}
			CloseHandle(process);
		}
	}

	/* do not wait if we're re-hooking a process */
	hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
	if (hook_restart) {
		gc->wait_for_target_startup = false;
		CloseHandle(hook_restart);
	}

	/* otherwise if it's an unhooked process, always wait a bit for the
	 * target process to start up before starting the hook process;
	 * sometimes they have important modules to load first or other hooks
	 * (such as steam) need a little bit of time to load.  ultimately this
	 * helps prevent crashes */
	if (gc->wait_for_target_startup) {
		gc->retry_interval = 3.0f * hook_rate_to_float(gc->config.hook_rate);
		gc->wait_for_target_startup = false;
	} else {
		gc->next_window = window;
	}
}

static int rects_nearly_equal(RECT first, RECT second, long delta)
{
	return abs(first.left - second.left) <= delta && abs(first.right - second.right) <= delta &&
	       abs(first.top - second.top) <= delta && abs(first.bottom - second.bottom) <= delta;
}

static void get_fullscreen_window(struct game_capture *gc)
{
	HWND window = GetForegroundWindow();
	MONITORINFO mi = {0};
	HMONITOR monitor;
	DWORD styles;
	RECT rect;

	gc->next_window = NULL;

	if (!window) {
		return;
	}
	if (!GetWindowRect(window, &rect)) {
		return;
	}

	/* ignore regular maximized windows */
	styles = (DWORD)GetWindowLongPtr(window, GWL_STYLE);
	if ((styles & WS_MAXIMIZE) != 0 && (styles & WS_BORDER) != 0) {
		return;
	}

	monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
	if (!monitor) {
		return;
	}

	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(monitor, &mi)) {
		return;
	}

	/* Some apps extend their windows 1-2 pixels beyond the monitor's supported resolution to circumvent certain
	 * exclusive fullscreen-related issues caused either by their graphics library, Windows itself, or both. Thus,
	 * we compare the window's corner coordinates with those of the monitor using an acceptable relative margin of
	 * 2, allowing it to extend slightly past the edge of the monitor. */
	if (rects_nearly_equal(rect, mi.rcMonitor, 2)) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void get_selected_window(struct game_capture *gc)
{
	HWND window;

	if (dstr_cmpi(&gc->class, "dwm") == 0) {
		wchar_t class_w[512];
		os_utf8_to_wcs(gc->class.array, 0, class_w, 512);
		window = FindWindowW(class_w, NULL);
	} else {
		window = ms_find_window(INCLUDE_MINIMIZED, gc->priority, gc->class.array, gc->title.array,
					gc->executable.array);
	}

	if (window) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void try_hook(struct game_capture *gc)
{
	if (gc->config.mode == CAPTURE_MODE_ANY) {
		get_fullscreen_window(gc);
	} else {
		get_selected_window(gc);
	}

	if (gc->next_window) {
		gc->thread_id = GetWindowThreadProcessId(gc->next_window, &gc->process_id);

		// Make sure we never try to hook ourselves (projector)
		if (gc->process_id == GetCurrentProcessId())
			return;

		if (!gc->thread_id && gc->process_id)
			return;
		if (!gc->process_id) {
			warn("error acquiring, failed to get window "
			     "thread/process ids: %lu",
			     GetLastError());
			gc->error_acquiring = true;
			return;
		}

		if (!init_hook(gc)) {
			stop_capture(gc);
		}
	} else {
		gc->active = false;
	}
}

static inline bool init_events(struct game_capture *gc)
{
	if (!gc->hook_restart) {
		gc->hook_restart = open_event_gc(gc, EVENT_CAPTURE_RESTART);
		if (!gc->hook_restart) {
			warn("init_events: failed to get hook_restart "
			     "event: %lu",
			     GetLastError());
			return false;
		}
	}

	if (!gc->hook_stop) {
		gc->hook_stop = open_event_gc(gc, EVENT_CAPTURE_STOP);
		if (!gc->hook_stop) {
			warn("init_events: failed to get hook_stop event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_init) {
		gc->hook_init = open_event_gc(gc, EVENT_HOOK_INIT);
		if (!gc->hook_init) {
			warn("init_events: failed to get hook_init event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_ready) {
		gc->hook_ready = open_event_gc(gc, EVENT_HOOK_READY);
		if (!gc->hook_ready) {
			warn("init_events: failed to get hook_ready event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_exit) {
		gc->hook_exit = open_event_gc(gc, EVENT_HOOK_EXIT);
		if (!gc->hook_exit) {
			warn("init_events: failed to get hook_exit event: %lu", GetLastError());
			return false;
		}
	}

	return true;
}

enum capture_result { CAPTURE_FAIL, CAPTURE_RETRY, CAPTURE_SUCCESS };

static inline bool init_data_map(struct game_capture *gc, HWND window)
{
	wchar_t name[64];
	swprintf(name, 64, SHMEM_TEXTURE "_%" PRIu64 "_", (uint64_t)(uintptr_t)window);

	gc->hook_data_map = open_map_plus_id(gc, name, gc->global_hook_info->map_id);
	return !!gc->hook_data_map;
}

static inline enum capture_result init_capture_data(struct game_capture *gc)
{
	gc->cx = gc->global_hook_info->cx;
	gc->cy = gc->global_hook_info->cy;
	gc->pitch = gc->global_hook_info->pitch;

	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	CloseHandle(gc->hook_data_map);

	DWORD error = 0;
	if (!init_data_map(gc, gc->window)) {
		HWND retry_hwnd = (HWND)(uintptr_t)gc->global_hook_info->window;
		error = GetLastError();

		/* if there's an error, just override.  some windows don't play
		 * nice. */
		if (init_data_map(gc, retry_hwnd)) {
			error = 0;
		}
	}

	if (!gc->hook_data_map) {
		if (error == 2) {
			return CAPTURE_RETRY;
		} else {
			warn("init_capture_data: failed to open file "
			     "mapping: %lu",
			     error);
		}
		return CAPTURE_FAIL;
	}

	gc->data = MapViewOfFile(gc->hook_data_map, FILE_MAP_ALL_ACCESS, 0, 0, gc->global_hook_info->map_size);
	if (!gc->data) {
		warn("init_capture_data: failed to map data view: %lu", GetLastError());
		return CAPTURE_FAIL;
	}

	return CAPTURE_SUCCESS;
}

static inline uint32_t convert_5_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x1F) * (255.0 / 31.0));
}

static inline uint32_t convert_6_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x3F) * (255.0 / 63.0));
}

static void copy_b5g6r5_tex(struct game_capture *gc, int cur_texture, uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (size_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (size_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i *)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000007E0);
			__m128i green_offset = _mm_set1_epi32(0x00000008);
			__m128i red_channel_mask = _mm_set1_epi32(0x0000F800);
			__m128i red_offset = _mm_set1_epi32(0x00000300);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i *)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i *)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}

static void copy_b5g5r5a1_tex(struct game_capture *gc, int cur_texture, uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (size_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (size_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red, pixels_alpha;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i *)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000003E0);
			__m128i green_offset = _mm_set1_epi32(0x000000C);
			__m128i red_channel_mask = _mm_set1_epi32(0x00007C00);
			__m128i red_offset = _mm_set1_epi32(0x00000180);
			__m128i alpha_channel_mask = _mm_set1_epi32(0x00008000);
			__m128i alpha_offset = _mm_set1_epi32(0x00000001);
			__m128i alpha_mask32 = _mm_set1_epi32(0xFF000000);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_low, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i *)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_high, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i *)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}

static inline void copy_16bit_tex(struct game_capture *gc, int cur_texture, uint8_t *data, uint32_t pitch)
{
	if (gc->global_hook_info->format == DXGI_FORMAT_B5G5R5A1_UNORM) {
		copy_b5g5r5a1_tex(gc, cur_texture, data, pitch);

	} else if (gc->global_hook_info->format == DXGI_FORMAT_B5G6R5_UNORM) {
		copy_b5g6r5_tex(gc, cur_texture, data, pitch);
	}
}

static void copy_shmem_tex(struct game_capture *gc)
{
	int cur_texture;
	HANDLE mutex = NULL;
	uint32_t pitch;
	int next_texture;
	uint8_t *data;

	if (!gc->shmem_data)
		return;

	cur_texture = gc->shmem_data->last_tex;

	if (cur_texture < 0 || cur_texture > 1)
		return;

	next_texture = cur_texture == 1 ? 0 : 1;

	if (object_signalled(gc->texture_mutexes[cur_texture])) {
		mutex = gc->texture_mutexes[cur_texture];

	} else if (object_signalled(gc->texture_mutexes[next_texture])) {
		mutex = gc->texture_mutexes[next_texture];
		cur_texture = next_texture;

	} else {
		return;
	}

	if (gs_texture_map(gc->texture, &data, &pitch)) {
		if (gc->convert_16bit) {
			copy_16bit_tex(gc, cur_texture, data, pitch);

		} else if (pitch == gc->pitch) {
			memcpy(data, gc->texture_buffers[cur_texture], (size_t)pitch * (size_t)gc->cy);
		} else {
			uint8_t *input = gc->texture_buffers[cur_texture];
			uint32_t best_pitch = pitch < gc->pitch ? pitch : gc->pitch;

			for (size_t y = 0; y < gc->cy; y++) {
				uint8_t *line_in = input + gc->pitch * y;
				uint8_t *line_out = data + pitch * y;
				memcpy(line_out, line_in, best_pitch);
			}
		}

		gs_texture_unmap(gc->texture);
	}

	ReleaseMutex(mutex);
}

static inline bool is_16bit_format(uint32_t format)
{
	return format == DXGI_FORMAT_B5G5R5A1_UNORM || format == DXGI_FORMAT_B5G6R5_UNORM;
}

static inline bool init_shmem_capture(struct game_capture *gc)
{
	const uint32_t dxgi_format = gc->global_hook_info->format;
	const bool convert_16bit = is_16bit_format(dxgi_format);
	const enum gs_color_format format = convert_16bit ? GS_BGRA : convert_format(dxgi_format);

	obs_enter_graphics();
	gs_texrender_destroy(gc->extra_texrender);
	gc->extra_texrender = NULL;
	gs_texture_destroy(gc->extra_texture);
	gc->extra_texture = NULL;
	gs_texture_destroy(gc->texture);
	gc->texture = NULL;
	gs_texture_t *const texture = gs_texture_create(gc->cx, gc->cy, format, 1, NULL, GS_DYNAMIC);
	obs_leave_graphics();

	bool success = texture != NULL;
	if (success) {
		const bool linear_sample = format != GS_R10G10B10A2;

		gs_texrender_t *extra_texrender = NULL;
		if (!linear_sample) {
			extra_texrender = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
			success = extra_texrender != NULL;
			if (!success)
				warn("init_shmem_capture: failed to create extra texrender");
		}

		if (success) {
			gc->texture_buffers[0] = (uint8_t *)gc->data + gc->shmem_data->tex1_offset;
			gc->texture_buffers[1] = (uint8_t *)gc->data + gc->shmem_data->tex2_offset;
			gc->convert_16bit = convert_16bit;

			gc->texture = texture;
			gc->extra_texture = NULL;
			gc->extra_texrender = extra_texrender;
			gc->linear_sample = linear_sample;
			gc->copy_texture = copy_shmem_tex;
		} else {
			gs_texture_destroy(texture);
		}
	} else {
		warn("init_shmem_capture: failed to create texture");
	}

	return success;
}

static inline bool init_shtex_capture(struct game_capture *gc)
{
	obs_enter_graphics();
	gs_texrender_destroy(gc->extra_texrender);
	gc->extra_texrender = NULL;
	gs_texture_destroy(gc->extra_texture);
	gc->extra_texture = NULL;
	gs_texture_destroy(gc->texture);
	gc->texture = NULL;
	gs_texture_t *const texture = gs_texture_open_shared(gc->shtex_data->tex_handle);
	bool success = texture != NULL;
	if (success) {
		enum gs_color_format format = gs_texture_get_color_format(texture);
		const bool ten_bit_srgb = (format == GS_R10G10B10A2);
		enum gs_color_format linear_format = ten_bit_srgb ? GS_RGBA16F : gs_generalize_format(format);
		const bool linear_sample = (linear_format == format);
		gs_texture_t *extra_texture = NULL;
		gs_texrender_t *extra_texrender = NULL;
		if (!linear_sample) {
			if (ten_bit_srgb) {
				extra_texrender = gs_texrender_create(linear_format, GS_ZS_NONE);
				success = extra_texrender != NULL;
				if (!success)
					warn("init_shtex_capture: failed to create extra texrender");
			} else {
				extra_texture = gs_texture_create(gs_texture_get_width(texture),
								  gs_texture_get_height(texture), linear_format, 1,
								  NULL, 0);
				success = extra_texture != NULL;
				if (!success)
					warn("init_shtex_capture: failed to create extra texture");
			}
		}

		if (success) {
			gc->texture = texture;
			gc->linear_sample = linear_sample;
			gc->extra_texture = extra_texture;
			gc->extra_texrender = extra_texrender;
			/* frame-locked capture: open the other ring slots (none for a stock hook) */
			if (!ring_open(gc, texture)) {
				success = false;
				gs_texrender_destroy(extra_texrender);
				gc->extra_texrender = NULL;
				gs_texture_destroy(extra_texture);
				gc->extra_texture = NULL;
				gs_texture_destroy(texture);
				gc->texture = NULL;
			}
		} else {
			gs_texture_destroy(texture);
		}
	} else {
		warn("init_shtex_capture: failed to open shared handle");
	}
	obs_leave_graphics();

	return success;
}

static bool start_capture(struct game_capture *gc)
{
	debug("Starting capture");

	/* prevent from using a DLL version that's higher than current */
	if (gc->global_hook_info->hook_ver_major > HOOK_VER_MAJOR) {
		warn("cannot initialize hook, DLL hook version is "
		     "%" PRIu32 ".%" PRIu32 ", current plugin hook major version is %d.%d",
		     gc->global_hook_info->hook_ver_major, gc->global_hook_info->hook_ver_minor, HOOK_VER_MAJOR,
		     HOOK_VER_MINOR);
		return false;
	}

	if (gc->global_hook_info->type == CAPTURE_TYPE_MEMORY) {
		if (!init_shmem_capture(gc)) {
			return false;
		}

		info("memory capture successful");
	} else {
		if (!init_shtex_capture(gc)) {
			return false;
		}

		info("shared texture capture successful");
	}

	return true;
}

static inline bool capture_valid(struct game_capture *gc)
{
	if (!gc->dwm_capture && !IsWindow(gc->window))
		return false;

	return !object_signalled(gc->target_process);
}

static void check_foreground_window(struct game_capture *gc, float seconds)
{
	// Hides the cursor if the user isn't actively in the game
	gc->cursor_check_time += seconds;
	if (gc->cursor_check_time >= 0.1f) {
		DWORD foreground_process_id;
		GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process_id);
		if (gc->process_id != foreground_process_id)
			gc->cursor_hidden = true;
		else
			gc->cursor_hidden = false;
		gc->cursor_check_time = 0.0f;
	}
}

static void game_capture_tick(void *data, float seconds)
{
	struct game_capture *gc = data;
	bool deactivate = os_atomic_set_bool(&gc->deactivate_hook, false);
	bool activate_now = os_atomic_set_bool(&gc->activate_hook_now, false);

	if (activate_now) {
		HWND hwnd = (HWND)(uintptr_t)os_atomic_load_long(&gc->hotkey_window);

		if (ms_is_uwp_window(hwnd))
			hwnd = ms_get_uwp_actual_window(hwnd);

		if (ms_get_window_exe(&gc->executable, hwnd)) {
			ms_get_window_title(&gc->title, hwnd);
			ms_get_window_class(&gc->class, hwnd);

			gc->priority = WINDOW_PRIORITY_CLASS;
			gc->retry_time = 10.0f * hook_rate_to_float(gc->config.hook_rate);
			gc->activate_hook = true;
		} else {
			deactivate = false;
			activate_now = false;
		}
	} else if (deactivate) {
		gc->activate_hook = false;
	}

	if (!obs_source_showing(gc->source)) {
		if (gc->showing) {
			if (gc->active)
				stop_capture(gc);
			gc->showing = false;
		}
		return;

	} else if (!gc->showing) {
		gc->retry_time = 10.0f * hook_rate_to_float(gc->config.hook_rate);
	}

	if (gc->hook_stop && object_signalled(gc->hook_stop)) {
		debug("hook stop signal received");
		stop_capture(gc);
	}
	if (gc->active && deactivate) {
		stop_capture(gc);
	}

	if (gc->active && !gc->hook_ready && gc->process_id) {
		gc->hook_ready = open_event_gc(gc, EVENT_HOOK_READY);
	}

	if (gc->injector_process && object_signalled(gc->injector_process)) {
		DWORD exit_code = 0;

		GetExitCodeProcess(gc->injector_process, &exit_code);
		close_handle(&gc->injector_process);

		if (exit_code != 0) {
			warn("inject process failed: %ld", (long)exit_code);
			gc->error_acquiring = true;

		} else if (!gc->capturing) {
			gc->retry_interval = ERROR_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
			stop_capture(gc);
		}
	}

	if (gc->hook_ready && object_signalled(gc->hook_ready)) {
		debug("capture initializing!");
		enum capture_result result = init_capture_data(gc);

		if (result == CAPTURE_SUCCESS)
			gc->capturing = start_capture(gc);
		else
			debug("init_capture_data failed");

		// If capture was successful, send a hooked signal
		if (gc->capturing) {
			if (gc->config.mode == CAPTURE_MODE_ANY) {
				ms_get_window_exe(&gc->executable, gc->window);
				ms_get_window_title(&gc->title, gc->window);
				ms_get_window_class(&gc->class, gc->window);
			}
			signal_handler_t *sh = obs_source_get_signal_handler(gc->source);
			calldata_t data = {0};

			calldata_set_ptr(&data, "source", gc->source);
			calldata_set_string(&data, "title", gc->title.array);
			calldata_set_string(&data, "class", gc->class.array);
			calldata_set_string(&data, "executable", gc->executable.array);

			signal_handler_signal(sh, "hooked", &data);
			calldata_free(&data);

			if (gc->audio_source) {
				reconfigure_audio_source(gc->audio_source, gc->window);
			}
		}
		if (result != CAPTURE_RETRY && !gc->capturing) {
			gc->retry_interval = ERROR_RETRY_INTERVAL * hook_rate_to_float(gc->config.hook_rate);
			stop_capture(gc);
		}
	}

	gc->retry_time += seconds;

	if (!gc->active) {
		if (!gc->error_acquiring && gc->retry_time > gc->retry_interval) {
			if (gc->config.mode == CAPTURE_MODE_ANY || gc->activate_hook) {
				try_hook(gc);
				gc->retry_time = 0.0f;
			}
		}
	} else {
		if (!capture_valid(gc)) {
			info("capture window no longer exists, "
			     "terminating capture");
			stop_capture(gc);
		} else {
			if (gc->copy_texture) {
				obs_enter_graphics();
				gc->copy_texture(gc);
				obs_leave_graphics();
			}

			ring_tick(gc);

			if (gc->config.cursor) {
				DPI_AWARENESS_CONTEXT previous = NULL;
				if (gc->get_window_dpi_awareness_context != NULL) {
					const DPI_AWARENESS_CONTEXT context =
						gc->get_window_dpi_awareness_context(gc->window);
					previous = gc->set_thread_dpi_awareness_context(context);
				}

				check_foreground_window(gc, seconds);
				obs_enter_graphics();
				cursor_capture(&gc->cursor_data);
				obs_leave_graphics();

				if (previous) {
					gc->set_thread_dpi_awareness_context(previous);
				}
			}

			gc->fps_reset_time += seconds;
			if (gc->fps_reset_time >= gc->retry_interval) {
				reset_frame_interval(gc);
				gc->fps_reset_time = 0.0f;
			}
		}
	}

	flog_tick(gc);

	if (!gc->showing)
		gc->showing = true;
}

static inline void game_capture_render_cursor(struct game_capture *gc)
{
	POINT p = {0};
	HWND window;

	if (!gc->global_hook_info->cx || !gc->global_hook_info->cy)
		return;

	window = !!gc->global_hook_info->window ? (HWND)(uintptr_t)gc->global_hook_info->window : gc->window;

	DPI_AWARENESS_CONTEXT previous = NULL;
	if (gc->get_window_dpi_awareness_context != NULL) {
		const DPI_AWARENESS_CONTEXT context = gc->get_window_dpi_awareness_context(gc->window);
		previous = gc->set_thread_dpi_awareness_context(context);
	}

	ClientToScreen(window, &p);

	if (previous)
		gc->set_thread_dpi_awareness_context(previous);

	cursor_draw(&gc->cursor_data, -p.x, -p.y, gc->global_hook_info->cx, gc->global_hook_info->cy);
}

static void game_capture_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct game_capture *gc = data;
	if (!gc->texture || !gc->active)
		return;

	const bool allow_transparency = gc->config.allow_transparency;
	gs_effect_t *const effect = obs_get_base_effect(allow_transparency ? OBS_EFFECT_DEFAULT : OBS_EFFECT_OPAQUE);

	gs_texture_t *texture = gc->texture;
	enum gs_color_space source_space = GS_CS_SRGB;
	bool is_10a2_compressed = false;
	if (gs_texture_get_color_format(texture) == GS_R10G10B10A2) {
		is_10a2_compressed = true;
		source_space = gc->is_10a2_2100pq ? GS_CS_709_EXTENDED : GS_CS_SRGB_16F;
	} else if (gs_texture_get_color_format(texture) == GS_RGBA16F) {
		source_space = GS_CS_709_SCRGB;
	}

	bool linear_sample = gc->linear_sample;
	const enum gs_color_space current_space = gs_get_color_space();
	const bool texcoords_centered = obs_source_get_texcoords_centered(gc->source);
	if (!linear_sample && !texcoords_centered) {
		if (gs_texture_get_color_format(texture) == GS_R10G10B10A2) {
			gs_texrender_t *const texrender = gc->extra_texrender;
			gs_texrender_reset(texrender);
			const uint32_t cx = gs_texture_get_width(texture);
			const uint32_t cy = gs_texture_get_height(texture);
			if (gs_texrender_begin(texrender, cx, cy)) {
				gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
				gs_enable_blending(false);
				gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
				gs_eparam_t *const image = gs_effect_get_param_by_name(default_effect, "image");
				gs_effect_set_texture(image, texture);

				const char *tech_name = "DrawSrgbDecompress";
				if (gc->is_10a2_2100pq) {
					tech_name = "DrawPQ";

					const float multiplier = 10000.f / obs_get_video_sdr_white_level();
					gs_effect_set_float(gs_effect_get_param_by_name(default_effect, "multiplier"),
							    multiplier);
				}

				while (gs_effect_loop(default_effect, tech_name)) {
					gs_draw_sprite(texture, 0, 0, 0);
				}
				gs_enable_blending(true);

				gs_texrender_end(texrender);

				texture = gs_texrender_get_texture(texrender);
			}

			is_10a2_compressed = false;
		} else {
			gs_texture_t *const extra_texture = gc->extra_texture;
			if (extra_texture) {
				gs_copy_texture(extra_texture, texture);
				texture = extra_texture;
			} else {
				gs_texrender_t *const texrender = gc->extra_texrender;
				gs_texrender_reset(texrender);
				const uint32_t cx = gs_texture_get_width(texture);
				const uint32_t cy = gs_texture_get_height(texture);
				if (gs_texrender_begin(texrender, cx, cy)) {
					gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					const bool previous = gs_framebuffer_srgb_enabled();
					gs_enable_framebuffer_srgb(false);
					gs_enable_blending(false);
					gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
					gs_eparam_t *const image = gs_effect_get_param_by_name(default_effect, "image");
					gs_effect_set_texture(image, texture);
					while (gs_effect_loop(default_effect, "Draw")) {
						gs_draw_sprite(texture, 0, 0, 0);
					}
					gs_enable_blending(true);
					gs_enable_framebuffer_srgb(previous);

					gs_texrender_end(texrender);

					texture = gs_texrender_get_texture(texrender);
				}
			}
		}

		linear_sample = true;
	}

	gs_eparam_t *const image = gs_effect_get_param_by_name(effect, "image");
	const uint32_t flip = gc->global_hook_info->flip ? GS_FLIP_V : 0;

	const char *tech_name = "Draw";
	float multiplier = 1.f;
	switch (source_space) {
	case GS_CS_SRGB:
		switch (current_space) {
		case GS_CS_SRGB:
			if (allow_transparency && !linear_sample)
				tech_name = "DrawSrgbDecompress";
			break;
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
			if (!linear_sample)
				tech_name = "DrawSrgbDecompress";
			break;
		case GS_CS_709_SCRGB:
			if (linear_sample)
				tech_name = "DrawMultiply";
			else
				tech_name = "DrawSrgbDecompressMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}
		break;
	case GS_CS_SRGB_16F:
		linear_sample = true;
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
			tech_name = is_10a2_compressed ? "DrawSrgbDecompress" : "Draw";
			break;
		case GS_CS_709_SCRGB:
			tech_name = is_10a2_compressed ? "DrawSrgbDecompressMultiply" : "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}
		break;
	case GS_CS_709_EXTENDED:
		linear_sample = true;
		if (is_10a2_compressed) {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawTonemapPQ";
				multiplier = 10000.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_EXTENDED:
				tech_name = "DrawPQ";
				multiplier = 10000.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_SCRGB:
				tech_name = "DrawPQ";
				multiplier = 10000.f / 80.f;
			}
		} else {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawTonemap";
				break;
			case GS_CS_709_SCRGB:
				tech_name = "DrawMultiply";
				multiplier = obs_get_video_sdr_white_level() / 80.f;
			}
		}
		break;
	case GS_CS_709_SCRGB:
		linear_sample = true;
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			multiplier = 80.f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			multiplier = 80.f / obs_get_video_sdr_white_level();
		}
	}

	while (gs_effect_loop(effect, tech_name)) {
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(allow_transparency || linear_sample);
		gs_blend_state_push();
		gs_enable_blending(allow_transparency);
		gs_blend_function_separate(gc->config.premultiplied_alpha ? GS_BLEND_ONE : GS_BLEND_SRCALPHA,
					   GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (linear_sample)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);
		gs_draw_sprite(texture, flip, 0, 0);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
	}

	if (gc->config.cursor && !gc->cursor_hidden) {
		gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		const char *cursor_tech_name = "Draw";
		float cursor_multiplier = 1.f;
		if (current_space == GS_CS_709_SCRGB) {
			cursor_tech_name = "DrawMultiply";
			cursor_multiplier = obs_get_video_sdr_white_level() / 80.f;
		}

		const bool previous = gs_set_linear_srgb(true);

		while (gs_effect_loop(default_effect, cursor_tech_name)) {
			gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), cursor_multiplier);
			game_capture_render_cursor(gc);
		}

		gs_set_linear_srgb(previous);
	}
}

static uint32_t game_capture_width(void *data)
{
	struct game_capture *gc = data;
	return (gc->active && gc->capturing) ? gc->cx : 0;
}

static uint32_t game_capture_height(void *data)
{
	struct game_capture *gc = data;
	return (gc->active && gc->capturing) ? gc->cy : 0;
}

static const char *game_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_GAME_CAPTURE;
}

static void game_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_MODE, SETTING_MODE_ANY);
	obs_data_set_default_int(settings, SETTING_WINDOW_PRIORITY, (int)WINDOW_PRIORITY_EXE);
	obs_data_set_default_bool(settings, SETTING_COMPATIBILITY, false);
	obs_data_set_default_bool(settings, SETTING_CURSOR, true);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENCY, false);
	obs_data_set_default_bool(settings, SETTING_PREMULTIPLIED_ALPHA, false);
	obs_data_set_default_bool(settings, SETTING_LIMIT_FRAMERATE, false);
	obs_data_set_default_bool(settings, SETTING_CAPTURE_OVERLAYS, false);
	obs_data_set_default_bool(settings, SETTING_ANTI_CHEAT_HOOK, true);
	obs_data_set_default_int(settings, SETTING_HOOK_RATE, (int)HOOK_RATE_NORMAL);
	obs_data_set_default_string(settings, SETTING_RGBA10A2_SPACE, RGBA10A2_SPACE_SRGB);
	obs_data_set_default_bool(settings, SETTING_FRAME_LOG, false);
	obs_data_set_default_int(settings, SETTING_FRAME_RING, 8);
	obs_data_set_default_bool(settings, SETTING_BUFFERING, true);
	obs_data_set_default_int(settings, SETTING_BUFFER_DELAY, 2);
}

static bool mode_callback(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	bool capture_window;

	if (using_older_non_mode_format(settings)) {
		capture_window = !obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
	} else {
		const char *mode = obs_data_get_string(settings, SETTING_MODE);
		capture_window = strcmp(mode, SETTING_MODE_WINDOW) == 0;
	}

	p = obs_properties_get(ppts, SETTING_CAPTURE_WINDOW);
	obs_property_set_visible(p, capture_window);

	p = obs_properties_get(ppts, SETTING_WINDOW_PRIORITY);
	obs_property_set_visible(p, capture_window);

	return true;
}

static bool window_changed_callback(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	bool modified = ms_check_window_property_setting(ppts, p, settings, SETTING_CAPTURE_WINDOW, 1);

	bool capture_any;
	if (using_older_non_mode_format(settings)) {
		capture_any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
	} else {
		const char *mode = obs_data_get_string(settings, SETTING_MODE);
		capture_any = strcmp(mode, SETTING_MODE_ANY) == 0 || strcmp(mode, SETTING_MODE_HOTKEY) == 0;
	}

	if (capture_any)
		return modified;

	const char *window = obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);

	char *class;
	char *exe;
	char *title;
	ms_build_window_strings(window, &class, &title, &exe);
	struct compat_result *compat = check_compatibility(title, class, exe, GAME_CAPTURE);
	bfree(title);
	bfree(exe);
	bfree(class);

	obs_property_t *p_warn = obs_properties_get(ppts, SETTINGS_COMPAT_INFO);

	if (!compat) {
		modified = obs_property_visible(p_warn) || modified;
		obs_property_set_visible(p_warn, false);
		return modified;
	}

	obs_property_set_long_description(p_warn, compat->message);
	obs_property_text_set_info_type(p_warn, compat->severity);
	obs_property_set_visible(p_warn, true);

	compat_result_free(compat);

	return true;
}

static BOOL CALLBACK EnumFirstMonitor(HMONITOR monitor, HDC hdc, LPRECT rc, LPARAM data)
{
	*(HMONITOR *)data = monitor;

	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(rc);
	return false;
}

static bool window_not_blacklisted(const char *title, const char *class, const char *exe)
{
	UNUSED_PARAMETER(title);
	UNUSED_PARAMETER(class);

	return !is_blacklisted_exe(exe);
}

/* frame-locked capture, Phase 4: tell the user how much the buffer delays the
 * video and which audio sync offset matches it (the source cannot delay audio
 * it does not own, and Desktop Audio is a separate source) */
static bool framelock_changed_callback(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	struct obs_video_info ovi;
	const bool buffering = obs_data_get_bool(settings, SETTING_BUFFERING);
	int delay = (int)obs_data_get_int(settings, SETTING_BUFFER_DELAY);
	double fps = 60.0;
	char text[512];

	UNUSED_PARAMETER(ppts);
	UNUSED_PARAMETER(p);

	if (obs_get_video_info(&ovi) && ovi.fps_num && ovi.fps_den)
		fps = (double)ovi.fps_num / (double)ovi.fps_den;
	if (delay < 1)
		delay = 1;
	if (delay > LOCK_MAX_DELAY)
		delay = LOCK_MAX_DELAY;

	if (buffering) {
		const double ms = 1000.0 * delay / fps;
		snprintf(text, sizeof(text),
			 "Buffering delays the video by %.0f ms (%d frame%s at %.2f fps). To keep audio in sync, set a "
			 "Sync Offset of +%.0f ms on your audio sources (Audio Mixer > Advanced Audio Properties).",
			 ms, delay, delay == 1 ? "" : "s", fps, ms);
	} else {
		snprintf(text, sizeof(text), "Buffering is off: the video is not delayed and no audio sync offset is needed.");
	}
	obs_data_set_string(settings, SETTING_AUDIO_INFO, text);
	return true;
}

static obs_properties_t *game_capture_properties(void *data)
{
	HMONITOR monitor;
	uint32_t cx = 1920;
	uint32_t cy = 1080;

	/* scaling is free form, this is mostly just to provide some common
	 * values */
	bool success = !!EnumDisplayMonitors(NULL, NULL, EnumFirstMonitor, (LPARAM)&monitor);
	if (success) {
		MONITORINFO mi = {0};
		mi.cbSize = sizeof(mi);

		if (!!GetMonitorInfo(monitor, &mi)) {
			cx = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
			cy = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
		}
	}

	/* update from deprecated settings */
	if (data) {
		struct game_capture *gc = data;
		obs_data_t *settings = obs_source_get_settings(gc->source);
		if (using_older_non_mode_format(settings)) {
			bool any = obs_data_get_bool(settings, SETTING_ANY_FULLSCREEN);
			const char *mode = any ? SETTING_MODE_ANY : SETTING_MODE_WINDOW;

			obs_data_set_string(settings, SETTING_MODE, mode);
		}
		obs_data_release(settings);
	}

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, SETTING_MODE, TEXT_MODE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, TEXT_MODE_ANY, SETTING_MODE_ANY);
	obs_property_list_add_string(p, TEXT_MODE_WINDOW, SETTING_MODE_WINDOW);
	obs_property_list_add_string(p, TEXT_MODE_HOTKEY, SETTING_MODE_HOTKEY);

	obs_property_set_modified_callback(p, mode_callback);

	p = obs_properties_add_list(ppts, SETTING_CAPTURE_WINDOW, TEXT_WINDOW, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "", "");
	ms_fill_window_list(p, INCLUDE_MINIMIZED, window_not_blacklisted);

	obs_property_set_modified_callback(p, window_changed_callback);

	p = obs_properties_add_list(ppts, SETTING_WINDOW_PRIORITY, TEXT_MATCH_PRIORITY, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE, WINDOW_PRIORITY_EXE);

	p = obs_properties_add_text(ppts, SETTINGS_COMPAT_INFO, NULL, OBS_TEXT_INFO);
	obs_property_set_enabled(p, false);

	if (audio_capture_available()) {
		p = obs_properties_add_bool(ppts, SETTING_CAPTURE_AUDIO, TEXT_CAPTURE_AUDIO);
		obs_property_set_long_description(p, TEXT_CAPTURE_AUDIO_TT);
	}

	obs_properties_add_bool(ppts, SETTING_COMPATIBILITY, TEXT_SLI_COMPATIBILITY);

	obs_properties_add_bool(ppts, SETTING_TRANSPARENCY, TEXT_ALLOW_TRANSPARENCY);

	obs_properties_add_bool(ppts, SETTING_PREMULTIPLIED_ALPHA, TEXT_PREMULTIPLIED_ALPHA);

	obs_properties_add_bool(ppts, SETTING_LIMIT_FRAMERATE, TEXT_LIMIT_FRAMERATE);

	obs_properties_add_bool(ppts, SETTING_CURSOR, TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(ppts, SETTING_ANTI_CHEAT_HOOK, TEXT_ANTI_CHEAT_HOOK);

	obs_properties_add_bool(ppts, SETTING_CAPTURE_OVERLAYS, TEXT_CAPTURE_OVERLAYS);

	p = obs_properties_add_bool(ppts, SETTING_BUFFERING, TEXT_BUFFERING);
	obs_property_set_long_description(p, TEXT_BUFFERING_TT);
	obs_property_set_modified_callback(p, framelock_changed_callback);

	p = obs_properties_add_int_slider(ppts, SETTING_BUFFER_DELAY, TEXT_BUFFER_DELAY, 1, LOCK_MAX_DELAY, 1);
	obs_property_set_long_description(p, TEXT_BUFFER_DELAY_TT);
	obs_property_set_modified_callback(p, framelock_changed_callback);

	p = obs_properties_add_text(ppts, SETTING_AUDIO_INFO, NULL, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_NORMAL);

	p = obs_properties_add_int_slider(ppts, SETTING_FRAME_RING, TEXT_FRAME_RING, 1, FRAME_RING_MAX, 1);
	obs_property_set_long_description(p, TEXT_FRAME_RING_TT);

	p = obs_properties_add_bool(ppts, SETTING_FRAME_LOG, TEXT_FRAME_LOG);
	obs_property_set_long_description(p, TEXT_FRAME_LOG_TT);

	p = obs_properties_add_list(ppts, SETTING_HOOK_RATE, TEXT_HOOK_RATE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_SLOW, HOOK_RATE_SLOW);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_NORMAL, HOOK_RATE_NORMAL);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_FAST, HOOK_RATE_FAST);
	obs_property_list_add_int(p, TEXT_HOOK_RATE_FASTEST, HOOK_RATE_FASTEST);

	p = obs_properties_add_list(ppts, SETTING_RGBA10A2_SPACE, TEXT_RGBA10A2_SPACE, OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, TEXT_RGBA10A2_SPACE_SRGB, RGBA10A2_SPACE_SRGB);
	obs_property_list_add_string(p, TEXT_RGBA10A2_SPACE_2100PQ, RGBA10A2_SPACE_2100PQ);

	return ppts;
}

enum gs_color_space game_capture_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	enum gs_color_space capture_space = GS_CS_SRGB;

	struct game_capture *const gc = data;
	if (gc->texture) {
		const enum gs_color_format format = gs_texture_get_color_format(gc->texture);
		if (((format == GS_R10G10B10A2) && gc->is_10a2_2100pq) || (format == GS_RGBA16F)) {
			for (size_t i = 0; i < count; ++i) {
				if (preferred_spaces[i] == GS_CS_709_SCRGB)
					return GS_CS_709_SCRGB;
			}

			capture_space = GS_CS_709_EXTENDED;
		} else if (format == GS_R10G10B10A2) {
			for (size_t i = 0; i < count; ++i) {
				if (preferred_spaces[i] == GS_CS_SRGB_16F)
					return GS_CS_SRGB_16F;
			}

			capture_space = GS_CS_709_EXTENDED;
		}
	}

	enum gs_color_space space = capture_space;
	for (size_t i = 0; i < count; ++i) {
		const enum gs_color_space preferred_space = preferred_spaces[i];
		space = preferred_space;
		if (preferred_space == capture_space)
			break;
	}

	return space;
}

static void game_capture_enum(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct game_capture *gc = data;
	if (gc->audio_source)
		cb(gc->source, gc->audio_source, param);
}

struct obs_source_info game_capture_info = {
	.id = "game_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_SRGB,
	.get_name = game_capture_name,
	.create = game_capture_create,
	.destroy = game_capture_destroy,
	.get_width = game_capture_width,
	.get_height = game_capture_height,
	.get_defaults = game_capture_defaults,
	.get_properties = game_capture_properties,
	.enum_active_sources = game_capture_enum,
	.update = game_capture_update,
	.video_tick = game_capture_tick,
	.video_render = game_capture_render,
	.icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
	.video_get_color_space = game_capture_get_color_space,
};
