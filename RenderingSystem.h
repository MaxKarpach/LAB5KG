#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <DirectXMath.h>

#include "GBuffer.h"
#include "Types.h"

using Microsoft::WRL::ComPtr;

class RenderingSystem
{
public:
    bool IsWireframe() const { return m_wireframe; }  // Добавьте этот метод
    void SetWireframe(bool on) { m_wireframe = on; }   // Уже
    void RenderGeometryPass(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12RootSignature* rootSig,
        ID3D12PipelineState* pso,
        const D3D12_VERTEX_BUFFER_VIEW& vbView,
        const D3D12_INDEX_BUFFER_VIEW& ibView,
        UINT indexCount,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12Resource* constantBuffer,
        GBuffer* gbuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

    void RenderLightingPass(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12RootSignature* rootSig,
        ID3D12PipelineState* pso,
        GBuffer* gbuffer,
        ID3D12Resource* lightConstantBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
        int screenWidth, int screenHeight,
        UINT backBufferIndex, UINT rtvDescriptorSize);

    void RenderForward(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12RootSignature* rootSig,
        ID3D12PipelineState* pso,
        const D3D12_VERTEX_BUFFER_VIEW& vbView,
        const D3D12_INDEX_BUFFER_VIEW& ibView,
        UINT indexCount,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12Resource* constantBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        int screenWidth, int screenHeight,
        UINT backBufferIndex, UINT rtvDescriptorSize);
private:
    bool m_wireframe = false;  // Добавьте этот член, если его нет
};