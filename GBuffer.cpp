#include "GBuffer.h"
#include "ThrowIfFailed.h"

namespace
{
    constexpr std::array<DXGI_FORMAT, GBuffer::TargetCount> kFormats =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM,      // AlbedoSpec
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // WorldPosition
        DXGI_FORMAT_R16G16B16A16_FLOAT,  // Normal
        DXGI_FORMAT_R32_FLOAT             // Depth
    };

    constexpr std::array<float, 4> kClearAlbedo = { 0.0f, 0.0f, 0.0f, 1.0f };
    constexpr std::array<float, 4> kClearWorld = { 0.0f, 0.0f, 0.0f, 0.0f };
    constexpr std::array<float, 4> kClearNormal = { 0.5f, 0.5f, 1.0f, 0.0f };
    constexpr std::array<float, 4> kClearDepth = { 1.0f, 0.0f, 0.0f, 0.0f };

    constexpr std::array<std::array<float, 4>, GBuffer::TargetCount> kClearColors =
    {
        kClearAlbedo,
        kClearWorld,
        kClearNormal,
        kClearDepth
    };
}

bool GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height)
{
    if (!device || width == 0 || height == 0)
        return false;

    m_device = device;
    m_width = width;
    m_height = height;

    if (!CreateDescriptorHeaps())
        return false;

    return CreateRenderTargets();
}

void GBuffer::Shutdown()
{
    for (auto& target : m_targets)
        target.Reset();

    m_rtvHeap.Reset();
    m_srvHeap.Reset();
    m_device.Reset();
}

bool GBuffer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = TargetCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = TargetCount + 4;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)));

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void GBuffer::CreateExternalSRV(
    UINT slot,
    ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();

    handle.ptr += slot * m_srvDescriptorSize;

    m_device->CreateShaderResourceView(
        resource,
        &srvDesc,
        handle);
}

bool GBuffer::CreateRenderTargets()
{
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT i = 0; i < TargetCount; ++i)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = m_width;
        desc.Height = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kFormats[i];
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = kFormats[i];
        memcpy(clearValue.Color, kClearColors[i].data(), sizeof(float) * 4);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearValue,
            IID_PPV_ARGS(&m_targets[i])));

        m_device->CreateRenderTargetView(m_targets[i].Get(), nullptr, GetRTV(static_cast<Slot>(i)));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = kFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, GetSRV(static_cast<Slot>(i)));
    }
    return true;
}

void GBuffer::BeginGeometryPass(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView)
{
    if (m_currentState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        TransitionAll(commandList, m_currentState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, TargetCount> rtvs;
    for (UINT i = 0; i < TargetCount; ++i)
    {
        rtvs[i] = GetRTV(static_cast<Slot>(i));
        commandList->ClearRenderTargetView(rtvs[i], kClearColors[i].data(), 0, nullptr);
    }

    commandList->OMSetRenderTargets(TargetCount, rtvs.data(), FALSE, &depthStencilView);
}

void GBuffer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    if (m_currentState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        TransitionAll(commandList, m_currentState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void GBuffer::TransitionAll(ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES beforeState,
    D3D12_RESOURCE_STATES afterState)
{
    std::array<D3D12_RESOURCE_BARRIER, TargetCount> barriers = {};
    for (UINT i = 0; i < TargetCount; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = m_targets[i].Get();
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = beforeState;
        barriers[i].Transition.StateAfter = afterState;
    }
    commandList->ResourceBarrier(TargetCount, barriers.data());
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetRTV(Slot slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT>(slot) * m_rtvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetSRV(Slot slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT>(slot) * m_srvDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::GetSRVGPU(Slot slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT>(slot) * m_srvDescriptorSize;
    return handle;
}

DXGI_FORMAT GBuffer::GetFormat(Slot slot) const
{
    return kFormats[static_cast<UINT>(slot)];
}

ID3D12DescriptorHeap* GBuffer::GetRTVHeap() const { return m_rtvHeap.Get(); }
ID3D12DescriptorHeap* GBuffer::GetSRVHeap() const { return m_srvHeap.Get(); }