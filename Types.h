#pragma once
#include <DirectXMath.h>
#include <d3d12.h>

using namespace DirectX;

struct Vertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT4 Color;
    XMFLOAT2 TexCoord;
    XMFLOAT3 Tangent;     // Добавляем Tangent
    XMFLOAT3 Binormal;    // Добавляем Binormal
};
struct ConstantBufferData
{
    XMMATRIX World;
    XMMATRIX View;
    XMMATRIX Proj;
    XMFLOAT4 LightPos;
    XMFLOAT4 LightColor;
    XMFLOAT4 CameraPos;
    XMFLOAT2 Tiling;
    XMFLOAT2 UVOffset;
};

struct GBufferData
{
    XMFLOAT4 AlbedoSpec;  // rgb = albedo, a = specular
    XMFLOAT4 Normal;       // normal
    XMFLOAT4 WorldPos;     // world position
};


struct DynamicLight
{
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Color;
    float Intensity;
    float Range;
    bool Active;
};

const int MAX_DYNAMIC_LIGHTS = 256;