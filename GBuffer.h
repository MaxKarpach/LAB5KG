#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>

using Microsoft::WRL::ComPtr;

class GBuffer
{
public:
    static constexpr UINT TargetCount = 4;

    void CreateExternalSRV(
        UINT slot,
        ID3D12Resource* resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);

    enum class Slot : UINT
    {
        AlbedoSpec = 0,
        WorldPosition = 1,
        Normal = 2,
        Depth = 3,
        Count = 4
    };

    bool Initialize(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();

    void BeginGeometryPass(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView);
    void EndGeometryPass(ID3D12GraphicsCommandList* commandList);

    ID3D12DescriptorHeap* GetRTVHeap() const;
    ID3D12DescriptorHeap* GetSRVHeap() const;

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(Slot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(Slot slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGPU(Slot slot) const;

    // ВАЖНО: Добавить этот метод
    ID3D12Resource* GetResource(Slot slot) const { return m_targets[static_cast<UINT>(slot)].Get(); }

    DXGI_FORMAT GetFormat(Slot slot) const;

    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    bool CreateDescriptorHeaps();
    bool CreateRenderTargets();
    void TransitionAll(ID3D12GraphicsCommandList* commandList,
        D3D12_RESOURCE_STATES beforeState,
        D3D12_RESOURCE_STATES afterState);

private:
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    std::array<ComPtr<ID3D12Resource>, TargetCount> m_targets;
};