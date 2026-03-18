#include "RenderingSystem.h"

void RenderingSystem::RenderGeometryPass(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12RootSignature* rootSig,
    ID3D12PipelineState* pso,
    const D3D12_VERTEX_BUFFER_VIEW& vbView,
    const D3D12_INDEX_BUFFER_VIEW& ibView,
    UINT indexCount,
    ID3D12DescriptorHeap* srvHeap,
    ID3D12Resource* constantBuffer,
    GBuffer* gbuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    D3D12_VIEWPORT viewport = {};
    viewport.Width = 800; // Эти значения должны передаваться параметрами
    viewport.Height = 600;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = { 0, 0, 800, 600 };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissorRect);

    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    gbuffer->BeginGeometryPass(cmdList, dsvHandle);

    cmdList->SetPipelineState(pso);
    cmdList->SetGraphicsRootSignature(rootSig);

    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);
    cmdList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);

    gbuffer->EndGeometryPass(cmdList);
}

void RenderingSystem::RenderLightingPass(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12RootSignature* rootSig,
    ID3D12PipelineState* pso,
    GBuffer* gbuffer,
    ID3D12Resource* lightConstantBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    int screenWidth, int screenHeight,
    UINT backBufferIndex, UINT rtvDescriptorSize)
{
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(screenWidth);
    viewport.Height = static_cast<float>(screenHeight);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = { 0, 0, screenWidth, screenHeight };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandleCurrent = rtvHandle;
    rtvHandleCurrent.ptr += backBufferIndex * rtvDescriptorSize;

    cmdList->OMSetRenderTargets(1, &rtvHandleCurrent, FALSE, nullptr);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandleCurrent, clearColor, 0, nullptr);

    cmdList->SetPipelineState(pso);
    cmdList->SetGraphicsRootSignature(rootSig);

    ID3D12DescriptorHeap* heaps[] = { gbuffer->GetSRVHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootDescriptorTable(0, gbuffer->GetSRVGPU(GBuffer::Slot::AlbedoSpec));
    cmdList->SetGraphicsRootConstantBufferView(1, lightConstantBuffer->GetGPUVirtualAddress());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::RenderForward(
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
    UINT backBufferIndex, UINT rtvDescriptorSize)
{
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(screenWidth);
    viewport.Height = static_cast<float>(screenHeight);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = { 0, 0, screenWidth, screenHeight };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandleCurrent = rtvHandle;
    rtvHandleCurrent.ptr += backBufferIndex * rtvDescriptorSize;

    cmdList->OMSetRenderTargets(1, &rtvHandleCurrent, FALSE, &dsvHandle);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandleCurrent, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(rootSig);

    cmdList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &vbView);
    cmdList->IASetIndexBuffer(&ibView);
    cmdList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}