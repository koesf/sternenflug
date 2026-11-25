#include "renderer.hpp"
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sf {
namespace {
constexpr size_t maxVertices = 786432;
const char *effectSource = R"EFFECT(
uniform float4x4 ViewProj;
struct VertexInput {
    float4 position : POSITION;
    float3 shape : NORMAL;
    float2 pixel : TEXCOORD0;
    float4 ends : TEXCOORD1;
    float4 tint : TEXCOORD2;
};
struct PixelInput {
    float4 position : POSITION;
    float3 shape : NORMAL;
    float2 pixel : TEXCOORD0;
    float4 ends : TEXCOORD1;
    float4 tint : TEXCOORD2;
};
PixelInput VSMain(VertexInput v) {
    PixelInput o;
    o.position = mul(float4(v.position.xyz, 1.0), ViewProj);
    o.shape = v.shape; o.pixel = v.pixel; o.ends = v.ends; o.tint = v.tint;
    return o;
}
float4 PSMain(PixelInput p) : TARGET {
    float2 delta = p.ends.zw - p.ends.xy;
    float along = saturate(dot(p.pixel - p.ends.xy, delta) / max(dot(delta, delta), 0.0001));
    float distanceToLine = length(p.pixel - (p.ends.xy + along * delta));
    float tail = p.shape.z > 1.5 ? pow(max(1.0 - along, 0.0), p.shape.y) : 1.0;
    float radius = max(0.3, p.shape.x * (p.shape.z > 1.5 ? 1.0 - 0.8 * along : 1.0));
    float core = 1.0 - smoothstep(max(0.0, radius - 0.75), radius + 0.75, distanceToLine);
    float halo = exp(-distanceToLine * distanceToLine / max(radius * radius * 5.0, 1.0));
    float alpha = saturate((0.86 * core + 0.14 * halo) * p.tint.a * tail);
    return float4(p.tint.rgb * alpha, alpha);
}
technique Draw {
    pass { vertex_shader = VSMain(v); pixel_shader = PSMain(p); }
}
)EFFECT";

struct Segment { Point a, b; bool endOfStroke = false; };
struct Shape {
    std::weak_ptr<const Drawing> owner;
    std::vector<Segment> segments;
};

Shape prepareShape(const std::shared_ptr<const Drawing> &drawing)
{
    Shape out; out.owner = drawing;
    double x0 = 1e6, y0 = 1e6, x1 = -1e6, y1 = -1e6;
    for (const auto &stroke : drawing->strokes) for (const auto &p : stroke) {
        x0 = std::min(x0, p.x * 16.0 / 9.0); x1 = std::max(x1, p.x * 16.0 / 9.0);
        y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
    }
    const double scale = std::max({x1 - x0, y1 - y0, 0.001}) * 0.5;
    const double cx = (x0 + x1) * .5, cy = (y0 + y1) * .5;
    auto point = [&](const Point &p) { return Point{(p.x * 16.0 / 9.0 - cx) / scale, (p.y - cy) / scale}; };
    size_t pointCount = 0;
    for (const auto &stroke : drawing->strokes) pointCount += stroke.size();
    // Arc sampling bounds rendering cost while retaining corners as far as possible.
    const size_t stride = std::max<size_t>(1, (pointCount + 383) / 384);
    for (const auto &stroke : drawing->strokes) {
        if (stroke.empty()) continue;
        if (stroke.size() == 1) out.segments.push_back({point(stroke[0]), point(stroke[0]), true});
        for (size_t i = 0; i + 1 < stroke.size(); i += stride) {
            const size_t j = std::min(i + stride, stroke.size() - 1);
            out.segments.push_back({point(stroke[i]), point(stroke[j]), j == stroke.size() - 1});
        }
    }
    return out;
}
}

struct Renderer::Impl {
    gs_effect_t *effect = nullptr;
    gs_vertbuffer_t *buffer = nullptr;
    size_t capacity = 0, count = 0;
    std::string failure;
    std::vector<vec3> positions, shapes;
    std::vector<vec2> pixels;
    std::vector<vec4> endpoints, tints;
    std::unordered_map<const Drawing *, Shape> cache;

    bool reserve(size_t desired) {
        if (desired <= capacity) return true;
        if (buffer) { gs_vertexbuffer_destroy(buffer); buffer = nullptr; }
        capacity = std::min(maxVertices, std::max<size_t>(4096, desired + desired / 4));
        auto *data = gs_vbdata_create();
        data->num = capacity;
        data->points = static_cast<vec3 *>(bmalloc(capacity * sizeof(vec3)));
        data->normals = static_cast<vec3 *>(bmalloc(capacity * sizeof(vec3)));
        data->num_tex = 3;
        data->tvarray = static_cast<gs_tvertarray *>(bzalloc(3 * sizeof(gs_tvertarray)));
        data->tvarray[0].width = 2; data->tvarray[0].array = bmalloc(capacity * sizeof(vec2));
        data->tvarray[1].width = 4; data->tvarray[1].array = bmalloc(capacity * sizeof(vec4));
        data->tvarray[2].width = 4; data->tvarray[2].array = bmalloc(capacity * sizeof(vec4));
        buffer = gs_vertexbuffer_create(data, GS_DYNAMIC);
        if (!buffer) { capacity = 0; failure = "Vertex buffer creation failed"; return false; }
        return true;
    }

    void segment(double ax, double ay, double bx, double by, double radius,
                 const Color &color, double opacity, double type, double falloff,
                 unsigned width, unsigned height) {
        if (positions.size() + 6 > maxVertices || opacity <= .0001 || !std::isfinite(ax + ay + bx + by + radius)) return;
        radius = std::clamp(radius, .15, 256.0);
        const double expand = radius * 5.0 + 2.0;
        const float x0 = static_cast<float>(std::min(ax, bx) - expand);
        const float y0 = static_cast<float>(std::min(ay, by) - expand);
        const float x1 = static_cast<float>(std::max(ax, bx) + expand);
        const float y1 = static_cast<float>(std::max(ay, by) + expand);
        if (x1 < 0 || y1 < 0 || x0 > width || y0 > height) return;
        const vec4 ends{{static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(bx), static_cast<float>(by)}};
        const vec4 tint{{static_cast<float>(std::clamp(color.r, 0.0, 1.0)),
                         static_cast<float>(std::clamp(color.g, 0.0, 1.0)),
                         static_cast<float>(std::clamp(color.b, 0.0, 1.0)),
                         static_cast<float>(std::clamp(opacity, 0.0, 1.0))}};
        const float x[6]{x0,x1,x0,x0,x1,x1}, y[6]{y0,y0,y1,y1,y0,y1};
        for (int v = 0; v < 6; ++v) {
            vec3 p; vec3_set(&p, x[v], y[v], 0); positions.push_back(p);
            vec3 s; vec3_set(&s, static_cast<float>(radius), static_cast<float>(falloff), static_cast<float>(type)); shapes.push_back(s);
            vec2 uv; vec2_set(&uv, x[v], y[v]); pixels.push_back(uv);
            endpoints.push_back(ends); tints.push_back(tint);
        }
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { if (impl_->buffer) gs_vertexbuffer_destroy(impl_->buffer); if (impl_->effect) gs_effect_destroy(impl_->effect); }
bool Renderer::initialize() {
    if (impl_->effect) return true;
    char *errors = nullptr;
    impl_->effect = gs_effect_create(effectSource, "sternenflug-vector.effect", &errors);
    if (errors) { impl_->failure = errors; bfree(errors); }
    return impl_->effect != nullptr;
}
const std::string &Renderer::error() const { return impl_->failure; }
size_t Renderer::vertexCount() const { return impl_->count; }

bool Renderer::render(const std::vector<RenderParticle> &particles, unsigned width, unsigned height) {
    auto &d = *impl_;
    if (!width || !height || !initialize()) return false;
    d.positions.clear(); d.shapes.clear(); d.pixels.clear(); d.endpoints.clear(); d.tints.clear();
    for (auto it = d.cache.begin(); it != d.cache.end();) {
        if (it->second.owner.expired()) it = d.cache.erase(it); else ++it;
    }
    const double scale = height / 1080.0;
    const size_t symbols = std::count_if(particles.begin(), particles.end(), [](const auto &p){return bool(p.drawing);});
    const size_t perSymbol = std::max<size_t>(8, (maxVertices/6 - std::min(particles.size()*2, maxVertices/12)) / std::max<size_t>(1,symbols));
    for (const auto &particle : particles) {
        const double x = particle.center.x * width, y = particle.center.y * height;
        const double radius = particle.radiusPixels * scale;
        if (particle.trailStrength > .0001) d.segment(x, y, particle.trailEnd.x * width,
            particle.trailEnd.y * height, particle.drawing ? std::max(.5,scale) : std::max(.5,radius*.45), particle.color,
            particle.fade * particle.trailStrength * .6, 2, particle.trailFalloff, width, height);
        if (!particle.drawing) {
            d.segment(x,y,x,y,std::max(.35,radius),particle.color,particle.fade,0,1,width,height);
            continue;
        }
        auto it = d.cache.find(particle.drawing.get());
        if (it == d.cache.end()) it = d.cache.emplace(particle.drawing.get(), prepareShape(particle.drawing)).first;
        const double lineWidth = std::clamp(radius * .014, .6, 12.0 * scale);
        const auto &segments = it->second.segments;
        const size_t stride = std::max<size_t>(1,(segments.size()+perSymbol-1)/perSymbol);
        for (size_t i=0;i<segments.size();) {
            size_t j=i;
            while (j+1<segments.size() && j-i+1<stride && !segments[j].endOfStroke) ++j;
            d.segment(x+segments[i].a.x*radius, y+segments[i].a.y*radius,
                      x+segments[j].b.x*radius, y+segments[j].b.y*radius,
                      lineWidth,particle.color,particle.fade,1,1,width,height);
            i=j+1;
        }
    }
    d.count = d.positions.size();
    if (!d.count) return true;
    if (!d.reserve(d.count)) return false;
    gs_tvertarray arrays[3]{{2,d.pixels.data()},{4,d.endpoints.data()},{4,d.tints.data()}};
    gs_vb_data upload{}; upload.num=d.count; upload.points=d.positions.data(); upload.normals=d.shapes.data();
    upload.num_tex=3; upload.tvarray=arrays;
    // D3D11 copies the buffer's stored num, not upload.num. The allocation may
    // be larger than this frame, so restrict the transfer to initialized data.
    auto *stored = gs_vertexbuffer_get_data(d.buffer);
    const size_t allocated = stored->num;
    stored->num = d.count;
    gs_vertexbuffer_flush_direct(d.buffer,&upload);
    stored->num = allocated;
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_load_vertexbuffer(d.buffer); gs_load_indexbuffer(nullptr);
    while (gs_effect_loop(d.effect,"Draw")) gs_draw(GS_TRIS,0,static_cast<uint32_t>(d.count));
    gs_load_vertexbuffer(nullptr);
    gs_blend_state_pop(); gs_enable_framebuffer_srgb(srgb);
    return true;
}
}
