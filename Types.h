#pragma once
#include <DirectXMath.h>
#include <random>
#include <chrono>
#include <d3d12.h>

using namespace DirectX;

struct Vertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT4 Color;
    XMFLOAT2 TexCoord;
    XMFLOAT3 Tangent;     // Tangent
    XMFLOAT3 Binormal;    // Binormal
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
    float WaterTime;      // Время для анимации воды
    float Padding[3];     // Выравнивание до 16 байт
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

// Types.h - добавить после существующих структур

// Инстансинг: данные для каждого экземпляра объекта
struct InstanceData
{
    DirectX::XMFLOAT4X4 WorldMatrix;
    DirectX::XMFLOAT4 ColorModulation;  // Для вариации цвета
    DirectX::XMFLOAT4 CustomData;       // x: scale, y: emission, z: reflection, w: custom
};

// Сцена: коллекция инстансов
struct SceneObject
{
    std::string Name;
    std::vector<InstanceData> Instances;
    DirectX::XMFLOAT3 BasePosition;
    int InstanceCount;
};

// Пул объектов для случайной генерации
struct ObjectPool
{
    std::vector<InstanceData> Instances;
    DirectX::XMFLOAT3 SpawnAreaMin;
    DirectX::XMFLOAT3 SpawnAreaMax;
    float MinScale;
    float MaxScale;
};

const int MAX_DYNAMIC_LIGHTS = 256;