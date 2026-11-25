#pragma once
#include "core.hpp"
#include <memory>
#include <string>

namespace sf {
// All methods, including destruction, require the OBS graphics context.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    bool initialize();
    bool render(const std::vector<RenderParticle> &particles, unsigned width, unsigned height);
    const std::string &error() const;
    size_t vertexCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
