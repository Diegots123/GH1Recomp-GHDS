// GH1 Recomp builds PCSX2's GS with its OpenGL renderer on every platform.
// On Windows, GS.cpp also names the Direct3D devices; this header (found
// before PCSX2's own) gives those names the OpenGL device, so the D3D
// renderers are not compiled. GH1 Recomp only ever asks for OpenGL.
#pragma once
#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
using GSDevice11 = GSDeviceOGL;
