#pragma once

#ifdef _MSC_VER
/* conversion from data/function pointer */
#pragma warning(disable : 4152)
#endif

#ifdef OBS_LEGACY
#include "../graphics-hook-info.h"
#else
#include <graphics-hook-info.h>
#endif
#include <ipc-util/pipe.h>
#include <psapi.h>

#ifdef __cplusplus
extern "C" {
#else
#if defined(_MSC_VER) && !defined(inline)
#define inline __inline
#endif
#endif

#define NUM_BUFFERS 3
#define HOOK_VERBOSE_LOGGING 0

#if HOOK_VERBOSE_LOGGING
#define hlog_verbose(...) hlog(__VA_ARGS__)
#else
#define hlog_verbose(...) (void)0
#endif

extern void hlog(const char *format, ...);
extern void hlog_hr(const char *text, HRESULT hr);
static inline const char *get_process_name(void);
static inline HMODULE get_system_module(const char *module);
static inline HMODULE load_system_library(const char *module);
extern uint64_t os_gettime_ns(void);
extern uint64_t capture_init_retry_ns;

#define flog(format, ...) hlog("%s: " format, __FUNCTION__, ##__VA_ARGS__)
#define flog_hr(text, hr) hlog_hr(__FUNCTION__ ": " text, hr)

static inline bool capture_active(void);
static inline bool capture_ready(void);
static inline bool capture_should_stop(void);
static inline bool capture_should_init(void);

extern void shmem_copy_data(size_t idx, void *volatile data);
extern bool shmem_texture_data_lock(int idx);
extern void shmem_texture_data_unlock(int idx);

extern bool hook_ddraw(void);
extern bool hook_d3d8(void);
extern bool hook_d3d9(void);
extern bool hook_d3d12(void);
extern bool hook_dxgi(void);
extern bool hook_gl(void);
#ifdef COMPILE_VULKAN_HOOK
extern bool hook_vulkan(void);
#endif

extern void d3d10_capture(void *swap, void *backbuffer);
extern void d3d10_free(void);
extern void d3d11_capture(void *swap, void *backbuffer);
extern void d3d11_free(void);

#ifdef COMPILE_D3D12_HOOK
extern void d3d12_capture(void *swap, void *backbuffer);
extern void d3d12_free(void);
#endif

extern bool rehook_gl(void);

extern bool capture_init_shtex(struct shtex_data **data, HWND window, uint32_t cx, uint32_t cy, uint32_t format,
			       bool flip, uintptr_t handle);
/* frame-locked capture: same, with a ring of count shared textures (1..FRAME_RING_MAX) */
extern bool capture_init_shtex_ring(struct shtex_data **data, HWND window, uint32_t cx, uint32_t cy, uint32_t format,
				    bool flip, const uintptr_t *handles, uint32_t count);
/* frame-locked capture: slots to allocate for the next capture init, from hook_info (1..FRAME_RING_MAX) */
extern uint32_t capture_ring_size_wanted(void);
extern bool capture_init_shmem(struct shmem_data **data, HWND window, uint32_t cx, uint32_t cy, uint32_t pitch,
			       uint32_t format, bool flip);
extern void capture_free(void);

extern struct hook_info *global_hook_info;

struct vertex {
	struct {
		float x, y, z, w;
	} pos;
	struct {
		float u, v;
	} tex;
};

static inline bool duplicate_handle(HANDLE *dst, HANDLE src)
{
	return !!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), dst, 0, false, DUPLICATE_SAME_ACCESS);
}

static inline void *get_offset_addr(HMODULE module, uint32_t offset)
{
	return (void *)((uintptr_t)module + (uintptr_t)offset);
}

/* Frame-locked capture: ring slot the next frame goes into.  Without a ring this
 * is always 0.  With a ring the slots are used round-robin, except that the slot
 * OBS reports it is showing (read_slot) is stepped over, so the hook never
 * overwrites what OBS draws and never waits for it. */
extern uint32_t capture_ring_next_slot;

static inline uint32_t capture_next_slot(struct shtex_data *shtex)
{
	uint32_t slot;

	if (!shtex || shtex->ring_size <= 1)
		return 0;

	slot = capture_ring_next_slot % shtex->ring_size;
	if (slot == shtex->read_slot) {
		slot = (slot + 1) % shtex->ring_size;
		InterlockedIncrement64((volatile LONG64 *)&shtex->conflicts);
	}
	capture_ring_next_slot = (slot + 1) % shtex->ring_size;
	return slot;
}

/* Frame-locked capture: record one copied frame in the shared present log (see
 * struct shtex_data).  Called by a capture path right after it copied the frame
 * into the shared texture slot.  The record is written first and the count
 * published last with an interlocked store, so the OBS side never reads a
 * half-written record and the 64-bit count stays atomic from the 32-bit hook. */
static inline void capture_log_present(struct shtex_data *shtex, uint32_t slot)
{
	uint64_t n;
	struct present_record *rec;

	if (!shtex)
		return;

	n = shtex->present_count;
	rec = &shtex->present_log[n % PRESENT_LOG_SIZE];
	rec->time_ns = os_gettime_ns();
	rec->counter = n + 1;
	rec->slot = slot;
	InterlockedExchange64((volatile LONG64 *)&shtex->present_count, (LONG64)(n + 1));
}

/* ------------------------------------------------------------------------- */

extern ipc_pipe_client_t pipe;
extern HANDLE signal_restart;
extern HANDLE signal_stop;
extern HANDLE signal_ready;
extern HANDLE signal_exit;
extern HANDLE tex_mutexes[2];
extern char system_path[MAX_PATH];
extern char process_name[MAX_PATH];
extern wchar_t keepalive_name[64];
extern HWND dummy_window;
extern volatile bool active;

static inline const char *get_process_name(void)
{
	return process_name;
}

static inline HMODULE get_system_module(const char *module)
{
	char base_path[MAX_PATH];

	strcpy(base_path, system_path);
	strcat(base_path, "\\");
	strcat(base_path, module);
	return GetModuleHandleA(base_path);
}

static inline uint32_t module_size(HMODULE module)
{
	MODULEINFO info;
	bool success = !!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
	return success ? info.SizeOfImage : 0;
}

static inline HMODULE load_system_library(const char *name)
{
	char base_path[MAX_PATH];
	HMODULE module;

	strcpy(base_path, system_path);
	strcat(base_path, "\\");
	strcat(base_path, name);

	module = GetModuleHandleA(base_path);
	if (module)
		return module;

	return LoadLibraryA(base_path);
}

static inline bool capture_alive(void)
{
	HANDLE handle = OpenMutexW(SYNCHRONIZE, false, keepalive_name);
	const bool success = handle != NULL;
	if (success)
		CloseHandle(handle);
	return success;
}

static inline bool capture_active(void)
{
	return active;
}

static inline bool frame_ready(uint64_t interval)
{
	static uint64_t last_time = 0;
	uint64_t elapsed;
	uint64_t t;

	if (!interval) {
		return true;
	}

	t = os_gettime_ns();
	elapsed = t - last_time;

	if (elapsed < interval) {
		return false;
	}

	last_time = (elapsed > interval * 2) ? t : last_time + interval;
	return true;
}

static inline bool capture_ready(void)
{
	return capture_active() && frame_ready(global_hook_info->frame_interval);
}

static inline bool capture_stopped(void)
{
	return WaitForSingleObject(signal_stop, 0) == WAIT_OBJECT_0;
}

static inline bool capture_restarted(void)
{
	return WaitForSingleObject(signal_restart, 0) == WAIT_OBJECT_0;
}

static inline bool capture_should_stop(void)
{
	bool stop_requested = false;

	if (capture_active()) {
		static uint64_t last_keepalive_check = 0;
		uint64_t cur_time = os_gettime_ns();
		bool alive = true;

		if (cur_time - last_keepalive_check > 5000000000) {
			alive = capture_alive();
			last_keepalive_check = cur_time;
		}

		stop_requested = capture_stopped() || !alive;
	}

	return stop_requested;
}

extern bool init_pipe(void);

static inline bool capture_should_init(void)
{
	bool should_init = false;

	if (!capture_active()) {
		/* frame-locked capture: a failed init waits (capture_init_failed) */
		if (capture_init_retry_ns && os_gettime_ns() < capture_init_retry_ns)
			return false;
		if (capture_restarted()) {
			if (capture_alive()) {
				if (!ipc_pipe_client_valid(&pipe)) {
					init_pipe();
				}

				should_init = true;
			} else {
				hlog_verbose("capture_should_init: inactive, restarted, not alive");
			}
		} else {
			hlog_verbose("capture_should_init: inactive, not restarted");
		}
	}

	return should_init;
}

/* Frame-locked capture, automatic ring size: OBS asks for a different ring by
 * writing hook_info.frame_ring_size while the capture runs.  The capture path
 * then frees its capture (capture_free signals the restart) and re-creates it
 * at the new size at the same present, the path a swap chain rebuild takes. */
static inline bool capture_ring_resize_wanted(const struct shtex_data *shtex)
{
	return shtex && capture_active() && shtex->ring_size != capture_ring_size_wanted();
}

/* Frame-locked capture: after a failed capture init (video memory ran out for
 * the ring while the game rebuilt its swap chain on an alt-tab) wait half a
 * second before the next attempt instead of trying again at every present;
 * each attempt creates and destroys a device and the slots made so far. */
#define CAPTURE_INIT_RETRY_NS 500000000ULL
static inline void capture_init_failed(void)
{
	capture_init_retry_ns = os_gettime_ns() + CAPTURE_INIT_RETRY_NS;
	hlog("capture init failed; trying again in 0.5 s");
}

#if COMPILE_VULKAN_HOOK
extern __declspec(thread) int vk_presenting;
#endif

/* ------------------------------------------------------------------------- */
/* hooking in front of another hook (third-party overlay exclusion)          */

/* RTSS and the like draw their on-screen display from a hook on the same
 * present function we detour.  Whoever patched the entry last runs first,
 * so with their hook in front of ours the overlay is on the back buffer
 * before we copy it, and only a late hook (slow hook rate) used to avoid
 * that.  When a capture starts with "Capture third-party overlays" off,
 * hook_in_front_install() looks at the entry of the function we hooked: if
 * it no longer runs into our detour first, we detour it once more (Detours
 * follows the entry jump and patches whoever is in front), so `outer` runs
 * before them, copies the frame and raises captured_in_front for the rest
 * of the call; the original detour then skips its copy. */
struct hook_in_front {
	void *target; /* the function we hooked (its real entry) */
	void *inner;  /* our original detour on it */
	void *outer;  /* detour to put in front of whoever patched over ours */
	void *real;   /* trampoline for outer; NULL until installed */
	const char *name;
	bool checked; /* "first in line" already logged */
	bool failed;
};

extern __declspec(thread) int captured_in_front;

extern const void *hook_follow_jumps(const void *fn);
extern const char *hook_module_name(const void *addr, char *buf, size_t size);
extern const char *hook_module_path(const void *addr, char *buf, size_t size);
extern void hook_in_front_install(struct hook_in_front *h);

static inline bool should_passthrough()
{
#if COMPILE_VULKAN_HOOK
	return vk_presenting > 0;
#else
	return false;
#endif
}

#ifdef __cplusplus
}
#endif
