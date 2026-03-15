//#include "RenderingSystem.h"
//#include "ThrowIfFailed.h"
//#include <DirectXMath.h>
//#include <cstring>
//
//using namespace DirectX;
//
//RenderingSystem::RenderingSystem() : m_mappedLightData(nullptr)
//{
//    m_ambientColor = XMFLOAT3(0.1f, 0.1f, 0.1f);
//}
//
//RenderingSystem::~RenderingSystem()
//{
//    Release();
//}
//
//void RenderingSystem::Release()
//{
//    if (m_lightBuffer && m_mappedLightData)
//    {
//        m_lightBuffer->Unmap(0, nullptr);
//        m_mappedLightData = nullptr;
//    }
//    m_lightBuffer.Reset();
//    m_gbuffer.Release();
//}
//
//bool RenderingSystem::Initialize(ID3D12Device* device, int width, int height)
//{
//    if (!m_gbuffer.Initialize(device, width, height))
//        return false;
//
//    UINT64 bufferSize = (sizeof(LightConstants) + 255) & ~255ull;
//
//    D3D12_HEAP_PROPERTIES heapProps = {};
//    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
//
//    D3D12_RESOURCE_DESC bufferDesc = {};
//    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
//    bufferDesc.Width = bufferSize;
//    bufferDesc.Height = 1;
//    bufferDesc.DepthOrArraySize = 1;
//    bufferDesc.MipLevels = 1;
//    bufferDesc.SampleDesc = { 1, 0 };
//    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
//
//    HRESULT hr = device->CreateCommittedResource(
//        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
//        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
//        IID_PPV_ARGS(&m_lightBuffer));
//
//    if (FAILED(hr)) return false;
//
//    D3D12_RANGE mapRange = { 0, 0 };
//    hr = m_lightBuffer->Map(0, &mapRange, reinterpret_cast<void**>(&m_mappedLightData));
//    if (FAILED(hr) || !m_mappedLightData) return false;
//
//    memset(m_mappedLightData, 0, sizeof(LightConstants));
//    return true;
//}
//
//void RenderingSystem::UpdateLights(const std::vector<LightData>& lights)
//{
//    m_lights = lights;
//}
//
//bool RenderingSystem::UpdateLightBuffer(ID3D12GraphicsCommandList* cmdList)
//{
//    if (!m_mappedLightData) return false;
//
//    int lightCount = min((int)m_lights.size(), 16);
//    memset(m_mappedLightData, 0, sizeof(LightConstants));
//
//    for (int i = 0; i < lightCount; i++)
//    {
//        m_mappedLightData->positions[i] = XMFLOAT4(
//            m_lights[i].position.x,
//            m_lights[i].position.y,
//            m_lights[i].position.z,
//            0.0f
//        );
//        m_mappedLightData->ranges[i] = m_lights[i].range;
//        m_mappedLightData->colors[i] = XMFLOAT4(
//            m_lights[i].color.x,
//            m_lights[i].color.y,
//            m_lights[i].color.z,
//            0.0f
//        );
//        m_mappedLightData->intensities[i] = m_lights[i].intensity;
//    }
//
//    m_mappedLightData->count = lightCount;
//    return true;
//}
//
//void RenderingSystem::RenderOpaqueGeometry(ID3D12GraphicsCommandList* cmdList,
//    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
//    ID3D12Resource* constantBuffer,
//    D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress)
//{
//    D3D12_RESOURCE_BARRIER barriers[4];
//    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
//    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
//    barriers[0].Transition.pResource = m_gbuffer.GetDiffuseTexture();
//    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
//    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
//    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
//
//    barriers[1] = barriers[0];
//    barriers[1].Transition.pResource = m_gbuffer.GetNormalTexture();
//
//    barriers[2] = barriers[0];
//    barriers[2].Transition.pResource = m_gbuffer.GetPositionTexture();
//
//    barriers[3] = barriers[0];
//    barriers[3].Transition.pResource = m_gbuffer.GetSpecularTexture();
//
//    cmdList->ResourceBarrier(4, barriers);
//    m_gbuffer.SetRenderTargets(cmdList, dsvHandle);
//
//    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
//    m_gbuffer.Clear(cmdList, clearColor);
//}
//
//void RenderingSystem::RenderLights(ID3D12GraphicsCommandList* cmdList,
//    ID3D12RootSignature* rootSig,
//    ID3D12PipelineState* lightingPSO,
//    D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress)
//{
//    UpdateLightBuffer(cmdList);
//
//    D3D12_RESOURCE_BARRIER barriers[4];
//    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
//    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
//    barriers[0].Transition.pResource = m_gbuffer.GetDiffuseTexture();
//    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
//    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
//    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
//
//    barriers[1] = barriers[0];
//    barriers[1].Transition.pResource = m_gbuffer.GetNormalTexture();
//
//    barriers[2] = barriers[0];
//    barriers[2].Transition.pResource = m_gbuffer.GetPositionTexture();
//
//    barriers[3] = barriers[0];
//    barriers[3].Transition.pResource = m_gbuffer.GetSpecularTexture();
//
//    cmdList->ResourceBarrier(4, barriers);
//    cmdList->SetGraphicsRootSignature(rootSig);
//    cmdList->SetPipelineState(lightingPSO);
//
//    cmdList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
//    cmdList->SetGraphicsRootConstantBufferView(1, m_lightBuffer->GetGPUVirtualAddress());
//
//    ID3D12DescriptorHeap* heaps[] = { m_gbuffer.GetSRVHeap() };
//    cmdList->SetDescriptorHeaps(1, heaps);
//    cmdList->SetGraphicsRootDescriptorTable(2,
//        m_gbuffer.GetSRVHeap()->GetGPUDescriptorHandleForHeapStart());
//
//    cmdList->DrawInstanced(3, 1, 0, 0);
//}