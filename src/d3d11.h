#pragma once

#include <d3d11_1.h>
#include <memory>

namespace d3d11
{
	struct Geometry;
	struct Effect;
	struct Texture2D;

	struct Context
	{
		Context(ID3D11DeviceContext*);
		operator ID3D11DeviceContext*() const { return ctx_.get(); }
		void flush() const;

	private:
		const std::shared_ptr<ID3D11DeviceContext> ctx_;
	};

	struct Device
	{
		Device(ID3D11Device*, ID3D11DeviceContext*);
		operator ID3D11Device*() const { return device_.get(); }
		const Context& immedidate_context() const;
		std::shared_ptr<Geometry> create_quad(float x, float y, float width, float height, bool flip = false) const;
		std::shared_ptr<Texture2D> create_texture(int width, int height, DXGI_FORMAT format,
			const void* data, size_t row_stride) const;
		std::shared_ptr<Texture2D> open_shared_texture(void*) const;
		std::shared_ptr<Texture2D> open_shared_texture_nt(void* handle) const;
		void recreate_shared_texture_nt(const wchar_t* name, void* handle, void*& previous) const;
		std::shared_ptr<Effect> create_default_effect() const;
		std::shared_ptr<Effect> create_effect(const char* vertex_code, const char* vertex_entry, const char* vertex_model,
			const char* pixel_code, const char* pixel_entry, const char* pixel_model) const;

	private:
		static std::shared_ptr<ID3DBlob> compile_shader(const char* source_code, const char* entry_point, const char* model);
		const std::shared_ptr<ID3D11Device> device_;
		const std::shared_ptr<Context> ctx_;
	};

	struct Texture2D
	{
		Texture2D(ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv);
		void bind(const Context& ctx) const;
		uint32_t width() const;
		uint32_t height() const;
		DXGI_FORMAT format() const;

		void* share_handle() const;
		void copy_from(const Context& ctx, const std::shared_ptr<Texture2D>&) const;
		void copy_from(const Context& ctx, const void* buffer, uint32_t stride, uint32_t rows) const;

	private:
		HANDLE share_handle_;
		const std::shared_ptr<ID3D11Texture2D> texture_;
		const std::shared_ptr<ID3D11ShaderResourceView> srv_;
	};

	struct Effect
	{
		Effect(ID3D11VertexShader* vsh, ID3D11PixelShader* psh, ID3D11InputLayout* layout);
		void bind(const Context& ctx) const;

	private:
		const std::shared_ptr<ID3D11VertexShader> vsh_;
		const std::shared_ptr<ID3D11PixelShader> psh_;
		const std::shared_ptr<ID3D11InputLayout> layout_;
	};

	struct Geometry
	{
		Geometry(D3D_PRIMITIVE_TOPOLOGY primitive, uint32_t vertices, uint32_t stride, ID3D11Buffer*);
		void bind(const Context& ctx) const;
		void draw(const Context& ctx) const;

	private:
		D3D_PRIMITIVE_TOPOLOGY primitive_;
		uint32_t vertices_;
		uint32_t stride_;
		const std::shared_ptr<ID3D11Buffer> buffer_;
	};

	std::shared_ptr<Device> create_device();
}