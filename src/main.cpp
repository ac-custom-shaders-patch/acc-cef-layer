#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "composition.h"
#include "d3d11.h"
#include "platform.h"
#include "util.h"

#include <avrt.h>
#include <mmsystem.h>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Avrt.lib")

extern "C"
{
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

uint64_t hash_code_raw(const void* data, size_t size)
{
	return XXH3_64bits(data, size);
}

struct DxCommonHelpers
{
	ID3D11SamplerState* sampler{};
	ID3D11BlendState* blender{};

	DxCommonHelpers(ID3D11Device* device)
	{
		device->CreateSamplerState(std::array<D3D11_SAMPLER_DESC, 1>{
			{
				{
					D3D11_FILTER_MIN_MAG_MIP_LINEAR,
					D3D11_TEXTURE_ADDRESS_CLAMP,
					D3D11_TEXTURE_ADDRESS_CLAMP,
					D3D11_TEXTURE_ADDRESS_CLAMP,
					0.f,
					16U,
					D3D11_COMPARISON_NEVER,
					{0.f, 0.f, 0.f, 0.f},
					0.f,
					D3D11_FLOAT32_MAX
				}
			}
		}.data(), &sampler);
		device->CreateBlendState(std::array<D3D11_BLEND_DESC, 1>{
			{
				FALSE,
				FALSE,
				{
					{
						TRUE,
						D3D11_BLEND_ONE,
						D3D11_BLEND_INV_SRC_ALPHA,
						D3D11_BLEND_OP_ADD,
						D3D11_BLEND_ONE,
						D3D11_BLEND_INV_SRC_ALPHA,
						D3D11_BLEND_OP_ADD,
						D3D11_COLOR_WRITE_ENABLE_ALL
					}
				}
			}
		}.data(), &blender);
	}

	static DxCommonHelpers& get(ID3D11Device* device)
	{
		static auto& ret = *new DxCommonHelpers(device);
		return ret;
	}
};

struct RenderTarget
{
	ID3D11RenderTargetView* rtv{};
	HANDLE shared_handle{};
	unsigned width, height;

	~RenderTarget()
	{
		if (rtv) rtv->Release();
	}

	RenderTarget(ID3D11Device* device, unsigned width, unsigned height)
		: width(width), height(height)
	{
		ID3D11Texture2D* tex{};
		device->CreateTexture2D(std::array<D3D11_TEXTURE2D_DESC, 1>{
			{
				{
					width,
					height,
					1U,
					1U,
					DXGI_FORMAT_R8G8B8A8_UNORM,
					{1U, 0U},
					D3D11_USAGE_DEFAULT,
					D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
					0,
					D3D11_RESOURCE_MISC_SHARED
				}
			}
		}.data(), nullptr, &tex);
		device->CreateRenderTargetView(tex, nullptr, &rtv);
		tex->Release();
		IDXGIResource* tex_dxgi{};
		tex->QueryInterface(__uuidof(IDXGIResource), (void**)&tex_dxgi);
		tex_dxgi->GetSharedHandle(&shared_handle);
		tex_dxgi->Release();
	}

	void bind(ID3D11Device* device, ID3D11DeviceContext* ctx)
	{
		ctx->OMSetBlendState(DxCommonHelpers::get(device).blender, std::array{0.f, 0.f, 0.f, 0.f}.data(), 0xffffffff);
		ctx->PSSetSamplers(0, 1, std::array{DxCommonHelpers::get(device).sampler}.data());
		ctx->OMSetRenderTargets(1U, &rtv, nullptr);
		ctx->RSSetViewports(1U, std::array<D3D11_VIEWPORT, 1>{{{0.f, 0.f, float(width), float(height), 0.f, 1.f}}}.data());
		ctx->ClearRenderTargetView(rtv, std::array<float, 4>{0.f, 0.f, 0.f, 0.f}.data());
	}
};

struct accsp_wb_tabs
{
	int32_t count;
	uint32_t tabs[255];
};

std::shared_ptr<Layer> create_web_layer(std::shared_ptr<accsp_mapped_typed<accsp_wb_entry>> entry, const d3d11::Device& device, bool* passthrough_mode_out, bool has_full_access);

struct WebTab
{
	uint32_t width = 320;
	uint32_t height = 240;

	const d3d11::Device& device;
	std::shared_ptr<accsp_mapped_typed<accsp_wb_entry>> mmf;
	std::unique_ptr<Composition> composition;
	std::unique_ptr<RenderTarget> rt;
	std::shared_ptr<Layer> web;
	uint64_t last_frame{};
	bool passthrough_mode{};

	WebTab(const d3d11::Device& device, const std::wstring& shared_name)
		: device(device), mmf(std::make_shared<accsp_mapped_typed<accsp_wb_entry>>(shared_name))
	{
		// TODO: Store size in a single place, not separately, to avoid occasional confusion and things going out-of-sync
		width = mmf->entry->width;
		height = mmf->entry->height;
		log_message("Creating a tab(%d, %d)", mmf->entry->width, mmf->entry->height);

		web = create_web_layer(mmf, device, &passthrough_mode, !shared_name.starts_with(L"AcTools.CSP.Limited."));
		web->set_handle_prefix(shared_name + L".T");

		composition = std::make_unique<Composition>(width, height);
		composition->add_layer(web);
		log_message("WebTab(%p, %p; %d, %d)", this, web.get(), width, height);
	}

	~WebTab()
	{		
		log_message("~WebTab(%p, %p)", this, web.get());
	}

	void update()
	{
		auto entry = mmf->entry;
		if (entry->width != width || entry->height != height)
		{
			log_message("w: %d, h: %d, fe: %d", entry->width, entry->height, entry->fe_flags);
			width = entry->width;
			height = entry->height;
			composition->resize(width, height);
		}

		if (rt)
		{
			entry->handle = uint64_t(rt->shared_handle);
		}

		web->sync();
	}

	bool render()
	{
		if (passthrough_mode) return false;
		const auto& ctx = device.immedidate_context();
		if (!rt || rt->width != width || rt->height != height)
		{
			rt = std::make_unique<RenderTarget>(device, width, height);
		}
		if (rt)
		{
			rt->bind(device, ctx);
			composition->render(ctx);
		}
		return true;
	}
};

#ifdef _DEBUG
LONG WINAPI exception_filter(__in _EXCEPTION_POINTERS*)
{
	MessageBoxW(nullptr, L"Exception", L"Fatal error", 0U);
	_exit(57);
}
#else
LONG WINAPI exception_filter(__in _EXCEPTION_POINTERS*)
{
	std::cerr << "Unhandled exception" << std::endl;
	_exit(57);
}
#endif

std::chrono::high_resolution_clock::time_point time_start = std::chrono::high_resolution_clock::now();

#define FLAG_WRITING_TABS (1 << 25)

struct CefWrapper
{
	CefWrapper(const d3d11::Device& device, const std::wstring& filename)
		: device_(device), tabs_(filename), prefix_(filename + L'.')
	{
		start_time_ = time_now_ms();
	}

	void frame()
	{
		const auto t0 = time_now_ms();
		if (tabs_->count != FLAG_WRITING_TABS)
		{
			for (auto i = 0; i < tabs_->count; ++i)
			{				
				if (auto f = windows_.find(tabs_->tabs[i]); f != windows_.end())
				{
					f->second->last_frame = frames_;
				}
				else if (!failed_.contains(tabs_->tabs[i]))
				{
					try
					{
						auto n = ((tabs_->tabs[i] & 1) != 0 ? L"AcTools.CSP.Limited.CEF.v0." : L"AcTools.CSP.CEF.v0.") + std::to_wstring(tabs_->tabs[i]);
						auto created = std::make_unique<WebTab>(device_, n);
						created->last_frame = frames_;
						windows_[tabs_->tabs[i]] = std::move(created);
						log_message("New tab: %d", tabs_->tabs[i]);
					}
					catch (std::exception& e)
					{
						std::cout << "Failed to open a tab: " << e.what() << " (" << std::to_string(tabs_->tabs[i]) << ")" << std::endl;
						failed_.insert(tabs_->tabs[i]);
					}
				}
			}
		}

		auto needs_flush = false;
		for (auto i = windows_.begin(); i != windows_.end();)
		{
			if (i->second->last_frame == frames_)
			{
				i->second->update();
				if (i->second->render())
				{
					needs_flush = true;
				}
				++i;
			}
			else
			{
				log_message("Closing tab: %d", i->first);
				i = windows_.erase(i);
			}
		}

		if (needs_flush || frames_ % 512 == 0)
		{
			device_.immedidate_context().flush();
		}

		++frames_;
		const auto c0 = time_now_ms() - t0;
		const auto t1 = time_now_ms();
		cef_step();
		const auto c1 = time_now_ms() - t1;

		ctime_app_ += c0;
		ctime_cef_ += c1;
	}

	int verify_performance(double target_frame_time_ms)
	{
		const auto expected_time_ms = double(frames_) * target_frame_time_ms;
		const auto actual_time_ms = time_now_ms() - start_time_;
		const auto timeout = 16 + int(round(expected_time_ms - actual_time_ms));
		if (frames_ % 4096 == 0)
		{
			log_message("CEF: frames=%d, avg. frame time=%.2f ms, timeout=%d ms, frame times=[ app=%.2f ms, cef=%.2f ms, sleep=%.2f ms ]", frames_,
				actual_time_ms / double(frames_), timeout, ctime_app_ / double(frames_), ctime_cef_ / double(frames_), ctime_sleep_ / double(frames_));
		}
		return timeout;
	}

	void kill_all()
	{
		const auto v = tabs_->count;
		log_message("Kill all: %d", v);
		if (v == -2) 
		{
			std::quick_exit(0);
			log_message("Killed?");
		}
		windows_.clear();
		Sleep(100);
	}

	void run_timer(double target_frame_time_ms)
	{
		static auto& wait_var = *new std::condition_variable;
		std::mutex wait_mutex;

		timeSetEvent(int(target_frame_time_ms), 5, [](UINT timer_id, UINT msg, DWORD_PTR user, DWORD_PTR dw1, DWORD_PTR dw2)
		{
			wait_var.notify_one();
		}, 0, TIME_PERIODIC);

		while (tabs_->count >= 0)
		{
			std::unique_lock<std::mutex> lock(wait_mutex);
			wait_var.wait(lock);

			frame();
			verify_performance(target_frame_time_ms);
		}
		kill_all();
	}

	void run_sleep(double target_frame_time_ms)
	{
		timeBeginPeriod(1U);
		while (tabs_->count >= 0)
		{
			// log_message("count: %d", tabs_->count);
			frame();
			const auto timeout = verify_performance(target_frame_time_ms);
			if (timeout < 4)
			{
				frame();
			}
			const auto t1 = time_now_ms();
			Sleep(std::max(4, timeout));
			const auto c1 = time_now_ms() - t1;
			ctime_sleep_ += c1;
			// log_message("count (end): %d", tabs_->count);
		}
		kill_all();
	}

	void run(bool timer_mode, int target_fps)
	{
		auto id = 0UL;
		AvSetMmThreadCharacteristicsW(L"Pro Audio", &id);

		if (timer_mode)
		{
			run_timer(1e3 / double(target_fps));
		}
		else
		{
			run_sleep(1e3 / double(target_fps));
		}
	}

private:
	std::unordered_map<uint32_t, std::unique_ptr<WebTab>> windows_;
	std::unordered_set<uint32_t> failed_;
	const d3d11::Device& device_;
	accsp_mapped_typed<accsp_wb_tabs> tabs_;
	std::wstring prefix_;
	double start_time_;
	double ctime_app_{};
	double ctime_cef_{};
	double ctime_sleep_{};
	uint64_t frames_{};
};

int main()
{	
	if (const auto exit_code = cef_initialize(GetModuleHandleW(nullptr)); exit_code >= 0)
	{
		return exit_code;
	}
	
	SetUnhandledExceptionFilter(exception_filter);
	const auto filename = get_env_value(L"ACCSPWB_KEY", L"");
	if (filename.empty())
	{
		std::cout << "Assetto Corsa CEF\n"
			"v103.0.5060.137\n\n"
			"Wraps around Chromium engine allowing Lua scripts in Assetto Corsa to\n"
			"load and render web pages. Based on OBS fork of Chromium Embedded\n"
			"Framework.\n\n"
			"Usage: use \"shared/ui/web\" library from a Lua script to create a new\n"
			"\"WebBrowser\" instance. Internals of Custom Shaders Patch will handle\n"
			"loading and managing this engine automatically." << std::endl;
		std::cin.get();
		return 1;
	}

	Sleep(50);
	try
	{
		const auto device = d3d11::create_device();
		if (!device)
		{
			throw std::exception("Failed to initialize DirectX device");
		}

		CefWrapper(*device, filename).run(
			get_env_value(L"ACCSPWB_USE_TIMER", false), get_env_value(L"ACCSPWB_TARGET_FPS", 60U));
		std::cout << "Shutting down" << std::endl;
		cef_uninitialize();
	}
	catch (const std::exception& e)
	{
		std::cerr << (strlen(e.what()) == 0 ? "Unknown error" : e.what()) << std::endl;
		return 10;
	}
	return 0;
}