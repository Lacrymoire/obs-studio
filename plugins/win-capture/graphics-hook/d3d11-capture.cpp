#include <d3d11.h>
#include <dxgi1_4.h>

#include "dxgi-helpers.hpp"
#include "graphics-hook.h"

struct d3d11_data {
	ID3D11Device *device;         /* do not release */
	ID3D11DeviceContext *context; /* do not release */
	uint32_t cx;
	uint32_t cy;
	DXGI_FORMAT format;
	bool using_shtex;
	bool multisampled;

	/* shared texture(s): frame-locked capture uses a ring of ring_size
	 * slots, one shared texture each (see struct shtex_data) */
	struct shtex_data *shtex_info;
	ID3D11Texture2D *texture[FRAME_RING_MAX];
	HANDLE handle[FRAME_RING_MAX];
	uint32_t ring_size;

	/* shared memory */
	ID3D11Texture2D *copy_surfaces[NUM_BUFFERS];
	bool texture_ready[NUM_BUFFERS];
	bool texture_mapped[NUM_BUFFERS];
	uint32_t pitch;
	struct shmem_data *shmem_info;
	int cur_tex;
	int copy_wait;
};

static struct d3d11_data data = {};

/* true once d3d11_init() finished: slot 0 of the ring exists, or in shared
 * memory mode the last copy surface (the stock code read the latter through
 * a union alias of the handle) */
static inline bool d3d11_initialized(void)
{
	return data.using_shtex ? data.handle[0] != nullptr : data.copy_surfaces[NUM_BUFFERS - 1] != nullptr;
}

void d3d11_free(void)
{
	capture_free();

	if (data.using_shtex) {
		for (uint32_t i = 0; i < FRAME_RING_MAX; i++) {
			if (data.texture[i]) {
				data.texture[i]->Release();
			}
		}
	} else {
		for (size_t i = 0; i < NUM_BUFFERS; i++) {
			if (data.copy_surfaces[i]) {
				if (data.texture_mapped[i]) {
					data.context->Unmap(data.copy_surfaces[i], 0);
				}
				data.copy_surfaces[i]->Release();
			}
		}
	}

	memset(&data, 0, sizeof(data));

	hlog("----------------- d3d11 capture freed ----------------");
}

static bool create_d3d11_stage_surface(ID3D11Texture2D **tex)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = data.cx;
	desc.Height = data.cy;
	desc.Format = data.format;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = data.device->CreateTexture2D(&desc, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d11_stage_surface: failed to create texture", hr);
		return false;
	}

	return true;
}

static bool create_d3d11_tex(uint32_t cx, uint32_t cy, ID3D11Texture2D **tex, HANDLE *handle)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = cx;
	desc.Height = cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = apply_dxgi_format_typeless(data.format, global_hook_info->allow_srgb_alias);
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	hr = data.device->CreateTexture2D(&desc, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d11_tex: failed to create texture", hr);
		return false;
	}

	if (!!handle) {
		IDXGIResource *dxgi_res;
		hr = (*tex)->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgi_res);
		if (FAILED(hr)) {
			hlog_hr("create_d3d11_tex: failed to query "
				"IDXGIResource interface from texture",
				hr);
			return false;
		}

		hr = dxgi_res->GetSharedHandle(handle);
		dxgi_res->Release();
		if (FAILED(hr)) {
			hlog_hr("create_d3d11_tex: failed to get shared handle", hr);
			return false;
		}
	}

	return true;
}

static inline bool d3d11_init_format(IDXGISwapChain *swap, HWND &window)
{
	DXGI_SWAP_CHAIN_DESC desc;
	HRESULT hr;

	hr = swap->GetDesc(&desc);
	if (FAILED(hr)) {
		hlog_hr("d3d11_init_format: swap->GetDesc failed", hr);
		return false;
	}

	print_swap_desc(&desc);

	data.format = strip_dxgi_format_srgb(desc.BufferDesc.Format);
	data.multisampled = desc.SampleDesc.Count > 1;
	window = desc.OutputWindow;
	data.cx = desc.BufferDesc.Width;
	data.cy = desc.BufferDesc.Height;

	return true;
}

static bool d3d11_shmem_init_buffers(size_t idx)
{
	bool success;

	success = create_d3d11_stage_surface(&data.copy_surfaces[idx]);
	if (!success) {
		hlog("d3d11_shmem_init_buffers: failed to create copy surface");
		return false;
	}

	if (idx == 0) {
		D3D11_MAPPED_SUBRESOURCE map = {};
		HRESULT hr;

		hr = data.context->Map(data.copy_surfaces[idx], 0, D3D11_MAP_READ, 0, &map);
		if (FAILED(hr)) {
			hlog_hr("d3d11_shmem_init_buffers: failed to get "
				"pitch",
				hr);
			return false;
		}

		data.pitch = map.RowPitch;
		data.context->Unmap(data.copy_surfaces[idx], 0);
	}

	return true;
}

static bool d3d11_shmem_init(HWND window)
{
	data.using_shtex = false;

	for (size_t i = 0; i < NUM_BUFFERS; i++) {
		if (!d3d11_shmem_init_buffers(i)) {
			return false;
		}
	}
	if (!capture_init_shmem(&data.shmem_info, window, data.cx, data.cy, data.pitch, data.format, false)) {
		return false;
	}

	hlog("d3d11 memory capture successful");
	return true;
}

/* Frame-locked capture: this process' video memory as Windows accounts it
 * (see d3d12_log_video_memory) */
static void d3d11_log_video_memory(const char *when)
{
	IDXGIDevice *dxgi_device = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIAdapter3 *adapter3 = nullptr;
	DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};

	if (!data.device || FAILED(data.device->QueryInterface(IID_PPV_ARGS(&dxgi_device))))
		return;
	HRESULT hr = dxgi_device->GetAdapter(&adapter);
	dxgi_device->Release();
	if (FAILED(hr))
		return;
	hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
	adapter->Release();
	if (FAILED(hr))
		return;
	if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
	    SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
		hlog("video memory (%s): the game has %llu MB resident of a %llu MB budget on the GPU, %llu MB of "
		     "%llu MB in system memory",
		     when, (unsigned long long)(local.CurrentUsage >> 20), (unsigned long long)(local.Budget >> 20),
		     (unsigned long long)(shared.CurrentUsage >> 20), (unsigned long long)(shared.Budget >> 20));
	adapter3->Release();
}

static bool d3d11_shtex_init(HWND window)
{
	uintptr_t handles[FRAME_RING_MAX];

	data.using_shtex = true;
	data.ring_size = capture_ring_size_wanted();

	/* frame-locked capture: one shared texture per ring slot */
	for (uint32_t i = 0; i < data.ring_size; i++) {
		if (!create_d3d11_tex(data.cx, data.cy, &data.texture[i], &data.handle[i])) {
			hlog("d3d11_shtex_init: failed to create texture (slot %u of %u)", i, data.ring_size);
			return false;
		}
		handles[i] = (uintptr_t)data.handle[i];
	}
	if (!capture_init_shtex_ring(&data.shtex_info, window, data.cx, data.cy, data.format, false, handles,
				     data.ring_size)) {
		return false;
	}

	hlog("d3d11 shared texture capture successful (%u slot%s)", data.ring_size, data.ring_size == 1 ? "" : "s");
	d3d11_log_video_memory("ring created");
	return true;
}

static void d3d11_init(IDXGISwapChain *swap)
{
	HWND window;
	HRESULT hr;

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&data.device);
	if (FAILED(hr)) {
		hlog_hr("d3d11_init: failed to get device from swap", hr);
		return;
	}

	data.device->Release();

	data.device->GetImmediateContext(&data.context);
	data.context->Release();

	if (!d3d11_init_format(swap, window)) {
		return;
	}

	const bool success = global_hook_info->force_shmem ? d3d11_shmem_init(window) : d3d11_shtex_init(window);
	if (!success) {
		d3d11_log_video_memory("capture init failed");
		d3d11_free();
		capture_init_failed();
	}
}

static inline void d3d11_copy_texture(ID3D11Resource *dst, ID3D11Resource *src)
{
	if (data.multisampled) {
		data.context->ResolveSubresource(dst, 0, src, 0, data.format);
	} else {
		data.context->CopyResource(dst, src);
	}
}

static inline void d3d11_shtex_capture(ID3D11Resource *backbuffer)
{
	if (!data.texture[0]) {
		return;
	}

	/* frame-locked capture: copy this present into the next free ring slot
	 * and record (counter, present time, slot) in the shared present log,
	 * exactly as the D3D9 path does */
	const uint32_t slot = capture_next_slot(data.shtex_info);
	d3d11_copy_texture(data.texture[slot], backbuffer);
	capture_log_present(data.shtex_info, slot);
}

static void d3d11_shmem_capture_copy(int i)
{
	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hr;

	if (data.texture_ready[i]) {
		data.texture_ready[i] = false;

		hr = data.context->Map(data.copy_surfaces[i], 0, D3D11_MAP_READ, 0, &map);
		if (SUCCEEDED(hr)) {
			data.texture_mapped[i] = true;
			shmem_copy_data(i, map.pData);
		}
	}
}

static inline void d3d11_shmem_capture(ID3D11Resource *backbuffer)
{
	int next_tex;

	next_tex = (data.cur_tex + 1) % NUM_BUFFERS;
	d3d11_shmem_capture_copy(next_tex);

	if (data.copy_wait < NUM_BUFFERS - 1) {
		data.copy_wait++;
	} else {
		if (shmem_texture_data_lock(data.cur_tex)) {
			data.context->Unmap(data.copy_surfaces[data.cur_tex], 0);
			data.texture_mapped[data.cur_tex] = false;
			shmem_texture_data_unlock(data.cur_tex);
		}

		d3d11_copy_texture(data.copy_surfaces[data.cur_tex], backbuffer);
		data.texture_ready[data.cur_tex] = true;
	}

	data.cur_tex = next_tex;
}

void d3d11_capture(void *swap_ptr, void *backbuffer_ptr)
{
	IDXGIResource *dxgi_backbuffer = (IDXGIResource *)backbuffer_ptr;
	IDXGISwapChain *swap = (IDXGISwapChain *)swap_ptr;

	HRESULT hr;
	if (capture_should_stop() || capture_ring_resize_wanted(data.shtex_info)) {
		d3d11_free();
	}
	if (capture_should_init()) {
		d3d11_init(swap);
	}
	if (d3d11_initialized() && capture_ready()) {
		ID3D11Resource *backbuffer;

		hr = dxgi_backbuffer->QueryInterface(__uuidof(ID3D11Resource), (void **)&backbuffer);
		if (FAILED(hr)) {
			hlog_hr("d3d11_shtex_capture: failed to get "
				"backbuffer",
				hr);
			return;
		}

		if (data.using_shtex) {
			d3d11_shtex_capture(backbuffer);
		} else {
			d3d11_shmem_capture(backbuffer);
		}

		backbuffer->Release();
	}
}
