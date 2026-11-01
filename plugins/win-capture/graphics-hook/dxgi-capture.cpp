#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <inttypes.h>

#include "graphics-hook.h"

#include <detours.h>

#if COMPILE_D3D12_HOOK
#include <d3d12.h>
#endif

typedef HRESULT(STDMETHODCALLTYPE *resize_buffers_t)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present_t)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present1_t)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

resize_buffers_t RealResizeBuffers = nullptr;
present_t RealPresent = nullptr;
present1_t RealPresent1 = nullptr;

thread_local int dxgi_presenting = 0;
struct ID3D12CommandQueue *dxgi_possible_swap_queues[8]{};
size_t dxgi_possible_swap_queue_count;
bool dxgi_present_attempted = false;

struct dxgi_swap_data {
	IDXGISwapChain *swap;
	void (*capture)(void *, void *);
	void (*free)(void);
};

static struct dxgi_swap_data data = {};

static inline IUnknown *get_dxgi_backbuffer(IDXGISwapChain *swap);

static struct hook_in_front present_front = {};
static struct hook_in_front present1_front = {};
static bool front_checked_active = false;

/* at every present the capture copied: (re)check who is in front of our
 * hooks when a capture has just started */
static inline void dxgi_front_check(void)
{
	const bool now = capture_active();
	if (now && !front_checked_active) {
		hook_in_front_install(&present_front);
		hook_in_front_install(&present1_front);
	}
	front_checked_active = now;
}

/* the copy for a present whose hook we got in front of (see graphics-hook.h) */
static inline bool dxgi_capture_in_front(IDXGISwapChain *swap, UINT flags)
{
	if (captured_in_front != 0 || global_hook_info->capture_overlay || should_passthrough())
		return false;
	if ((flags & DXGI_PRESENT_TEST) != 0 || swap != data.swap || !data.capture)
		return false;

	IUnknown *backbuffer = get_dxgi_backbuffer(swap);
	if (!backbuffer)
		return false;

	data.capture(swap, backbuffer);
	backbuffer->Release();
	return true;
}

/* Frame-locked capture, overlay exclusion in Direct3D 12 (Phase 10 follow-up).
 * The checks above look at the entry of the present function we detoured.
 * RTSS can also get in front of us without touching that entry: by putting
 * its own function into the swap chain's method table (the game, or the
 * frame-generation proxy, calls Present through that table), or by drawing
 * its OSD from its D3D12 command-queue hook before the present.  While a
 * capture runs with "Capture third-party overlays" off, look at the Present
 * and Present1 entries of the captured swap chain's table: when they no
 * longer lead to our detour, put our front function there (it copies, then
 * calls what was in the entry; the detour below skips its copy, see
 * captured_in_front).  A clean table with RTSS loaded means the OSD comes in
 * before the present, where no hook on the present can leave it out; the
 * module list logged once per capture says who is in the process. */
struct vtable_front {
	void **slot;     /* the table entry we replaced, NULL until installed */
	void *real;      /* what was in the entry (the overlay's function) */
	void *ours;      /* our front function */
	void *target;    /* the function we detoured (the stock table entry) */
	void *inner;     /* our detour on it */
	void *last_seen; /* entry seen last, for the log */
	unsigned index;  /* method index in the table */
	const char *name;
	bool checked;
	bool failed;
};

static HRESULT STDMETHODCALLTYPE hook_present_vt_front(IDXGISwapChain *swap, UINT sync_interval, UINT flags);
static HRESULT STDMETHODCALLTYPE hook_present1_vt_front(IDXGISwapChain1 *swap, UINT sync_interval, UINT flags,
							const DXGI_PRESENT_PARAMETERS *params);

static struct vtable_front vt_present = {nullptr, nullptr,  (void *)hook_present_vt_front, nullptr, nullptr,
					 nullptr, 8,        "IDXGISwapChain::Present",      false,   false};
static struct vtable_front vt_present1 = {nullptr, nullptr, (void *)hook_present1_vt_front, nullptr, nullptr,
					  nullptr, 22,      "IDXGISwapChain1::Present1",    false,   false};
static bool vt_checked_active = false;

static void dxgi_log_overlay_modules(void)
{
	static const wchar_t *const names[] = {L"RTSSHooks64.dll",          L"RTSSHooks.dll",
					       L"sl.interposer.dll",        L"sl.dlss_g.dll",
					       L"nvngx_dlssg.dll",          L"GameOverlayRenderer64.dll",
					       L"GameOverlayRenderer.dll",  L"DiscordHook64.dll",
					       L"nvspcap64.dll",            L"ReShade64.dll",
					       L"dxgi.dll",                 L"d3d12.dll",
					       L"d3d11.dll"};
	const size_t first_runtime = 10; /* dxgi.dll, d3d12.dll, d3d11.dll */
	wchar_t windir[MAX_PATH] = L"";
	const size_t windir_len = GetWindowsDirectoryW(windir, MAX_PATH);
	char list[1024] = "";

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const HMODULE mod = GetModuleHandleW(names[i]);
		if (!mod)
			continue;
		/* the runtime names are only worth a word when they come from
		 * outside the Windows folder (a proxy DLL in the game's folder,
		 * such as ReShade or a mod loader); then with their path */
		wchar_t path[MAX_PATH];
		const bool runtime = i >= first_runtime;
		if (!GetModuleFileNameW(mod, path, MAX_PATH))
			continue;
		if (runtime && windir_len && _wcsnicmp(path, windir, windir_len) == 0)
			continue;
		char narrow[MAX_PATH + 8];
		snprintf(narrow, sizeof(narrow), "%s%ls", list[0] ? ", " : "", runtime ? path : names[i]);
		strcat_s(list, narrow);
	}
	hlog("frame-lock: overlay and frame-generation modules in this process: %s", list[0] ? list : "none");
}

static void vtable_front_check(struct vtable_front *f, IDXGISwapChain *swap)
{
	char m[MAX_PATH + 1];

	if (f->failed || !f->target || !f->inner)
		return;

	void **table = *(void ***)swap;
	void *entry = table[f->index];
	const void *first = hook_follow_jumps(entry);

	if (entry == f->target || entry == f->ours || first == hook_follow_jumps(f->inner) ||
	    first == hook_follow_jumps(f->target)) {
		/* the table leads to the function we detoured, whose entry leads
		 * to our hook (or to the front we put there) */
		if (!f->checked)
			hlog("frame-lock: %s: the swap chain's method table points at the function we hooked", f->name);
		f->checked = true;
		return;
	}
	if (f->slot) {
		if (entry != f->last_seen)
			hlog("frame-lock: %s: %s replaced our entry in the swap chain's method table after the "
			     "capture started; its overlay stays in the capture",
			     f->name, hook_module_name(entry, m, sizeof(m)));
		f->last_seen = entry;
		return;
	}

	hlog("frame-lock: %s: the swap chain's method table runs %s before our hook; hooking in front of it there",
	     f->name, hook_module_path(entry, m, sizeof(m)));

	DWORD old = 0;
	if (!VirtualProtect(&table[f->index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
		hlog("frame-lock: %s: could not write the method table (%lu); overlays drawn there stay in the capture",
		     f->name, GetLastError());
		f->failed = true;
		return;
	}
	f->real = entry;
	f->slot = &table[f->index];
	f->last_seen = f->ours;
	InterlockedExchangePointer(f->slot, f->ours);
	VirtualProtect(&table[f->index], sizeof(void *), old, &old);
	f->checked = true;
	hlog("frame-lock: %s: hooked in front in the method table; third-party overlays drawn there are left out of "
	     "the capture",
	     f->name);
}

/* at every present with overlays off: the module list once per capture, the
 * table entries of the captured swap chain (a pointer compare each time, so a
 * hook that arrives after the capture started is caught too) */
static inline void dxgi_vt_front_check(IDXGISwapChain *swap)
{
	const bool now = capture_active();

	if (now && !vt_checked_active)
		dxgi_log_overlay_modules();
	vt_checked_active = now;
	if (!now || swap != data.swap)
		return;
	vtable_front_check(&vt_present, swap);
	if (vt_present1.target)
		vtable_front_check(&vt_present1, swap);
}

static HRESULT STDMETHODCALLTYPE hook_present_vt_front(IDXGISwapChain *swap, UINT sync_interval, UINT flags)
{
	const bool took = dxgi_capture_in_front(swap, flags);
	if (took)
		++captured_in_front;
	const HRESULT hr = ((present_t)vt_present.real)(swap, sync_interval, flags);
	if (took)
		--captured_in_front;
	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present1_vt_front(IDXGISwapChain1 *swap, UINT sync_interval, UINT flags,
							const DXGI_PRESENT_PARAMETERS *params)
{
	const bool took = dxgi_capture_in_front(swap, flags);
	if (took)
		++captured_in_front;
	const HRESULT hr = ((present1_t)vt_present1.real)(swap, sync_interval, flags, params);
	if (took)
		--captured_in_front;
	return hr;
}

static int swap_chain_mismatch_count = 0;
constexpr int swap_chain_mismtach_limit = 16;

static void STDMETHODCALLTYPE SwapChainDestructed(void *pData)
{
	if (pData == data.swap) {
		data.swap = nullptr;
		data.capture = nullptr;
		memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
		dxgi_possible_swap_queue_count = 0;
		dxgi_present_attempted = false;

		if (data.free) {
			data.free();
		}
		data.free = nullptr;
	}
}

static void init_swap_data(IDXGISwapChain *swap, void (*capture)(void *, void *), void (*free)(void))
{
	data.swap = swap;
	data.capture = capture;
	data.free = free;

	ID3DDestructionNotifier *notifier;
	if (SUCCEEDED(swap->QueryInterface<ID3DDestructionNotifier>(&notifier))) {
		UINT callbackID;
		notifier->RegisterDestructionCallback(&SwapChainDestructed, swap, &callbackID);
		notifier->Release();
	}
}

static bool setup_dxgi(IDXGISwapChain *swap)
{
	IUnknown *device;
	HRESULT hr;

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		ID3D11Device *d3d11 = static_cast<ID3D11Device *>(device);
		D3D_FEATURE_LEVEL level = d3d11->GetFeatureLevel();
		device->Release();

		if (level >= D3D_FEATURE_LEVEL_11_0) {
			hlog("Found D3D11 11.0 device on swap chain");

			init_swap_data(swap, d3d11_capture, d3d11_free);
			return true;
		}
	}

	hr = swap->GetDevice(__uuidof(ID3D10Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		device->Release();

		hlog("Found D3D10 device on swap chain");

		init_swap_data(swap, d3d10_capture, d3d10_free);
		return true;
	}

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		device->Release();

		hlog("Found D3D11 device on swap chain");

		init_swap_data(swap, d3d11_capture, d3d11_free);
		return true;
	}

#if COMPILE_D3D12_HOOK
	hr = swap->GetDevice(__uuidof(ID3D12Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		device->Release();

		hlog("Found D3D12 device on swap chain: swap=0x%" PRIX64 ", device=0x%" PRIX64,
		     (uint64_t)(uintptr_t)swap, (uint64_t)(uintptr_t)device);
		for (size_t i = 0; i < dxgi_possible_swap_queue_count; ++i) {
			hlog("    queue=0x%" PRIX64, (uint64_t)(uintptr_t)dxgi_possible_swap_queues[i]);
		}

		if (dxgi_possible_swap_queue_count > 0) {
			init_swap_data(swap, d3d12_capture, d3d12_free);
			return true;
		}
	}
#endif

	hlog_verbose("Failed to setup DXGI");
	return false;
}

static bool resize_buffers_called = false;

static HRESULT STDMETHODCALLTYPE hook_resize_buffers(IDXGISwapChain *swap, UINT buffer_count, UINT width, UINT height,
						     DXGI_FORMAT format, UINT flags)
{
	hlog_verbose("ResizeBuffers callback");

	data.swap = nullptr;
	data.capture = nullptr;
	memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
	dxgi_possible_swap_queue_count = 0;
	dxgi_present_attempted = false;

	if (data.free) {
		data.free();
	}
	data.free = nullptr;

	const HRESULT hr = RealResizeBuffers(swap, buffer_count, width, height, format, flags);

	resize_buffers_called = true;

	return hr;
}

static inline IUnknown *get_dxgi_backbuffer(IDXGISwapChain *swap)
{
	IUnknown *res = nullptr;

	const HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(&res));
	if (FAILED(hr)) {
		hlog_hr("get_dxgi_backbuffer: GetBuffer failed", hr);
	}

	return res;
}

static void update_mismatch_count(bool match)
{
	if (match) {
		swap_chain_mismatch_count = 0;
	} else {
		++swap_chain_mismatch_count;

		if (swap_chain_mismatch_count == swap_chain_mismtach_limit) {
			data.swap = nullptr;
			data.capture = nullptr;
			memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
			dxgi_possible_swap_queue_count = 0;
			dxgi_present_attempted = false;

			if (data.free) {
				data.free();
			}
			data.free = nullptr;

			swap_chain_mismatch_count = 0;
		}
	}
}

static HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *swap, UINT sync_interval, UINT flags)
{
	if (should_passthrough()) {
		dxgi_presenting = true;
		const HRESULT hr = RealPresent(swap, sync_interval, flags);
		dxgi_presenting = false;
		return hr;
	}

	const bool capture_overlay = global_hook_info->capture_overlay;
	const bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;

	if (data.swap) {
		update_mismatch_count(swap == data.swap);
	}

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	hlog_verbose("Present callback: sync_interval=%u, flags=%u, current_swap=0x%" PRIX64
		     ", expected_swap=0x%" PRIX64,
		     sync_interval, flags, swap, data.swap);
	const bool capture = !test_draw && swap == data.swap && data.capture;
	if (capture && !capture_overlay && captured_in_front == 0) {
		IUnknown *backbuffer = get_dxgi_backbuffer(swap);

		if (backbuffer) {
			data.capture(swap, backbuffer);
			backbuffer->Release();
		}
	}
	/* at every present, not only while capturing, so the check re-arms when
	 * a capture ends (as the D3D9 and OpenGL hooks do) */
	if (!capture_overlay && captured_in_front == 0) {
		dxgi_front_check();
		dxgi_vt_front_check(swap);
	}

	++dxgi_presenting;
	const HRESULT hr = RealPresent(swap, sync_interval, flags);
	--dxgi_presenting;
	dxgi_present_attempted = true;

	if (capture && capture_overlay) {
		/*
		 * It seems that the first call to Present after ResizeBuffers
		 * will cause the backbuffer to be invalidated, so do not
		 * perform the post-overlay capture if ResizeBuffers has
		 * recently been called.  (The backbuffer returned by
		 * get_dxgi_backbuffer *will* be invalid otherwise)
		 */
		if (resize_buffers_called) {
			resize_buffers_called = false;
		} else {
			IUnknown *backbuffer = get_dxgi_backbuffer(swap);

			if (backbuffer) {
				data.capture(swap, backbuffer);
				backbuffer->Release();
			}
		}
	}

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1 *swap, UINT sync_interval, UINT flags,
					       const DXGI_PRESENT_PARAMETERS *params)
{
	if (should_passthrough()) {
		dxgi_presenting = true;
		const HRESULT hr = RealPresent1(swap, sync_interval, flags, params);
		dxgi_presenting = false;
		return hr;
	}

	const bool capture_overlay = global_hook_info->capture_overlay;
	const bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;

	if (data.swap) {
		update_mismatch_count(swap == data.swap);
	}

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	hlog_verbose("Present1 callback: sync_interval=%u, flags=%u, current_swap=0x%" PRIX64
		     ", expected_swap=0x%" PRIX64,
		     sync_interval, flags, swap, data.swap);
	const bool capture = !test_draw && swap == data.swap && !!data.capture;
	if (capture && !capture_overlay && captured_in_front == 0) {
		IUnknown *backbuffer = get_dxgi_backbuffer(swap);

		if (backbuffer) {
			data.capture(swap, backbuffer);
			backbuffer->Release();
		}
	}
	/* at every present, not only while capturing, so the check re-arms when
	 * a capture ends (as the D3D9 and OpenGL hooks do) */
	if (!capture_overlay && captured_in_front == 0) {
		dxgi_front_check();
		dxgi_vt_front_check(swap);
	}

	++dxgi_presenting;
	const HRESULT hr = RealPresent1(swap, sync_interval, flags, params);
	--dxgi_presenting;
	dxgi_present_attempted = true;

	if (capture && capture_overlay) {
		if (resize_buffers_called) {
			resize_buffers_called = false;
		} else {
			IUnknown *backbuffer = get_dxgi_backbuffer(swap);

			if (backbuffer) {
				data.capture(swap, backbuffer);
				backbuffer->Release();
			}
		}
	}

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present_front(IDXGISwapChain *swap, UINT sync_interval, UINT flags)
{
	const bool took = dxgi_capture_in_front(swap, flags);
	if (took)
		++captured_in_front;
	const HRESULT hr = ((present_t)present_front.real)(swap, sync_interval, flags);
	if (took)
		--captured_in_front;
	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present1_front(IDXGISwapChain1 *swap, UINT sync_interval, UINT flags,
						     const DXGI_PRESENT_PARAMETERS *params)
{
	const bool took = dxgi_capture_in_front(swap, flags);
	if (took)
		++captured_in_front;
	const HRESULT hr = ((present1_t)present1_front.real)(swap, sync_interval, flags, params);
	if (took)
		--captured_in_front;
	return hr;
}

bool hook_dxgi(void)
{
	HMODULE dxgi_module = get_system_module("dxgi.dll");
	if (!dxgi_module) {
		hlog_verbose("Failed to find dxgi.dll. Skipping hook attempt.");
		return false;
	}

	/* ---------------------- */

	void *present_addr = get_offset_addr(dxgi_module, global_hook_info->offsets.dxgi.present);
	void *resize_addr = get_offset_addr(dxgi_module, global_hook_info->offsets.dxgi.resize);
	void *present1_addr = nullptr;
	if (global_hook_info->offsets.dxgi.present1) {
		present1_addr = get_offset_addr(dxgi_module, global_hook_info->offsets.dxgi.present1);
	}

	DetourTransactionBegin();

	RealPresent = (present_t)present_addr;
	DetourAttach(&(PVOID &)RealPresent, hook_present);

	RealResizeBuffers = (resize_buffers_t)resize_addr;
	DetourAttach(&(PVOID &)RealResizeBuffers, hook_resize_buffers);

	if (present1_addr) {
		RealPresent1 = (present1_t)present1_addr;
		DetourAttach(&(PVOID &)RealPresent1, hook_present1);
	}

	const LONG error = DetourTransactionCommit();
	const bool success = error == NO_ERROR;
	if (success) {
		present_front.target = present_addr;
		present_front.inner = (void *)hook_present;
		present_front.outer = (void *)hook_present_front;
		present_front.name = "IDXGISwapChain::Present";
		vt_present.target = present_addr;
		vt_present.inner = (void *)hook_present;
		if (present1_addr) {
			present1_front.target = present1_addr;
			present1_front.inner = (void *)hook_present1;
			present1_front.outer = (void *)hook_present1_front;
			present1_front.name = "IDXGISwapChain1::Present1";
			vt_present1.target = present1_addr;
			vt_present1.inner = (void *)hook_present1;
		}

		hlog("Hooked IDXGISwapChain::Present");
		hlog("Hooked IDXGISwapChain::ResizeBuffers");
		if (RealPresent1) {
			hlog("Hooked IDXGISwapChain1::Present1");
		}
		hlog("Hooked DXGI");
	} else {
		RealPresent = nullptr;
		RealResizeBuffers = nullptr;
		RealPresent1 = nullptr;
		hlog("Failed to attach Detours hook: %ld", error);
	}

	return success;
}
