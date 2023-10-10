#pragma once

#include <memory>
#include <vector>

#include "d3d11.h"
#include "util.h"

struct accsp_wb_entry;

// basic rect for floats
struct Rect
{
	float x;
	float y;
	float width;
	float height;
};

struct Composition;

struct Layer
{
	Layer(const d3d11::Device& device, bool flip);
	virtual ~Layer();
	virtual void attach(Composition*);
	virtual void move(float x, float y, float width, float height);		
	virtual void resize(int width, int height);
	virtual void render(const d3d11::Context& ctx) = 0;	
	virtual void set_handle_prefix(const std::wstring& basic_string) {}
	virtual void sync();
	Rect bounds() const;
	bool active() const;
	Composition* composition() const;

protected:
	void render_texture(const d3d11::Context& ctx, d3d11::Texture2D* texture);
	Rect bounds_{0.f, 0.f, 1.f, 1.f};
	bool flip_;
	std::shared_ptr<d3d11::Geometry> geometry_;
	std::shared_ptr<d3d11::Effect> effect_;
	const d3d11::Device& device_;

private:	
	Composition* composition_{};
};

struct Composition
{
	Composition(int width, int height);
	~Composition();
	int width() const { return width_; }
	int height() const { return height_; }
	void render(const d3d11::Context& ctx) const;	
	Layer* add_layer(std::shared_ptr<Layer> layer);
	void resize(int width, int height);

private:
	std::vector<std::shared_ptr<Layer>> layers_;
	int width_;
	int height_;
};

int cef_initialize(HINSTANCE);
void cef_step();
void cef_uninitialize();