#include <include/cef_parser.h>

#include "composition.h"
#include "util.h"

Layer::Layer(const d3d11::Device& device, bool flip) : flip_(flip), device_(device) {}
Layer::~Layer() = default;
void Layer::attach(Composition* parent) { composition_ = parent; }
Composition* Layer::composition() const { return composition_; }
Rect Layer::bounds() const { return bounds_; }
bool Layer::active() const { return bounds_.width > 0.f; }
void Layer::sync() {}
void Layer::resize(int width, int height) {}

void Layer::move(float x, float y, float width, float height)
{
	bounds_.x = x;
	bounds_.y = y;
	bounds_.width = width;
	bounds_.height = height;
	geometry_.reset();
}

void Layer::render_texture(const d3d11::Context& ctx, d3d11::Texture2D* texture)
{
	if (!geometry_)
	{
		geometry_ = device_.create_quad(bounds_.x,
			bounds_.y, bounds_.width, bounds_.height, flip_);
	}

	if (geometry_ && texture)
	{
		if (!effect_)
		{
			effect_ = device_.create_default_effect();
		}

		geometry_->bind(ctx);
		effect_->bind(ctx);
		texture->bind(ctx);
		geometry_->draw(ctx);
	}
}

Composition::Composition(int width, int height) : width_(width), height_(height) {}

Composition::~Composition()
{
	for (const auto& layer : layers_)
	{
		layer->attach(nullptr);
	}
}

Layer* Composition::add_layer(std::shared_ptr<Layer> layer)
{
	assert(layer);
	layers_.push_back(std::move(layer));
	const auto added = layers_.back().get();
	added->attach(this);
	return added;
}

void Composition::resize(int width, int height)
{
	width_ = width;
	height_ = height;
	for (const auto& layer : layers_)
	{
		layer->resize(width, height);
	}
}

void Composition::render(const d3d11::Context& ctx) const
{
	for (const auto& layer : layers_)
	{
		if (!layer->active()) continue;
		layer->render(ctx);
	}
}
