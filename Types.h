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

// В Types.h добавьте:
struct InstanceData
{
    DirectX::XMFLOAT3 Position;
    float Scale;
    DirectX::XMFLOAT3 Color;
    float Padding; // Для выравнивания до 16 байт
};
const int MAX_DYNAMIC_LIGHTS = 256;

// Particle structures
struct ParticleInstanceData
{
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT4 Color;
    float Size;
    float Padding; // For 16-byte alignment
};

struct ParticleVertex
{
    DirectX::XMFLOAT3 Position;
};

struct ParticleSystemConstants
{
    DirectX::XMMATRIX ViewProj;
    DirectX::XMFLOAT3 CameraPos;
    float Padding;
};

// Particle system parameters
struct Particle
{
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Velocity;
    DirectX::XMFLOAT4 Color;
    float Size;
    float Life;
    float MaxLife;
    float Weight;
};

// Particle emitter settings
struct ParticleEmitterSettings
{
    DirectX::XMFLOAT3 EmitterPosition;
    DirectX::XMFLOAT3 EmitterDirection;
    float EmitRate;           // Particles per second
    float SpreadAngle;        // Emission spread in radians
    float MinSpeed;
    float MaxSpeed;
    float MinSize;
    float MaxSize;
    float MinLife;
    float MaxLife;
    float Gravity;            // Downward force
    DirectX::XMFLOAT4 StartColor;
    DirectX::XMFLOAT4 EndColor;
};

// В Types.h добавь:
struct GPUParticle
{
    DirectX::XMFLOAT3 Position;
    float Life;           // Оставшееся время жизни
    DirectX::XMFLOAT3 Velocity;
    float LifeSpan;       // Полное время жизни
    DirectX::XMFLOAT4 Color;
    float Size;
    float Weight;         // Для гравитации
    float Age;            // Прожитое время
    float Rotation;       // Не используется, но для alignment
};