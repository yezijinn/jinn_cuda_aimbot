#pragma once

#include <Windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace PreviewWindow
{
void UpdateAndRender(bool enabled, ID3D11Device* device, ID3D11DeviceContext* deviceContext);
void Shutdown();
}
