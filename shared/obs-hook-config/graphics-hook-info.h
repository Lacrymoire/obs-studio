#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "hook-helpers.h"

#define EVENT_CAPTURE_RESTART L"CaptureHook_Restart"
#define EVENT_CAPTURE_STOP L"CaptureHook_Stop"

#define EVENT_HOOK_READY L"CaptureHook_HookReady"
#define EVENT_HOOK_EXIT L"CaptureHook_Exit"

#define EVENT_HOOK_INIT L"CaptureHook_Initialize"

#define WINDOW_HOOK_KEEPALIVE L"CaptureHook_KeepAlive"

#define MUTEX_TEXTURE1 L"CaptureHook_TextureMutex1"
#define MUTEX_TEXTURE2 L"CaptureHook_TextureMutex2"

#define SHMEM_HOOK_INFO L"CaptureHook_HookInfo"
#define SHMEM_TEXTURE L"CaptureHook_Texture"

#define PIPE_NAME "CaptureHook_Pipe"

#pragma pack(push, 8)

struct d3d8_offsets {
	uint32_t present;
};

struct d3d9_offsets {
	uint32_t present;
	uint32_t present_ex;
	uint32_t present_swap;
	uint32_t d3d9_clsoff;
	uint32_t is_d3d9ex_clsoff;
};

struct d3d12_offsets {
	uint32_t execute_command_lists;
};

struct dxgi_offsets {
	uint32_t present;
	uint32_t resize;

	uint32_t present1;
};

struct dxgi_offsets2 {
	uint32_t release;
};

struct ddraw_offsets {
	uint32_t surface_create;
	uint32_t surface_restore;
	uint32_t surface_release;
	uint32_t surface_unlock;
	uint32_t surface_blt;
	uint32_t surface_flip;
	uint32_t surface_set_palette;
	uint32_t palette_set_entries;
};

struct shmem_data {
	volatile int last_tex;
	uint32_t tex1_offset;
	uint32_t tex2_offset;
};

/* Frame-locked capture.
 *
 * Phase 1: the hook records every frame it copied (counter and present time)
 * in present_log, and OBS reads it once per video tick.
 * Phase 2: instead of one shared texture the hook owns a ring of ring_size
 * shared textures ("slots").  Every present is copied into the next free slot
 * and its record says which slot holds it.  OBS tells the hook which slot it
 * is showing in read_slot; the hook steps over that slot instead of writing
 * it, so it never waits for OBS and OBS never shows a slot being written.
 * Times are os_gettime_ns() values: QueryPerformanceCounter scaled to
 * nanoseconds, the clock libobs uses for obs_get_video_frame_time(). */
#define SHTEX_LOG_VERSION 4
#define PRESENT_LOG_SIZE 256
#define FRAME_RING_MAX 32 /* Phase 8: a frame-generated stream needs about 4 x its factor */
#define FRAME_RING_NONE 0xFFFFFFFFu

struct present_record {
	uint64_t time_ns; /* when the hook copied the frame, just before the game's Present ran */
	uint64_t counter; /* 1-based count of copied frames since capture init */
	uint32_t slot;    /* ring slot that holds this frame (0 without a ring) */
	uint32_t reserved;
};

struct shtex_data {
	uint32_t tex_handle;             /* shared handle of slot 0 (the only texture without a ring) */
	uint32_t log_version;            /* SHTEX_LOG_VERSION, so OBS can tell which layout it reads */
	volatile uint64_t present_count; /* records written; record i is present_log[i % PRESENT_LOG_SIZE] */
	uint32_t ring_size;              /* valid entries in tex_handles; 1 = no ring, stock behaviour */
	volatile uint32_t read_slot;     /* written by OBS: slot it shows now, FRAME_RING_NONE = none */
	volatile uint64_t conflicts;     /* hook: presents whose next slot was read_slot (stepped over it) */
	uint32_t tex_handles[FRAME_RING_MAX];
	struct present_record present_log[PRESENT_LOG_SIZE];
};

enum capture_type {
	CAPTURE_TYPE_MEMORY,
	CAPTURE_TYPE_TEXTURE,
};

struct graphics_offsets {
	struct d3d8_offsets d3d8;
	struct d3d9_offsets d3d9;
	struct dxgi_offsets dxgi;
	struct ddraw_offsets ddraw;
	struct dxgi_offsets2 dxgi2;
	struct d3d12_offsets d3d12;
};

struct hook_info {
	/* hook version */
	uint32_t hook_ver_major;
	uint32_t hook_ver_minor;

	/* capture info */
	enum capture_type type;
	uint32_t window;
	uint32_t format;
	uint32_t cx;
	uint32_t cy;
	uint32_t UNUSED_base_cx;
	uint32_t UNUSED_base_cy;
	uint32_t pitch;
	uint32_t map_id;
	uint32_t map_size;
	bool flip;

	/* additional options */
	uint64_t frame_interval;
	bool UNUSED_use_scale;
	bool force_shmem;
	bool capture_overlay;
	bool allow_srgb_alias;

	/* hook addresses */
	struct graphics_offsets offsets;

	/* frame-locked capture: slots the hook should allocate (1 = stock single texture) */
	uint32_t frame_ring_size;

	uint32_t reserved[125];
};
static_assert(sizeof(struct hook_info) == 648, "ABI compatibility");

#pragma pack(pop)

#define GC_MAPPING_FLAGS (FILE_MAP_READ | FILE_MAP_WRITE)

static inline HANDLE create_hook_info(DWORD id)
{
	HANDLE handle = NULL;

	wchar_t new_name[64];
	const int len = swprintf(new_name, _countof(new_name), SHMEM_HOOK_INFO L"%lu", id);
	if (len > 0) {
		handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct hook_info),
					    new_name);
	}

	return handle;
}
