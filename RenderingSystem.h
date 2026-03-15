//#pragma once
//#include <d3d12.h>
//#include <wrl/client.h>
//#include <vector>
//#include <DirectXMath.h>
//#include "GBuffer.h"
//
//using Microsoft::WRL::ComPtr;
//
//// Структура для GPU буфера
//struct LightConstants
//{
//    DirectX::XMFLOAT4 positions[16];
//    float ranges[16];
//    DirectX::XMFLOAT4 colors[16];
//    float intensities[16];
//    int count;
//    float padding[3];
//};
//
//// СТРУКТУРА LIGHTDATA - ЭТОЙ НЕ ХВАТАЛО!
//struct LightData
//{
//    DirectX::XMFLOAT3 position;
//    float range;
//    DirectX::XMFLOAT3 color;
//    float intensity;
//    int type;
//    int padding[3];
//};
//
//class RenderingSystem
//{
//public:
//    RenderingSystem();
//    ~RenderingSystem();
//
//    bool Initialize(ID3D12Device* device, int width, int height);
//    void Release();
//
//    void UpdateLights(const std::vector<LightData>& lights);
//    bool UpdateLightBuffer(ID3D12GraphicsCommandList* cmdList);
//
//    void RenderOpaqueGeometry(ID3D12GraphicsCommandList* cmdList,
//        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
//        ID3D12Resource* constantBuffer,
//        D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress);
//
//    void RenderLights(ID3D12GraphicsCommandList* cmdList,
//        ID3D12RootSignature* rootSig,
//        ID3D12PipelineState* lightingPSO,
//        D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress);
//
//    GBuffer* GetGBuffer() { return &m_gbuffer; }
//    void SetAmbientColor(float r, float g, float b) { m_ambientColor = DirectX::XMFLOAT3(r, g, b); }
//
//private:
//    GBuffer m_gbuffer;
//    ComPtr<ID3D12Resource> m_lightBuffer;
//    LightConstants* m_mappedLightData;
//    std::vector<LightData> m_lights;
//    DirectX::XMFLOAT3 m_ambientColor;
//};