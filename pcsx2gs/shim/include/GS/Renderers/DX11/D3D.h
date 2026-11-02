// See GSDevice11.h: without the Direct3D renderers there are no D3D adapters
// to list, and OpenGL is the renderer to prefer.
#pragma once
#include "common/RedtapeWindows.h" // what the real D3D.h brings GS.cpp
#include "GS/GS.h"
#include <memory>
#include <vector>

namespace D3D
{
    struct NoFactory
    {
    };
    inline std::unique_ptr<NoFactory> CreateFactory(bool)
    {
        return {};
    }
    inline std::vector<GSAdapterInfo> GetAdapterInfo(NoFactory *)
    {
        return {};
    }
    inline GSRendererType GetPreferredRenderer()
    {
        return GSRendererType::OGL;
    }
}
