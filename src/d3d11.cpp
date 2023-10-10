#include <array>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <iostream>

#include "d3d11.h"
#include "util.h"

#pragma comment(lib, "dxgi.lib")

namespace d3d11
{
	struct SimpleVertex
	{
		DirectX::XMFLOAT3 pos;
		DirectX::XMFLOAT2 tex;
	};

	Context::Context(ID3D11DeviceContext* ctx) : ctx_(to_com_ptr(ctx)) { }
	void Context::flush() const { ctx_->Flush(); }

	Effect::Effect(ID3D11VertexShader* vsh, ID3D11PixelShader* psh, ID3D11InputLayout* layout)
		: vsh_(to_com_ptr(vsh)), psh_(to_com_ptr(psh)), layout_(to_com_ptr(layout)) { }

	void Effect::bind(const Context& ctx) const
	{
		ID3D11DeviceContext* d3d11_ctx = ctx;
		d3d11_ctx->IASetInputLayout(layout_.get());
		d3d11_ctx->VSSetShader(vsh_.get(), nullptr, 0);
		d3d11_ctx->PSSetShader(psh_.get(), nullptr, 0);
	}

	Geometry::Geometry(
		D3D_PRIMITIVE_TOPOLOGY primitive,
		uint32_t vertices,
		uint32_t stride,
		ID3D11Buffer* buffer)
		: primitive_(primitive)
		, vertices_(vertices)
		, stride_(stride)
		, buffer_(to_com_ptr(buffer)) { }

	void Geometry::bind(const Context& ctx) const
	{
		ID3D11DeviceContext* d3d11_ctx = ctx;
		uint32_t offset = 0;
		ID3D11Buffer* buffers[1] = {buffer_.get()};
		d3d11_ctx->IASetVertexBuffers(0, 1, buffers, &stride_, &offset);
		d3d11_ctx->IASetPrimitiveTopology(primitive_);
	}

	void Geometry::draw(const Context& ctx) const
	{
		ID3D11DeviceContext* d3d11_ctx = ctx;
		d3d11_ctx->Draw(vertices_, 0);
	}

	Texture2D::Texture2D(ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv)
		: texture_(to_com_ptr(tex)), srv_(to_com_ptr(srv))
	{
		share_handle_ = nullptr;
		IDXGIResource* res = nullptr;
		if (SUCCEEDED(texture_->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&res))))
		{
			res->GetSharedHandle(&share_handle_);
			res->Release();
		}
	}

	uint32_t Texture2D::width() const
	{
		D3D11_TEXTURE2D_DESC desc;
		texture_->GetDesc(&desc);
		return desc.Width;
	}

	uint32_t Texture2D::height() const
	{
		D3D11_TEXTURE2D_DESC desc;
		texture_->GetDesc(&desc);
		return desc.Height;
	}

	DXGI_FORMAT Texture2D::format() const
	{
		D3D11_TEXTURE2D_DESC desc;
		texture_->GetDesc(&desc);
		return desc.Format;
	}

	void Texture2D::bind(const Context& ctx) const
	{
		if (srv_.get())
		{
			ID3D11DeviceContext* d3d11_ctx = ctx;
			ID3D11ShaderResourceView* views[1] = {srv_.get()};
			d3d11_ctx->PSSetShaderResources(0, 1, views);
		}
	}

	void* Texture2D::share_handle() const
	{
		return share_handle_;
	}

	void Texture2D::copy_from(const Context& ctx, const std::shared_ptr<Texture2D>& other) const
	{
		if (other)
		{
			ID3D11DeviceContext* d3d11_ctx = ctx;
			d3d11_ctx->CopyResource(texture_.get(), other->texture_.get());
		}
	}

	void Texture2D::copy_from(const Context& ctx, const void* buffer, uint32_t stride, uint32_t rows) const
	{
		if (!buffer)
		{
			return;
		}

		D3D11_MAPPED_SUBRESOURCE res;
		ID3D11DeviceContext* d3d11_ctx = ctx;
		const auto hr = d3d11_ctx->Map(texture_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
		if (SUCCEEDED(hr))
		{
			if (rows == height())
			{
				if (res.RowPitch == stride)
				{
					memcpy(res.pData, buffer, stride * rows);
				}
				else
				{
					const auto* src = (const uint8_t*)buffer;
					auto* dst = (uint8_t*)res.pData;
					auto cb = res.RowPitch < stride ? res.RowPitch : stride;
					for (auto y = 0U; y < rows; ++y)
					{
						memcpy(dst, src, cb);
						src += stride;
						dst += res.RowPitch;
					}
				}
			}
			d3d11_ctx->Unmap(texture_.get(), 0);
		}
	}

	Device::Device(ID3D11Device* pdev, ID3D11DeviceContext* pctx)
		: device_(to_com_ptr(pdev)), ctx_(std::make_shared<Context>(pctx)) { }

	const Context& Device::immedidate_context() const { return *ctx_; }

	std::shared_ptr<Geometry> Device::create_quad(float x, float y, float width, float height, bool flip) const
	{
		x = x * 2.f - 1.f;
		y = 1.f - y * 2.f;
		width = width * 2.f;
		height = height * 2.f;

		constexpr auto z = 1.f;
		SimpleVertex vertices[] = {
			{DirectX::XMFLOAT3(x, y, z), DirectX::XMFLOAT2(0.f, 0.f)},
			{DirectX::XMFLOAT3(x + width, y, z), DirectX::XMFLOAT2(1.f, 0.f)},
			{DirectX::XMFLOAT3(x, y - height, z), DirectX::XMFLOAT2(0.f, 1.f)},
			{DirectX::XMFLOAT3(x + width, y - height, z), DirectX::XMFLOAT2(1.f, 1.f)}
		};

		if (flip)
		{
			auto tmp(vertices[2].tex);
			vertices[2].tex = vertices[0].tex;
			vertices[0].tex = tmp;
			tmp = vertices[3].tex;
			vertices[3].tex = vertices[1].tex;
			vertices[1].tex = tmp;
		}

		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.ByteWidth = sizeof(SimpleVertex) * 4;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = 0;

		D3D11_SUBRESOURCE_DATA srd = {};
		srd.pSysMem = vertices;

		ID3D11Buffer* buffer = nullptr;
		const auto hr = device_->CreateBuffer(&desc, &srd, &buffer);
		if (SUCCEEDED(hr))
		{
			return std::make_shared<Geometry>(
				D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 4, static_cast<uint32_t>(sizeof(SimpleVertex)), buffer);
		}
		return nullptr;
	}

	std::shared_ptr<Texture2D> Device::open_shared_texture(void* handle) const
	{
		ID3D11Texture2D* tex = nullptr;
		auto hr = device_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void**)&tex);
		if (FAILED(hr))
		{
			return nullptr;
		}

		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);

		ID3D11ShaderResourceView* srv = nullptr;
		if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
			srv_desc.Format = td.Format;
			srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MostDetailedMip = 0;
			srv_desc.Texture2D.MipLevels = 1;

			hr = device_->CreateShaderResourceView(tex, &srv_desc, &srv);
			if (FAILED(hr))
			{
				tex->Release();
				return nullptr;
			}
		}

		return std::make_shared<Texture2D>(tex, srv);
	}

	void Device::recreate_shared_texture_nt(const wchar_t* name, void* handle, void*& previous) const
	{
		IDXGIResource1* tex = nullptr;
		ID3D11Device1* device1;
		if (FAILED(device_->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1))) return;

		{
			const auto hr = device1->OpenSharedResource1(HANDLE(handle), __uuidof(IDXGIResource1), (void**)&tex);
			device1->Release();
			if (FAILED(hr)) return;
		}

		{
			const auto hr = tex->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, name, &previous);
			tex->Release();
			if (FAILED(hr))
			{
				std::cout << "Failed to create named handle: 0x" << std::hex << hr;
				_exit(11);
			}
		}
	}

	std::shared_ptr<Texture2D> Device::open_shared_texture_nt(void* handle) const
	{
		ID3D11Device1* device1;
		if (FAILED(device_->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1))) return nullptr;

		ID3D11Texture2D* tex = nullptr;
		const auto hr = device1->OpenSharedResource1(HANDLE(handle), __uuidof(ID3D11Texture2D), (void**)&tex);
		device1->Release();
		if (FAILED(hr)) return nullptr;

		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);

		ID3D11ShaderResourceView* srv = nullptr;
		if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
			srv_desc.Format = td.Format;
			srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MostDetailedMip = 0;
			srv_desc.Texture2D.MipLevels = 1;

			if (FAILED(device_->CreateShaderResourceView(tex, &srv_desc, &srv)))
			{
				tex->Release();
				return nullptr;
			}
		}

		return std::make_shared<Texture2D>(tex, srv);
	}

	std::shared_ptr<Texture2D> Device::create_texture(int width, int height, DXGI_FORMAT format, const void* data, size_t row_stride) const
	{
		D3D11_TEXTURE2D_DESC td;
		td.ArraySize = 1;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = data ? 0 : D3D11_CPU_ACCESS_WRITE;
		td.Format = format;
		td.Width = width;
		td.Height = height;
		td.MipLevels = 1;
		td.MiscFlags = 0;
		td.SampleDesc.Count = 1;
		td.SampleDesc.Quality = 0;
		td.Usage = data ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;

		D3D11_SUBRESOURCE_DATA srd;
		srd.pSysMem = data;
		srd.SysMemPitch = static_cast<uint32_t>(row_stride);
		srd.SysMemSlicePitch = 0;

		ID3D11Texture2D* tex = nullptr;
		if (FAILED(device_->CreateTexture2D(&td, data ? &srd : nullptr, &tex)))
		{
			return nullptr;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
		srv_desc.Format = td.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MostDetailedMip = 0;
		srv_desc.Texture2D.MipLevels = 1;

		ID3D11ShaderResourceView* srv = nullptr;
		if (FAILED(device_->CreateShaderResourceView(tex, &srv_desc, &srv)))
		{
			tex->Release();
			return nullptr;
		}

		return std::make_shared<Texture2D>(tex, srv);
	}

	std::shared_ptr<ID3DBlob> Device::compile_shader(
		const char* source_code,
		const char* entry_point,
		const char* model)
	{
		static auto fnc_compile = []
		{
			typedef HRESULT (WINAPI*PFN_D3DCOMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, 
				LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
			const auto lib = LoadLibraryW(L"d3dcompiler_47.dll");
			return lib ? reinterpret_cast<PFN_D3DCOMPILE>(GetProcAddress(lib, "D3DCompile")) : nullptr;
		}();


		constexpr DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		ID3DBlob* blob = nullptr;
		ID3DBlob* blob_err = nullptr;
		const auto psrc = source_code;
		const auto len = strlen(source_code) + 1;
		const auto hr = fnc_compile(psrc, len, nullptr, nullptr, nullptr, entry_point, model, flags, 0, &blob, &blob_err);
		if (FAILED(hr))
		{
			if (blob_err)
			{
				blob_err->Release();
			}
			return nullptr;
		}

		if (blob_err)
		{
			blob_err->Release();
		}

		return std::shared_ptr<ID3DBlob>(blob, [](ID3DBlob* p) { if (p) p->Release(); });
	}

	std::shared_ptr<Effect> Device::create_default_effect() const
	{
		const auto vsh =
			R"--(
struct VS_INPUT{float4 pos:POSITION;float2 tex:TEXCOORD0;};
struct VS_OUTPUT{float4 pos:SV_POSITION;float2 tex:TEXCOORD0;};
VS_OUTPUT main(VS_INPUT input){VS_OUTPUT output;output.pos=input.pos;output.tex=input.tex;return output;})--";

		const auto psh =
			R"--(
Texture2D t0:register(t0);
SamplerState s0:register(s0);
struct VS_OUTPUT{float4 pos:SV_POSITION;float2 tex:TEXCOORD0;};
float4 main(VS_OUTPUT input):SV_Target{return t0.Sample(s0, input.tex);})--";

		return create_effect(vsh, "main", "vs_4_0",
			psh, "main", "ps_4_0");
	}

	std::shared_ptr<Effect> Device::create_effect(const char* vertex_code, const char* vertex_entry, const char* vertex_model,
		const char* pixel_code, const char* pixel_entry, const char* pixel_model) const
	{
		const auto vs_blob = compile_shader(vertex_code, vertex_entry, vertex_model);

		ID3D11VertexShader* vshdr = nullptr;
		ID3D11InputLayout* layout = nullptr;
		if (vs_blob)
		{
			device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vshdr);
			std::array<D3D11_INPUT_ELEMENT_DESC, 2> layout_desc{
				{
					{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
					{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
				}
			};
			device_->CreateInputLayout(layout_desc.data(), uint32_t(layout_desc.size()),
				vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &layout);
		}

		const auto ps_blob = compile_shader(pixel_code, pixel_entry, pixel_model);
		ID3D11PixelShader* pshdr = nullptr;
		if (ps_blob)
		{
			device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pshdr);
		}

		return std::make_shared<Effect>(vshdr, pshdr, layout);
	}

	static IDXGIAdapter* find_appropriate_adapter()
	{
		wchar_t var_data[256]{};
		if (const auto s = GetEnvironmentVariableW(L"ACCSPWB_D3D_DEVICE", var_data, 256))
		{
			const auto arg = utils::utf8(std::wstring(var_data, s));			
			if (const auto kv = utils::str_view::from_str(arg).pair(';');
				!kv.first.empty() && !kv.second.empty())
			{
				LUID id;
				id.LowPart = DWORD(kv.first.as(0ULL));
				id.HighPart = DWORD(kv.second.as(0LL));
				log_message("LUID: %d, %d", id.LowPart, id.HighPart);

				IDXGIFactory* factory;
				if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
				{
					IDXGIAdapter* adapter;
					for (auto i = 0U; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
					{
						DXGI_ADAPTER_DESC adapter_desc;
						if (SUCCEEDED(adapter->GetDesc(&adapter_desc)))
						{
							log_message("Adapter: %s, memory: %d MB (%d; %d)", utils::utf8(adapter_desc.Description).c_str(),
								adapter_desc.DedicatedVideoMemory / 1024 / 1024, adapter_desc.AdapterLuid.LowPart, adapter_desc.AdapterLuid.HighPart);
							if (adapter_desc.AdapterLuid.LowPart == id.LowPart
								&& adapter_desc.AdapterLuid.HighPart == id.HighPart)
							{
								log_message("Adapter found!");
								factory->Release();
								return adapter;
							}
						}
						adapter->Release();
					}
					factory->Release();
				}
			}
		}
		return nullptr;
	}

	std::shared_ptr<Device> create_device()
	{
		auto flags = 0U;
		#ifdef _DEBUG
		flags |= D3D11_CREATE_DEVICE_DEBUG;
		#endif

		ID3D11Device* pdev = nullptr;
		ID3D11DeviceContext* pctx = nullptr;
		const auto adapter = find_appropriate_adapter();
		const auto ret = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 
			flags, nullptr, 0U, D3D11_SDK_VERSION, &pdev, nullptr, &pctx);
		if (adapter)
		{
			adapter->Release();
		}
		if (SUCCEEDED(ret))
		{
			const auto dev = std::make_shared<Device>(pdev, pctx);
			return dev;
		}
		return nullptr;
	}
}