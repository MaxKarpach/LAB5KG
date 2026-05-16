#include "DirectXApp.h"
#include "ThrowIfFailed.h"
#include <cassert>
#include <memory>
#include <DirectXMath.h>
#include <functional>
using namespace DirectX;

// Constructor / Destructor
DirectXApp::DirectXApp(HWND windowHandle, int windowWidth, int windowHeight, const InputDevice* inputDevice)
    : m_windowHandle(windowHandle)
    , m_screenWidth(windowWidth)
    , m_screenHeight(windowHeight)
    , m_inputDevice(inputDevice)
{
    for (UINT i = 0; i < BACK_BUFFER_COUNT; ++i)
        m_fenceValues[i] = 0;

    // Настраиваем камеру
    m_camera.SetPosition(DirectX::XMFLOAT3(0.0f, 5.0f, -15.0f));
    m_camera.SetSpeed(15.0f);
    m_camera.SetRotationSpeed(60.0f);  // 60 градусов в секунду
}
DirectXApp::~DirectXApp()
{
    if (m_cmdQueue && m_fence && m_fenceEvent)
    {
        try { FlushCommandQueue(); }
        catch (...) {}
    }

    if (m_constantBuffer && m_mappedConstantData)
        m_constantBuffer->Unmap(0, nullptr);

    if (m_tessellationCB && m_mappedTessellationData)
        m_tessellationCB->Unmap(0, nullptr);

    if (m_deferredLightConstantBuffer && m_deferredLightCBMappedData)
        m_deferredLightConstantBuffer->Unmap(0, nullptr);

    if (m_shadowConstantBuffer && m_mappedShadowConstantData)
        m_shadowConstantBuffer->Unmap(0, nullptr);

    // Очистка 1000 буферов кубов
    for (int i = 0; i < (int)m_cubeConstantBuffers.size(); ++i)
    {
        if (m_cubeConstantBuffers[i] && m_mappedCubeConstantDataArray[i])
            m_cubeConstantBuffers[i]->Unmap(0, nullptr);
    }

    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);


}
// Descriptor Heaps
void DirectXApp::CreateDescriptorHeaps()
{
    // RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = BACK_BUFFER_COUNT;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // DSV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_dsvHeap)));
    }

    // SRV/CBV/UAV heap для текстур
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 16;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)));
    }

    // PARTICLE HEAP — отдельная куча для системы частиц
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 8;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_particleHeap)));
        m_particleDescSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

// Render Target Views
void DirectXApp::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < BACK_BUFFER_COUNT; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
        m_d3dDevice->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

// Depth Stencil Buffer
void DirectXApp::CreateDepthBuffer()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = static_cast<UINT>(m_screenWidth);
    depthDesc.Height = static_cast<UINT>(m_screenHeight);
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc = { 1, 0 };
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue, IID_PPV_ARGS(&m_depthStencil)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_d3dDevice->CreateDepthStencilView(m_depthStencil.Get(),
        &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// Command List and Allocators
void DirectXApp::CreateCommandList()
{
    for (UINT i = 0; i < BACK_BUFFER_COUNT; ++i)
        ThrowIfFailed(m_d3dDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_cmdAllocators[i])));

    ThrowIfFailed(m_d3dDevice->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&m_cmdList)));

    m_cmdList->Close();
}

// Fence and Sync Objects
void DirectXApp::CreateSyncObjects()
{
    ThrowIfFailed(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)));
    m_fenceValues[0] = 1;
    m_fenceValues[1] = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
}

// Pipeline State Object
ComPtr<ID3DBlob> DirectXApp::CompileShaderFile(
    const std::wstring& filePath,
    const std::string& entryPoint,
    const std::string& shaderModel,
    const D3D_SHADER_MACRO* defines)
{
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::string errorMsg = "Shader file missing:\n";
        int charCount = WideCharToMultiByte(
            CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrowPath(charCount, 0);
        WideCharToMultiByte(
            CP_UTF8, 0, filePath.c_str(), -1, &narrowPath[0], charCount, nullptr, nullptr);
        errorMsg += narrowPath;
        MessageBoxA(m_windowHandle, errorMsg.c_str(), "Shader Error", MB_OK | MB_ICONERROR);
        ThrowIfFailed(E_FAIL, "Shader compilation failed - file not found");
    }

    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompileFromFile(
        filePath.c_str(),
        defines,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        shaderModel.c_str(),
        compileFlags,
        0,
        &shaderBlob,
        &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            MessageBoxA(
                m_windowHandle,
                static_cast<const char*>(errorBlob->GetBufferPointer()),
                "Shader Compile Error",
                MB_OK | MB_ICONERROR);
        }
        ThrowIfFailed(hr, "Shader compilation failed");
    }

    return shaderBlob;
}

void DirectXApp::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRanges[4] = {};

    // t0 - main texture
    srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[0].NumDescriptors = 1;
    srvRanges[0].BaseShaderRegister = 0;
    srvRanges[0].RegisterSpace = 0;
    srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t1 - normal map
    srvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[1].NumDescriptors = 1;
    srvRanges[1].BaseShaderRegister = 1;
    srvRanges[1].RegisterSpace = 0;
    srvRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t2 - displacement map
    srvRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[2].NumDescriptors = 1;
    srvRanges[2].BaseShaderRegister = 2;
    srvRanges[2].RegisterSpace = 0;
    srvRanges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t4 - shadow map
    // ВАЖНО: t3 у тебя занят GBuffer depth в deferred lighting pass,
    // поэтому shadow map кладем в t4.
    srvRanges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[3].NumDescriptors = 1;
    srvRanges[3].BaseShaderRegister = 4;
    srvRanges[3].RegisterSpace = 0;
    srvRanges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[4] = {};

    // b0 - object/global constant buffer
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // textures table: t0, t1, t2, t4
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = _countof(srvRanges);
    rootParams[1].DescriptorTable.pDescriptorRanges = srvRanges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // b2 - tessellation constants
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 2;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // b3 - water constants
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 3;
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};

    // s0 - обычный sampler для текстур
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MipLODBias = 0.0f;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1 - comparison sampler для shadow map + PCF
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].MipLODBias = 0.0f;
    samplers[1].MaxAnisotropy = 1;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MinLOD = 0.0f;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = _countof(rootParams);
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = _countof(samplers);
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;

    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);

    if (FAILED(hr))
    {
        if (error)
        {
            std::string errorMsg =
                static_cast<const char*>(error->GetBufferPointer());

            MessageBoxA(
                m_windowHandle,
                errorMsg.c_str(),
                "Root Signature Error",
                MB_OK | MB_ICONERROR);
        }

        ThrowIfFailed(hr);
    }

    ThrowIfFailed(m_d3dDevice->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

void DirectXApp::CreateDeferredRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 5; // t0-t3 = GBuffer, t4 = ShadowMap
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};

    // Root parameter 0: SRV table for deferred lighting
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root parameter 1: DeferredLightCB at b1
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler = {};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.MipLODBias = 0.0f;
    shadowSampler.MaxAnisotropy = 1;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD = 0.0f;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 1; // s1 = ShadowSampler
    shadowSampler.RegisterSpace = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = _countof(rootParams);
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &shadowSampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;

    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);

    if (FAILED(hr))
    {
        if (error)
        {
            std::string errorMsg =
                static_cast<const char*>(error->GetBufferPointer());

            MessageBoxA(
                m_windowHandle,
                errorMsg.c_str(),
                "Deferred Root Signature Error",
                MB_OK | MB_ICONERROR);
        }

        ThrowIfFailed(hr);
    }

    ThrowIfFailed(m_d3dDevice->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_deferredLightingRootSignature)));
}

void DirectXApp::CreatePipelineState()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vertexShader = CompileShaderFile(filePath, "ForwardVSMain", "vs_5_0");
    auto pixelShader = CompileShaderFile(filePath, "ForwardPSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_pipelineState)));
}

void DirectXApp::CreateDeferredPipelines()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blendDesc = {};
    for (UINT i = 0; i < 4; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    auto geometryVS = CompileShaderFile(filePath, "GeometryVSMain", "vs_5_0");
    auto geometryPS = CompileShaderFile(filePath, "GeometryPSMain", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geomPsoDesc = {};
    geomPsoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    geomPsoDesc.pRootSignature = m_rootSignature.Get();
    geomPsoDesc.VS = { geometryVS->GetBufferPointer(), geometryVS->GetBufferSize() };
    geomPsoDesc.PS = { geometryPS->GetBufferPointer(), geometryPS->GetBufferSize() };
    geomPsoDesc.RasterizerState = rasterizerDesc;
    geomPsoDesc.BlendState = blendDesc;
    geomPsoDesc.DepthStencilState = depthStencilDesc;
    geomPsoDesc.SampleMask = UINT_MAX;
    geomPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geomPsoDesc.NumRenderTargets = GBuffer::TargetCount;
    geomPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geomPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geomPsoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geomPsoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
    geomPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    geomPsoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(
        &geomPsoDesc, IID_PPV_ARGS(&m_deferredGeometryPSO)));

    auto lightVS = CompileShaderFile(filePath, "LightVSMain", "vs_5_0");
    auto lightPS = CompileShaderFile(filePath, "LightPSMain", "ps_5_0");

    D3D12_BLEND_DESC lightBlendDesc = {};
    lightBlendDesc.RenderTarget[0].BlendEnable = TRUE;
    lightBlendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    lightBlendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    lightBlendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    lightBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    lightBlendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    lightBlendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    lightBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC lightDepthStencilDesc = {};
    lightDepthStencilDesc.DepthEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightPsoDesc = {};
    lightPsoDesc.pRootSignature = m_deferredLightingRootSignature.Get();
    lightPsoDesc.VS = { lightVS->GetBufferPointer(), lightVS->GetBufferSize() };
    lightPsoDesc.PS = { lightPS->GetBufferPointer(), lightPS->GetBufferSize() };
    lightPsoDesc.RasterizerState = rasterizerDesc;
    lightPsoDesc.BlendState = lightBlendDesc;
    lightPsoDesc.DepthStencilState = lightDepthStencilDesc;
    lightPsoDesc.SampleMask = UINT_MAX;
    lightPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightPsoDesc.NumRenderTargets = 1;
    lightPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    lightPsoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(
        &lightPsoDesc, IID_PPV_ARGS(&m_deferredLightingPSO)));
}

void DirectXApp::CreateUploadBuffer(const void* data, UINT64 dataSize,
    ComPtr<ID3D12Resource>& outputBuffer)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = dataSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&outputBuffer)));

    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    outputBuffer->Map(0, &readRange, &mappedData);
    memcpy(mappedData, data, static_cast<size_t>(dataSize));
    outputBuffer->Unmap(0, nullptr);
}

void DirectXApp::BuildMeshBuffers(const std::vector<Vertex>& vertices,
    const std::vector<UINT>& indices)
{
    m_indexCount = static_cast<UINT>(indices.size());

    UINT64 vertexBufferSize = vertices.size() * sizeof(Vertex);
    UINT64 indexBufferSize = indices.size() * sizeof(UINT);

    CreateUploadBuffer(vertices.data(), vertexBufferSize, m_vertexBuffer);
    CreateUploadBuffer(indices.data(), indexBufferSize, m_indexBuffer);

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);

    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
}
std::vector<Vertex> DirectXApp::GenerateCubeMesh()
{
    float halfSize = 0.5f;

    // Функция для вычисления tangent и binormal для грани
    auto calculateTangents = [](const XMFLOAT3& normal) -> std::pair<XMFLOAT3, XMFLOAT3> {
        XMFLOAT3 tangent, binormal;

        // Для каждой грани выбираем tangent на основе нормали
        if (fabs(normal.x) > 0.9f) // X-нормаль (лево/право)
        {
            tangent = XMFLOAT3(0.0f, 0.0f, 1.0f);
            binormal = XMFLOAT3(0.0f, 1.0f, 0.0f);
        }
        else if (fabs(normal.y) > 0.9f) // Y-нормаль (верх/низ)
        {
            tangent = XMFLOAT3(1.0f, 0.0f, 0.0f);
            binormal = XMFLOAT3(0.0f, 0.0f, 1.0f);
        }
        else // Z-нормаль (перед/зад)
        {
            tangent = XMFLOAT3(1.0f, 0.0f, 0.0f);
            binormal = XMFLOAT3(0.0f, 1.0f, 0.0f);
        }

        return { tangent, binormal };
        };

    Vertex vertexData[] =
    {
        // Front face (Z = halfSize)
        { {-halfSize,-halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {0,1}, {1,0,0}, {0,1,0} },
        { { halfSize,-halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {1,1}, {1,0,0}, {0,1,0} },
        { { halfSize, halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {1,0}, {1,0,0}, {0,1,0} },
        { {-halfSize, halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {0,0}, {1,0,0}, {0,1,0} },

        // Back face (Z = -halfSize)
        { { halfSize,-halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {0,1}, {-1,0,0}, {0,1,0} },
        { {-halfSize,-halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {1,1}, {-1,0,0}, {0,1,0} },
        { {-halfSize, halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {1,0}, {-1,0,0}, {0,1,0} },
        { { halfSize, halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {0,0}, {-1,0,0}, {0,1,0} },

        // Left face (X = -halfSize)
        { {-halfSize,-halfSize,-halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {0,1}, {0,0,1}, {0,1,0} },
        { {-halfSize,-halfSize, halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {1,1}, {0,0,1}, {0,1,0} },
        { {-halfSize, halfSize, halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {1,0}, {0,0,1}, {0,1,0} },
        { {-halfSize, halfSize,-halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {0,0}, {0,0,1}, {0,1,0} },

        // Right face (X = halfSize)
        { { halfSize,-halfSize, halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {0,1}, {0,0,-1}, {0,1,0} },
        { { halfSize,-halfSize,-halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {1,1}, {0,0,-1}, {0,1,0} },
        { { halfSize, halfSize,-halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {1,0}, {0,0,-1}, {0,1,0} },
        { { halfSize, halfSize, halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {0,0}, {0,0,-1}, {0,1,0} },

        // Top face (Y = halfSize)
        { {-halfSize, halfSize, halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {0,1}, {1,0,0}, {0,0,-1} },
        { { halfSize, halfSize, halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {1,1}, {1,0,0}, {0,0,-1} },
        { { halfSize, halfSize,-halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {1,0}, {1,0,0}, {0,0,-1} },
        { {-halfSize, halfSize,-halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {0,0}, {1,0,0}, {0,0,-1} },

        // Bottom face (Y = -halfSize)
        { {-halfSize,-halfSize,-halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {0,1}, {1,0,0}, {0,0,1} },
        { { halfSize,-halfSize,-halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {1,1}, {1,0,0}, {0,0,1} },
        { { halfSize,-halfSize, halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {1,0}, {1,0,0}, {0,0,1} },
        { {-halfSize,-halfSize, halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {0,0}, {1,0,0}, {0,0,1} },
    };

    return std::vector<Vertex>(vertexData, vertexData + _countof(vertexData));
}
std::vector<UINT> DirectXApp::GenerateCubeIndices()
{
    std::vector<UINT> indices;
    for (UINT face = 0; face < 6; ++face)
    {
        UINT baseIndex = face * 4;
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 3);
    }
    return indices;
}

void DirectXApp::CreateDefaultGeometry()
{
    auto vertices = GenerateCubeMesh();
    auto indices = GenerateCubeIndices();
    BuildMeshBuffers(vertices, indices);
}

void DirectXApp::ImportModel(const std::wstring& modelPath)
{
    ObjResult model = LoadObj(modelPath);
    if (!model.valid)
    {
        CreateDefaultGeometry();
        return;
    }
    BuildMeshBuffers(model.vertices, model.indices);
}

void DirectXApp::UploadTexture(const TextureData& texData, int textureSlot, bool isNormalMap)
{
    if (!texData.valid || texData.width == 0 || texData.height == 0)
        ThrowIfFailed(E_INVALIDARG, "Invalid texture data provided");

    ComPtr<ID3D12Resource>* targetTexture = nullptr;
    ComPtr<ID3D12Resource>* uploadBuffer = nullptr;

    // Выбираем правильный слот
    if (textureSlot == 0)
    {
        targetTexture = &m_textureFirst;
        uploadBuffer = &m_textureFirstUpload;
    }
    else if (textureSlot == 1)
    {
        targetTexture = &m_textureSecond;
        uploadBuffer = &m_textureSecondUpload;
    }
    else if (textureSlot == 2)  // Normal map
    {
        targetTexture = &m_normalTexture;
        uploadBuffer = &m_normalTextureUpload;
    }
    else if (textureSlot == 3)  // Displacement map
    {
        targetTexture = &m_displacementTexture;
        uploadBuffer = &m_displacementTextureUpload;
    }
    else
    {
        ThrowIfFailed(E_INVALIDARG, "Invalid texture slot");
        return;
    }

    if (!targetTexture || !uploadBuffer)
    {
        ThrowIfFailed(E_INVALIDARG, "Invalid texture slot");
        return;
    }

    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = texData.width;
    textureDesc.Height = texData.height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;

    // Для displacement текстуры (слот 3) используем R8_UNORM
    if (textureSlot == 3)  // displacement map slot
    {
        textureDesc.Format = DXGI_FORMAT_R8_UNORM;
    }
    else
    {
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    textureDesc.SampleDesc = { 1, 0 };
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Создаем текстуру в памяти GPU
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(targetTexture->GetAddressOf())));

    // Получаем информацию о размере для копирования
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowPitch = 0, totalBytes = 0;
    m_d3dDevice->GetCopyableFootprints(
        &textureDesc, 0, 1, 0, &footprint, &numRows, &rowPitch, &totalBytes);

    // Создаем буфер для загрузки
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc = { 1, 0 };
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(uploadBuffer->GetAddressOf())));

    // Копируем данные в буфер
    uint8_t* mappedMemory = nullptr;
    (*uploadBuffer)->Map(0, nullptr, reinterpret_cast<void**>(&mappedMemory));

    UINT sourceRowPitch = texData.width * 4;

    // Для displacement текстуры (слот 3) копируем только R канал
    if (textureSlot == 3)
    {
        for (UINT row = 0; row < texData.height; ++row)
        {
            for (UINT col = 0; col < texData.width; ++col)
            {
                uint8_t r = texData.pixels[(row * texData.width + col) * 4 + 0];
                mappedMemory[(UINT64)footprint.Footprint.RowPitch * row + col] = r;
            }
        }
    }
    else
    {
        for (UINT row = 0; row < texData.height; ++row)
        {
            memcpy(mappedMemory + (UINT64)footprint.Footprint.RowPitch * row,
                texData.pixels.data() + (UINT64)sourceRowPitch * row,
                sourceRowPitch);
        }
    }

    (*uploadBuffer)->Unmap(0, nullptr);

    // Копируем текстуру из буфера в GPU память
    D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
    sourceLocation.pResource = uploadBuffer->Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION destLocation = {};
    destLocation.pResource = targetTexture->Get();
    destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destLocation.SubresourceIndex = 0;

    m_cmdList->CopyTextureRegion(&destLocation, 0, 0, 0, &sourceLocation, nullptr);

    // Переводим текстуру в состояние для чтения шейдером
    D3D12_RESOURCE_BARRIER transitionBarrier = {};
    transitionBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transitionBarrier.Transition.pResource = targetTexture->Get();
    transitionBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    transitionBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    transitionBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &transitionBarrier);

    // Создаем SRV для текстуры
    UINT srvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();

    int srvOffset = textureSlot;
    srvHandle.ptr += srvOffset * srvDescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    // Для displacement текстуры используем R8_UNORM
    if (textureSlot == 3)
    {
        srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    }
    else
    {
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_d3dDevice->CreateShaderResourceView(targetTexture->Get(), &srvDesc, srvHandle);
}

void DirectXApp::CreateSwapChain()
{
    ComPtr<IDXGIFactory4> dxgiFactory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = BACK_BUFFER_COUNT;
    swapChainDesc.Width = static_cast<UINT>(m_screenWidth);
    swapChainDesc.Height = static_cast<UINT>(m_screenHeight);
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc = { 1, 0 };

    ComPtr<IDXGISwapChain1> tempSwapChain;
    ThrowIfFailed(dxgiFactory->CreateSwapChainForHwnd(
        m_cmdQueue.Get(), m_windowHandle, &swapChainDesc, nullptr, nullptr, &tempSwapChain));
    dxgiFactory->MakeWindowAssociation(m_windowHandle, DXGI_MWA_NO_ALT_ENTER);
    ThrowIfFailed(tempSwapChain.As(&m_swapChain));
    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();
}

void DirectXApp::CreateConstantBuffer()
{
    UINT64 bufferSize = sizeof(ConstantBufferData);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_constantBuffer)));

    D3D12_RANGE mapRange = { 0, 0 };
    m_constantBuffer->Map(0, &mapRange,
        reinterpret_cast<void**>(&m_mappedConstantData));
}

void DirectXApp::CreateLightingConstantBuffer()
{
    const UINT cbSize = (sizeof(DeferredLightCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cbSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_deferredLightConstantBuffer)));

    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(m_deferredLightConstantBuffer->Map(0, &readRange,
        reinterpret_cast<void**>(&m_deferredLightCBMappedData)));
}


// DirectXApp.cpp - обновить UpdateLightingConstants:
void DirectXApp::UpdateLightingConstants()
{
    if (!m_deferredLightCBMappedData)
        return;

    DeferredLightCB cb = {};

    // Directional light
    cb.DirectionalLightDirection = DirectX::XMFLOAT4(-0.5f, -1.0f, -0.3f, 0.0f);
    cb.DirectionalLightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 0.5f);
    cb.AmbientColor = DirectX::XMFLOAT4(0.15f, 0.15f, 0.2f, 1.0f);

    // Static point lights (6 штук)
    cb.PointLightPositionRange[0] = DirectX::XMFLOAT4(-10.0f, 5.0f, -8.0f, 100.0f);
    cb.PointLightColorIntensity[0] = DirectX::XMFLOAT4(1.0f, 0.2f, 0.2f, 8.0f);

    cb.PointLightPositionRange[1] = DirectX::XMFLOAT4(10.0f, 5.0f, 8.0f, 100.0f);
    cb.PointLightColorIntensity[1] = DirectX::XMFLOAT4(0.2f, 0.2f, 1.0f, 8.0f);

    cb.PointLightPositionRange[2] = DirectX::XMFLOAT4(-10.0f, 4.0f, 10.0f, 100.0f);
    cb.PointLightColorIntensity[2] = DirectX::XMFLOAT4(0.2f, 1.0f, 0.2f, 8.0f);

    cb.PointLightPositionRange[3] = DirectX::XMFLOAT4(10.0f, 4.0f, -10.0f, 100.0f);
    cb.PointLightColorIntensity[3] = DirectX::XMFLOAT4(1.0f, 1.0f, 0.2f, 8.0f);

    cb.PointLightPositionRange[4] = DirectX::XMFLOAT4(0.0f, 8.0f, 0.0f, 100.0f);
    cb.PointLightColorIntensity[4] = DirectX::XMFLOAT4(1.0f, 0.2f, 1.0f, 10.0f);

    cb.PointLightPositionRange[5] = DirectX::XMFLOAT4(-5.0f, 6.0f, -3.0f, 100.0f);
    cb.PointLightColorIntensity[5] = DirectX::XMFLOAT4(0.2f, 1.0f, 1.0f, 8.0f);

    // Dynamic lights (от выстрелов)
    int dynamicCount = min((int)m_dynamicLights.size(), 26);

    for (int i = 0; i < dynamicCount; ++i)
    {
        cb.PointLightPositionRange[6 + i] = DirectX::XMFLOAT4(
            m_dynamicLights[i].Position.x,
            m_dynamicLights[i].Position.y,
            m_dynamicLights[i].Position.z,
            m_dynamicLights[i].Range
        );
        cb.PointLightColorIntensity[6 + i] = DirectX::XMFLOAT4(
            m_dynamicLights[i].Color.x,
            m_dynamicLights[i].Color.y,
            m_dynamicLights[i].Color.z,
            m_dynamicLights[i].Intensity
        );
    }

    // Light counts
    cb.LightCounts = DirectX::XMFLOAT4(
        6.0f,
        0.0f,
        (float)dynamicCount,
        0.0f
    );

    cb.ScreenSize = DirectX::XMFLOAT4(
        static_cast<float>(m_screenWidth),
        static_cast<float>(m_screenHeight),
        1.0f / static_cast<float>(m_screenWidth),
        1.0f / static_cast<float>(m_screenHeight)
    );

    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();
    cb.CameraPosition = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 1.0f);

    for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade)
    {
        cb.LightViewProj[cascade] = m_lightViewProj[cascade];
    }

    cb.CascadeSplits = m_cascadeSplits;

    cb.ShadowParams = DirectX::XMFLOAT4(
        0.0025f,
        static_cast<float>(SHADOW_MAP_SIZE),
        static_cast<float>(SHADOW_CASCADE_COUNT),
        0.0f
    );

    cb.DebugParams = DirectX::XMFLOAT4(
        static_cast<float>(static_cast<int>(m_shadowDebugMode)),
        0.0f,
        0.0f,
        0.0f
    );

    // Camera matrices
    float aspectRatio = (m_screenHeight > 0) ?
        static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;

    DirectX::XMMATRIX view = m_camera.GetViewMatrix();
    DirectX::XMMATRIX proj = m_camera.GetProjectionMatrix(aspectRatio);

    DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
    DirectX::XMMATRIX invProj = DirectX::XMMatrixInverse(nullptr, proj);
    DirectX::XMStoreFloat4x4(&cb.InvView, DirectX::XMMatrixTranspose(invView));
    DirectX::XMStoreFloat4x4(&cb.InvProj, DirectX::XMMatrixTranspose(invProj));

    memcpy(m_deferredLightCBMappedData, &cb, sizeof(cb));

}

void DirectXApp::Update(float deltaTime)
{
    // Обновляем камеру
    if (m_inputDevice)
        m_camera.Update(deltaTime, *m_inputDevice);

    // Кулдаун стрельбы
    if (m_shootCooldown > 0.0f)
        m_shootCooldown -= deltaTime;

    // Стрельба по пробелу
    if (m_inputDevice && m_inputDevice->IsKeyDown(VK_SPACE) && m_shootCooldown <= 0.0f)
    {
        Shoot();
        m_shootCooldown = SHOOT_COOLDOWN_TIME;
    }

    // ========== ПЕРЕКЛЮЧЕНИЕ РЕЖИМОВ CULLING ==========
    if (m_inputDevice)
    {
        if (m_inputDevice->IsKeyDown('C') && !m_prevCKey)
        {
            switch (m_cullingMode)
            {
            case CullingMode::None:
                SetCullingMode(CullingMode::Frustum);
                break;
            case CullingMode::Frustum:
                SetCullingMode(CullingMode::Octree);
                break;
            case CullingMode::Octree:
                SetCullingMode(CullingMode::None);
                break;
            }
        }
        m_prevCKey = m_inputDevice->IsKeyDown('C');

        if (m_inputDevice->IsKeyDown('V') && !m_prevVKey)
        {
            if (m_cullingMode == CullingMode::None)
                SetCullingMode(CullingMode::Octree);
            else
                SetCullingMode(CullingMode::None);
        }
        m_prevVKey = m_inputDevice->IsKeyDown('V');

        if (m_inputDevice->IsKeyDown('B') && !m_prevBKey)
        {
            int mode = static_cast<int>(m_shadowDebugMode);
            mode = (mode + 1) % 5;
            m_shadowDebugMode = static_cast<ShadowDebugMode>(mode);

            const wchar_t* modeName = L"Normal";
            if (m_shadowDebugMode == ShadowDebugMode::ShadowMask)
                modeName = L"Shadow Mask";
            else if (m_shadowDebugMode == ShadowDebugMode::NoShadows)
                modeName = L"No Shadows";
            else if (m_shadowDebugMode == ShadowDebugMode::CascadeColors)
                modeName = L"Cascade Colors";
            else if (m_shadowDebugMode == ShadowDebugMode::CheckerShadows)
                modeName = L"Checker Shadows";

            wchar_t title[256];
            swprintf_s(title, L"Shadow Debug Mode: %s", modeName);
            SetWindowTextW(m_windowHandle, title);
        }
        m_prevBKey = m_inputDevice->IsKeyDown('B');
    }


    // Обновляем время воды
    m_waterTime += deltaTime;
    if (m_waterTime > 1000.0f)
        m_waterTime -= 1000.0f;

    if (m_mappedWaterConstantData)
    {
        m_mappedWaterConstantData->Time = m_waterTime;
        m_mappedWaterConstantData->WaveStrength = 0.6f;
        m_mappedWaterConstantData->WaveSpeed = 2.5f;
        m_mappedWaterConstantData->WaveFrequency = 1.5f;
    }

    float aspectRatio = (m_screenHeight > 0)
        ? static_cast<float>(m_screenWidth) / m_screenHeight
        : 1.0f;

    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    ConstantBufferData constantData = {};
    constantData.World = DirectX::XMMatrixTranspose(
        DirectX::XMMatrixScaling(0.1f, 0.1f, 0.1f) *
        DirectX::XMMatrixRotationY(0.0f));
    constantData.View = DirectX::XMMatrixTranspose(viewMatrix);
    constantData.Proj = DirectX::XMMatrixTranspose(projMatrix);
    constantData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
    constantData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constantData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, m_waterTime);
    constantData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
    constantData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);

    memcpy(m_mappedConstantData, &constantData, sizeof(constantData));

    // ВАЖНО: частицы здесь больше не обновляем.
    // Только сохраняем deltaTime, а GPU update будет в RenderDeferredFrame().
    m_particleDeltaTime = deltaTime;

    UpdateLightingConstants();
}

void DirectXApp::CreateCubeConstantBuffer()
{
    UINT64 bufferSize = sizeof(ConstantBufferData);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_cubeConstantBuffer)));

    D3D12_RANGE mapRange = { 0, 0 };
    ThrowIfFailed(m_cubeConstantBuffer->Map(0, &mapRange,
        reinterpret_cast<void**>(&m_mappedCubeConstantData)));
}

void DirectXApp::RenderGeometryPass()
{
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_screenWidth);
    viewport.Height = static_cast<float>(m_screenHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissorRect = { 0, 0, m_screenWidth, m_screenHeight };

    m_cmdList->RSSetViewports(1, &viewport);
    m_cmdList->RSSetScissorRects(1, &scissorRect);
    m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_gbuffer->BeginGeometryPass(m_cmdList.Get(), dsvHandle);

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    float aspectRatio = (m_screenHeight > 0) ? static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    ConstantBufferData globalData = {};
    globalData.World = DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity());
    globalData.View = DirectX::XMMatrixTranspose(viewMatrix);
    globalData.Proj = DirectX::XMMatrixTranspose(projMatrix);
    globalData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
    globalData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    globalData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
    globalData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
    globalData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);
    memcpy(m_mappedConstantData, &globalData, sizeof(ConstantBufferData));

    if (renderCubes) {

        UpdateFrustum();

        UpdateCubeColorsByDistance();

        std::vector<int> visibleIndices;
        switch (m_cullingMode)
        {
        case CullingMode::None:
            for (int i = 0; i < CUBE_COUNT; ++i)
                visibleIndices.push_back(i);
            break;

        case CullingMode::Frustum:
            for (int i = 0; i < CUBE_COUNT; ++i)
            {
                float radius = 0.866f * m_cubes[i].Scale * 1.5f;
                if (IsSphereInFrustum(m_cubes[i].Position, radius))
                    visibleIndices.push_back(i);
            }
            break;

        case CullingMode::Octree:
            if (m_kdTreeRoot)
                CullKDTree(m_kdTreeRoot.get(), m_frustum, visibleIndices);
            else
                for (int i = 0; i < CUBE_COUNT; ++i)
                    visibleIndices.push_back(i);
            break;
        }

        if (!visibleIndices.empty())
        {
            std::vector<int> visibleByLOD[3];
            for (int idx : visibleIndices)
            {
                float dx = m_cubes[idx].Position.x - cameraPos.x;
                float dy = m_cubes[idx].Position.y - cameraPos.y;
                float dz = m_cubes[idx].Position.z - cameraPos.z;
                float distance = sqrt(dx * dx + dy * dy + dz * dz);
                visibleByLOD[GetLODLevel(distance)].push_back(idx);
            }

            // LOD0 - полные кубы с текстурами (близко, < 30)
            if (!visibleByLOD[0].empty())
            {
                UpdateInstanceBufferForLOD(0, visibleByLOD[0]);
                m_cmdList->SetPipelineState(m_lod0PipelineState.Get());
                m_cmdList->IASetVertexBuffers(0, 1, &m_lod0VertexBufferView);
                m_cmdList->IASetVertexBuffers(1, 1, &m_instanceBufferViews[0]);
                m_cmdList->IASetIndexBuffer(&m_lod0IndexBufferView);
                m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
                m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
                m_cmdList->SetGraphicsRootConstantBufferView(2, m_tessellationCB->GetGPUVirtualAddress());
                m_cmdList->SetGraphicsRootConstantBufferView(3, m_waterConstantBuffer->GetGPUVirtualAddress());
                m_cmdList->DrawIndexedInstanced(m_lod0IndexCount, (UINT)visibleByLOD[0].size(), 0, 0, 0);
            }

            // LOD1 - упрощенные кубы (средне, 30-70)
            if (!visibleByLOD[1].empty())
            {
                UpdateInstanceBufferForLOD(1, visibleByLOD[1]);
                m_cmdList->SetPipelineState(m_lod1PipelineState.Get());
                m_cmdList->IASetVertexBuffers(0, 1, &m_lod1VertexBufferView);
                m_cmdList->IASetVertexBuffers(1, 1, &m_instanceBufferViews[1]);
                m_cmdList->IASetIndexBuffer(&m_lod1IndexBufferView);
                m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
                m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
                m_cmdList->SetGraphicsRootConstantBufferView(2, m_tessellationCB->GetGPUVirtualAddress());
                m_cmdList->SetGraphicsRootConstantBufferView(3, m_waterConstantBuffer->GetGPUVirtualAddress());
                m_cmdList->DrawIndexedInstanced(m_lod1IndexCount, (UINT)visibleByLOD[1].size(), 0, 0, 0);
            }

            // LOD2 - простые кубы (далеко, > 70)
            if (!visibleByLOD[2].empty())
            {
                UpdateInstanceBufferForLOD(2, visibleByLOD[2]);
                m_cmdList->SetPipelineState(m_lod2PipelineState.Get());
                m_cmdList->IASetVertexBuffers(0, 1, &m_lod2VertexBufferView);
                m_cmdList->IASetVertexBuffers(1, 1, &m_instanceBufferViews[2]);
                m_cmdList->IASetIndexBuffer(&m_lod2IndexBufferView);
                m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
                m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
                m_cmdList->SetGraphicsRootConstantBufferView(2, m_tessellationCB->GetGPUVirtualAddress());
                m_cmdList->SetGraphicsRootConstantBufferView(3, m_waterConstantBuffer->GetGPUVirtualAddress());
                m_cmdList->DrawIndexedInstanced(m_lod2IndexCount, (UINT)visibleByLOD[2].size(), 0, 0, 0);
            }

            static float debugTimer = 0.0f;
            debugTimer += 1.0f / 60.0f;
            if (debugTimer >= 1.0f)
            {
                debugTimer = 0.0f;
                const wchar_t* modeStr = L"None";
                switch (m_cullingMode)
                {
                case CullingMode::None:    modeStr = L"NO CULLING"; break;
                case CullingMode::Frustum: modeStr = L"FRUSTUM"; break;
                case CullingMode::Octree:  modeStr = L"OCTREE"; break;
                }
                wchar_t title[256];
                swprintf_s(title, L"[%s] LOD0:%d LOD1:%d LOD2:%d | Visible: %d/%d cubes",
                    modeStr,
                    (int)visibleByLOD[0].size(),
                    (int)visibleByLOD[1].size(),
                    (int)visibleByLOD[2].size(),
                    (int)visibleIndices.size(),
                    CUBE_COUNT);
                SetWindowTextW(m_windowHandle, title);
            }
        }
    }

    if (renderWater)
    {
        m_cmdList->SetPipelineState(m_waterTessPipeline.Get());

        float aspectRatio = (m_screenHeight > 0) ? static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
        DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
        DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);
        DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

        ConstantBufferData waterData = {};
        waterData.World = DirectX::XMMatrixTranspose(m_waterWorldMatrix);
        waterData.View = DirectX::XMMatrixTranspose(viewMatrix);
        waterData.Proj = DirectX::XMMatrixTranspose(projMatrix);
        waterData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
        waterData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        waterData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, m_waterTime);
        waterData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
        waterData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);

        memcpy(m_mappedConstantData, &waterData, sizeof(ConstantBufferData));

        m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
        m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        m_cmdList->SetGraphicsRootConstantBufferView(2, m_tessellationCB->GetGPUVirtualAddress());
        m_cmdList->SetGraphicsRootConstantBufferView(3, m_waterConstantBuffer->GetGPUVirtualAddress());

        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        m_cmdList->IASetVertexBuffers(0, 1, &m_waterVertexBufferView);
        m_cmdList->IASetIndexBuffer(&m_waterIndexBufferView);
        m_cmdList->DrawIndexedInstanced(m_waterIndexCount, 1, 0, 0, 0);
    }

    if (renderSponza)
    {
        TessellationConstants tessConsts;
        tessConsts.TessellationFactor = GetAdaptiveTessellationFactor();
        tessConsts.DisplacementStrength = 0.02f;
        tessConsts.TessMinDist = 0.0f;
        tessConsts.TessMaxDist = 30.0f;
        memcpy(m_mappedTessellationData, &tessConsts, sizeof(tessConsts));

        float aspectRatio = (m_screenHeight > 0) ? static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
        DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
        DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);
        DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

        ConstantBufferData palaceData = {};
        palaceData.World = DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(0.1f, 0.1f, 0.1f));
        palaceData.View = DirectX::XMMatrixTranspose(viewMatrix);
        palaceData.Proj = DirectX::XMMatrixTranspose(projMatrix);
        palaceData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
        palaceData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        palaceData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, m_waterTime);
        palaceData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
        palaceData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);

        memcpy(m_mappedConstantData, &palaceData, sizeof(ConstantBufferData));

        m_renderingSystem->RenderGeometryPass(
            m_cmdList.Get(),
            m_rootSignature.Get(),
            m_tessGeometryPSO.Get(),
            m_vertexBufferView,
            m_indexBufferView,
            m_indexCount,
            m_srvHeap.Get(),
            m_constantBuffer.Get(),
            m_tessellationCB.Get(),
            m_gbuffer.get(),
            dsvHandle);
    }

    m_gbuffer->EndGeometryPass(m_cmdList.Get());
}
void DirectXApp::RenderLightingPass()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    m_renderingSystem->RenderLightingPass(
        m_cmdList.Get(),
        m_deferredLightingRootSignature.Get(),
        m_deferredLightingPSO.Get(),
        m_gbuffer.get(),
        m_deferredLightConstantBuffer.Get(),
        rtvHandle,
        m_screenWidth, m_screenHeight,
        m_currentBackBuffer,
        m_rtvDescriptorSize);
}

void DirectXApp::RenderDeferredFrame()
{
    ThrowIfFailed(m_cmdAllocators[m_currentBackBuffer]->Reset());
    ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_currentBackBuffer].Get(), nullptr));

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_currentBackBuffer].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);

    UpdateParticles(m_particleDeltaTime);

    RenderShadowPass();

    // Важно: lighting constants обновляем после shadow pass,
    // потому что RenderShadowPass обновляет матрицы каскадов.
    UpdateLightingConstants();

    RenderGeometryPass();
    RenderLightingPass();
    RenderKDTreeLines();
    RenderParticles();

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_cmdList->Close());

    ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, commandLists);
}

void DirectXApp::Render()
{
    if (m_useDeferredRendering)
    {
        RenderDeferredFrame();
    }
    else
    {
        BuildCommandList();

        ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, commandLists);
    }

    ThrowIfFailed(m_swapChain->Present(1, 0));
    MoveToNextFrame();
}

void DirectXApp::BuildCommandList()
{
    ThrowIfFailed(m_cmdAllocators[m_currentBackBuffer]->Reset());
    ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_currentBackBuffer].Get(), m_tessPipeline.Get()));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // Transition
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_currentBackBuffer].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);

    // Clear
    const float clearColor[] = { 0.1f, 0.1f, 0.2f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set descriptors
    ID3D12DescriptorHeap* descriptorHeaps[] = { m_srvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, descriptorHeaps);
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set constant buffers
    m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

    // Set texture table
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

    // Set tessellation constant buffer
    m_cmdList->SetGraphicsRootConstantBufferView(2, m_tessellationCB->GetGPUVirtualAddress());

    // ========== РЕНДЕРИМ МОДЕЛЬ С ТЕССЕЛЯЦИЕЙ ==========
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_cmdList->IASetIndexBuffer(&m_indexBufferView);
    m_cmdList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    // Transition back
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_cmdList->Close());
}

void DirectXApp::Resize(int newWidth, int newHeight)
{
    if (newWidth <= 0 || newHeight <= 0) return;
    if (!m_swapChain || !m_d3dDevice) return;

    FlushCommandQueue();

    m_screenWidth = newWidth;
    m_screenHeight = newHeight;

    for (UINT i = 0; i < BACK_BUFFER_COUNT; ++i)
        m_backBuffers[i].Reset();
    m_depthStencil.Reset();

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    m_swapChain->GetDesc1(&swapChainDesc);
    ThrowIfFailed(m_swapChain->ResizeBuffers(
        BACK_BUFFER_COUNT,
        static_cast<UINT>(m_screenWidth),
        static_cast<UINT>(m_screenHeight),
        swapChainDesc.Format, 0));

    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargets();
    CreateDepthBuffer();

    if (m_gbuffer)
    {
        m_gbuffer->Shutdown();
        m_gbuffer->Initialize(m_d3dDevice.Get(), m_screenWidth, m_screenHeight);

        if (m_shadowMap)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;

            m_gbuffer->CreateExternalSRV(
                SHADOW_SRV_SLOT,
                m_shadowMap.Get(),
                srvDesc);
        }
    }
}

void DirectXApp::FlushCommandQueue()
{
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), m_fenceValues[m_currentBackBuffer]));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_currentBackBuffer], m_fenceEvent));
    ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    ++m_fenceValues[m_currentBackBuffer];
}

void DirectXApp::MoveToNextFrame()
{
    const UINT64 currentFence = m_fenceValues[m_currentBackBuffer];
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), currentFence));
    m_currentBackBuffer = m_swapChain->GetCurrentBackBufferIndex();

    if (m_fence->GetCompletedValue() < m_fenceValues[m_currentBackBuffer])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_currentBackBuffer], m_fenceEvent));
        ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }
    m_fenceValues[m_currentBackBuffer] = currentFence + 1;
}

void DirectXApp::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_cmdQueue)));
}

bool DirectXApp::Initialize()
{
    try
    {
        CreateD3DDevice();
        CreateCommandQueue();
        CreateSwapChain();
        CreateDescriptorHeaps();
        CreateRenderTargets();
        CreateDepthBuffer();
        CreateCommandList();
        CreateSyncObjects();
        CreateRootSignature();

        m_gbuffer = std::make_unique<GBuffer>();
        if (!m_gbuffer->Initialize(m_d3dDevice.Get(), m_screenWidth, m_screenHeight))
        {
            MessageBoxA(m_windowHandle, "Failed to initialize GBuffer!", "Error", MB_OK | MB_ICONERROR);
            return false;
        }

        m_renderingSystem = std::make_unique<RenderingSystem>();

        CreateDeferredRootSignatures();
        CreateLightingConstantBuffer();
        CreatePipelineState();
        CreateDeferredPipelines();
        CreateWaterTessellationPipeline();

        CreateShadowMap();
        CreateShadowPipeline();

        CreateTessellationPipeline();
        CreateTessellationGeometryPipeline();
        CreateTessellationConstantBuffer();

        ThrowIfFailed(m_cmdAllocators[0]->Reset());
        ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring workingDir(exePath);
            workingDir = workingDir.substr(0, workingDir.find_last_of(L"\\/") + 1);

            ObjResult modelData = LoadObj(workingDir + L"model.obj");
            if (modelData.valid)
            {
                BuildMeshBuffers(modelData.vertices, modelData.indices);
            }
            else
            {
                CreateDefaultGeometry();
                MessageBoxA(m_windowHandle, "Using default cube geometry", "Info", MB_OK);
            }

            TextureData texData0 = LoadTextureWIC(workingDir + L"texture_first.png");
            if (!texData0.valid)
                texData0 = LoadTextureWIC(workingDir + L"texture_first.jpg");
            if (texData0.valid)
            {
                UploadTexture(texData0, 0, false);
            }

            TextureData normalData = LoadTextureWIC(workingDir + L"normal.png");
            if (!normalData.valid)
                normalData = LoadTextureWIC(workingDir + L"normal.jpg");
            if (normalData.valid)
            {
                UploadTexture(normalData, 1, true);
                m_useNormalMap = true;
            }

            TextureData displacementData = LoadTextureWIC(workingDir + L"displacement.png");
            if (!displacementData.valid)
                displacementData = LoadTextureWIC(workingDir + L"displacement.jpg");
            if (displacementData.valid)
            {
                UploadTexture(displacementData, 2, false);
                m_useDisplacement = true;
            }

            TextureData texData3 = LoadTextureWIC(workingDir + L"texture_second.png");
            if (!texData3.valid)
                texData3 = LoadTextureWIC(workingDir + L"texture_second.jpg");
            if (texData3.valid)
            {
                UploadTexture(texData3, 3, false);
            }
        }

        CreateWaterPlane();
        CreateWaterPipelineState();
        CreateWaterConstantBuffer();

        // Создаем LOD меши для кубов
        CreateLOD0Mesh();  // Полный куб с текстурами
        CreateLOD1Mesh();  // Упрощенный куб (без текстур)
        CreateLOD2Mesh();  // Простой куб (только позиция)

        CreateCubeGeometry();
        CreateCubePipelineState();

        CreateInstancedPipeline();
        CreateInstanceBuffer();

        // Создаем 1000 кубов
        m_cubes.resize(CUBE_COUNT);
        m_cubeConstantBuffers.resize(CUBE_COUNT);
        m_mappedCubeConstantDataArray.resize(CUBE_COUNT);

        // Создаем константные буферы для всех 1000 кубов (fallback)
        for (int i = 0; i < CUBE_COUNT; ++i)
        {
            UINT64 bufferSize = sizeof(ConstantBufferData);
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = bufferSize;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.SampleDesc = { 1, 0 };
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&m_cubeConstantBuffers[i])));

            D3D12_RANGE mapRange = { 0, 0 };
            ThrowIfFailed(m_cubeConstantBuffers[i]->Map(0, &mapRange,
                reinterpret_cast<void**>(&m_mappedCubeConstantDataArray[i])));
        }

        // Генерируем 1000 кубов случайно
        std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
        std::uniform_real_distribution<float> posX(-50.0f, 50.0f);
        std::uniform_real_distribution<float> posZ(-50.0f, 50.0f);
        std::uniform_real_distribution<float> posY(-2.0f, 15.0f);
        std::uniform_real_distribution<float> scaleDist(1.2f, 4.5f);
        std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

        for (int i = 0; i < CUBE_COUNT; ++i)
        {
            float x = posX(rng);
            float z = posZ(rng);
            float y = posY(rng);
            float scale = scaleDist(rng);

            float r = colorDist(rng);
            float g = colorDist(rng);
            float b = colorDist(rng);

            m_cubes[i] = {
                DirectX::XMFLOAT3(x, y, z),
                scale,
                DirectX::XMFLOAT3(r, g, b)
            };
        }

        // Создаем AABB для каждого куба
        m_cubeAABBs.resize(CUBE_COUNT);
        for (int i = 0; i < CUBE_COUNT; ++i)
        {
            float half = m_cubes[i].Scale * 0.5f;
            m_cubeAABBs[i].Min = DirectX::XMFLOAT3(
                m_cubes[i].Position.x - half,
                m_cubes[i].Position.y - half,
                m_cubes[i].Position.z - half
            );
            m_cubeAABBs[i].Max = DirectX::XMFLOAT3(
                m_cubes[i].Position.x + half,
                m_cubes[i].Position.y + half,
                m_cubes[i].Position.z + half
            );
        }

        BuildKDTree();
        BuildKDTreeLines();
        CreateKDLinePipeline();

        InitializeParticleSystem();

        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, commandLists);
        FlushCommandQueue();

        CreateConstantBuffer();
        CreateShadowConstantBuffer();
        UpdateLightingConstants();

    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "Exception: ";
        errorMsg += e.what();
        MessageBoxA(m_windowHandle, errorMsg.c_str(), "Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    catch (...)
    {
        MessageBoxA(m_windowHandle, "Unknown exception occurred!", "Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}
void DirectXApp::CreateD3DDevice()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugLayer;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugLayer))))
        debugLayer->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory6> dxgiFactory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));

    ComPtr<IDXGIAdapter1> graphicsAdapter;
    for (UINT adapterIndex = 0; ; ++adapterIndex)
    {
        if (dxgiFactory->EnumAdapterByGpuPreference(
            adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&graphicsAdapter)) == DXGI_ERROR_NOT_FOUND)
            break;

        if (SUCCEEDED(D3D12CreateDevice(graphicsAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3dDevice))))
            return;
    }
    ThrowIfFailed(E_FAIL, "No compatible graphics device found");
}

void DirectXApp::CreateCubeGeometry()
{
    auto vertices = GenerateCubeMesh();
    auto indices = GenerateCubeIndices();

    m_cubeIndexCount = static_cast<UINT>(indices.size());

    UINT64 vertexBufferSize = vertices.size() * sizeof(Vertex);
    UINT64 indexBufferSize = indices.size() * sizeof(UINT);

    CreateUploadBuffer(vertices.data(), vertexBufferSize, m_cubeVertexBuffer);
    CreateUploadBuffer(indices.data(), indexBufferSize, m_cubeIndexBuffer);

    m_cubeVertexBufferView.BufferLocation = m_cubeVertexBuffer->GetGPUVirtualAddress();
    m_cubeVertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_cubeVertexBufferView.StrideInBytes = sizeof(Vertex);

    m_cubeIndexBufferView.BufferLocation = m_cubeIndexBuffer->GetGPUVirtualAddress();
    m_cubeIndexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_cubeIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

void DirectXApp::CreateCubePipelineState()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "CubeVSMain", "vs_5_0");
    auto ps = CompileShaderFile(filePath, "CubePSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blendDesc = {};
    for (UINT i = 0; i < 4; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = GBuffer::TargetCount;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_cubePipelineState)));
}
void DirectXApp::Shoot()
{
    if (!m_inputDevice)
        return;

    if (m_shootCooldown > 0.0f)
        return;

    m_shootCooldown = SHOOT_COOLDOWN_TIME;

    DirectX::XMFLOAT3 origin = m_camera.GetPosition();
    DirectX::XMFLOAT3 direction = m_camera.GetLookDirection();

    float len = sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (len > 0.0001f)
    {
        direction.x /= len;
        direction.y /= len;
        direction.z /= len;
    }

    float shootDistance = 15.0f;

    DirectX::XMFLOAT3 hitPoint;
    hitPoint.x = origin.x + direction.x * shootDistance;
    hitPoint.y = origin.y + direction.y * shootDistance;
    hitPoint.z = origin.z + direction.z * shootDistance;

    DynamicLight light1;
    light1.Position = hitPoint;
    light1.Intensity = 120.0f;
    light1.Range = 12.0f;
    light1.Active = true;
    light1.Color = DirectX::XMFLOAT3(1.0f, 0.4f, 0.1f);
    m_dynamicLights.push_back(light1);

    DirectX::XMFLOAT3 point2;
    point2.x = origin.x + direction.x * (shootDistance - 2.0f);
    point2.y = origin.y + direction.y * (shootDistance - 2.0f);
    point2.z = origin.z + direction.z * (shootDistance - 2.0f);

    DynamicLight light2;
    light2.Position = point2;
    light2.Intensity = 80.0f;
    light2.Range = 8.0f;
    light2.Active = true;
    light2.Color = DirectX::XMFLOAT3(1.0f, 0.6f, 0.2f);
    m_dynamicLights.push_back(light2);

    float maxDist = 30.0f;
    DirectX::XMFLOAT3 point3;
    point3.x = origin.x + direction.x * maxDist;
    point3.y = origin.y + direction.y * maxDist;
    point3.z = origin.z + direction.z * maxDist;

    DynamicLight light3;
    light3.Position = point3;
    light3.Intensity = 150.0f;
    light3.Range = 15.0f;
    light3.Active = true;
    light3.Color = DirectX::XMFLOAT3(1.0f, 0.2f, 0.2f);
    m_dynamicLights.push_back(light3);

    while (m_dynamicLights.size() > MAX_DYNAMIC_LIGHTS)
        m_dynamicLights.erase(m_dynamicLights.begin());

    UpdateLightingConstants();

    Beep(1000, 50);
}

void DirectXApp::CreateTessellationPipeline()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "VS", "vs_5_0");
    auto hs = CompileShaderFile(filePath, "HS", "hs_5_0");
    auto ds = CompileShaderFile(filePath, "DS", "ds_5_0");
    auto ps = CompileShaderFile(filePath, "PS", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // Обычный RASTERIZER_DESC (не CD3DX12)
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = 0;
    rasterizerDesc.DepthBiasClamp = 0.0f;
    rasterizerDesc.SlopeScaledDepthBias = 0.0f;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Обычный BLEND_DESC
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].LogicOpEnable = FALSE;
        blendDesc.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // Обычный DEPTH_STENCIL_DESC
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    depthStencilDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace = depthStencilDesc.FrontFace;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
    psoDesc.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_tessPipeline)));
}

void DirectXApp::CreateTessellationGeometryPipeline()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "TessVS", "vs_5_0");
    auto hs = CompileShaderFile(filePath, "TessHS", "hs_5_0");
    auto ds = CompileShaderFile(filePath, "TessDS", "ds_5_0");
    auto ps = CompileShaderFile(filePath, "TessGeometryPSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = 0;
    rasterizerDesc.DepthBiasClamp = 0.0f;
    rasterizerDesc.SlopeScaledDepthBias = 0.0f;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].LogicOpEnable = FALSE;
        blendDesc.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    depthStencilDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace = depthStencilDesc.FrontFace;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
    psoDesc.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    psoDesc.NumRenderTargets = GBuffer::TargetCount;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_tessGeometryPSO)));
}

void DirectXApp::CreateTessellationConstantBuffer()
{
    UINT64 bufferSize = (sizeof(TessellationConstants) + 255) & ~255;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_tessellationCB)));

    D3D12_RANGE mapRange = { 0, 0 };
    ThrowIfFailed(m_tessellationCB->Map(0, &mapRange, reinterpret_cast<void**>(&m_mappedTessellationData)));
}

float DirectXApp::GetAdaptiveTessellationFactor()
{
    DirectX::XMFLOAT3 modelCenter = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    // Расстояние
    float dx = cameraPos.x - modelCenter.x;
    float dy = cameraPos.y - modelCenter.y;
    float dz = cameraPos.z - modelCenter.z;
    float distance = sqrt(dx * dx + dy * dy + dz * dz);

    float maxTessFactor = 16.0f;
    float minTessFactor = 1.0f;
    float maxDistance = 50.0f;
    float minDistance = 5.0f;

    // Интерполяция
    float t = (distance - minDistance) / (maxDistance - minDistance);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float factor = maxTessFactor * (1.0f - t) + minTessFactor * t;
    return factor;
}

void DirectXApp::CreateWaterPlane()
{
    const int gridSize = 500;
    const float halfSize = 300.0f;
    const float step = (halfSize * 2.0f) / gridSize;

    std::vector<Vertex> vertices;
    std::vector<UINT> indices;

    for (int z = 0; z <= gridSize; ++z)
    {
        float zPos = -halfSize + z * step;
        for (int x = 0; x <= gridSize; ++x)
        {
            float xPos = -halfSize + x * step;

            Vertex v;
            v.Position = DirectX::XMFLOAT3(xPos, 0.0f, zPos);
            v.Normal = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
            v.Color = DirectX::XMFLOAT4(0.2f, 0.5f, 0.8f, 0.8f);
            v.TexCoord = DirectX::XMFLOAT2((float)x / gridSize, (float)z / gridSize);
            v.Tangent = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
            v.Binormal = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);

            vertices.push_back(v);
        }
    }

    for (int z = 0; z < gridSize; ++z)
    {
        for (int x = 0; x < gridSize; ++x)
        {
            int topLeft = z * (gridSize + 1) + x;
            int topRight = topLeft + 1;
            int bottomLeft = (z + 1) * (gridSize + 1) + x;
            int bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);

            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    m_waterIndexCount = static_cast<UINT>(indices.size());

    UINT64 vertexBufferSize = vertices.size() * sizeof(Vertex);
    UINT64 indexBufferSize = indices.size() * sizeof(UINT);

    CreateUploadBuffer(vertices.data(), vertexBufferSize, m_waterVertexBuffer);
    CreateUploadBuffer(indices.data(), indexBufferSize, m_waterIndexBuffer);

    m_waterVertexBufferView.BufferLocation = m_waterVertexBuffer->GetGPUVirtualAddress();
    m_waterVertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_waterVertexBufferView.StrideInBytes = sizeof(Vertex);

    m_waterIndexBufferView.BufferLocation = m_waterIndexBuffer->GetGPUVirtualAddress();
    m_waterIndexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_waterIndexBufferView.Format = DXGI_FORMAT_R32_UINT;

    m_waterWorldMatrix = DirectX::XMMatrixTranslation(20.0f, 20.0f, 20.0f);
}

void DirectXApp::CreateWaterPipelineState()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "WaterVSMain", "vs_5_0");
    auto ps = CompileShaderFile(filePath, "WaterPSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blendDesc = {};
    for (UINT i = 0; i < 4; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = GBuffer::TargetCount;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_waterPipelineState)));
}

void DirectXApp::CreateWaterConstantBuffer()
{
    UINT64 bufferSize = (sizeof(WaterConstantData) + 255) & ~255;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_waterConstantBuffer)));

    D3D12_RANGE mapRange = { 0, 0 };
    ThrowIfFailed(m_waterConstantBuffer->Map(0, &mapRange,
        reinterpret_cast<void**>(&m_mappedWaterConstantData)));

    if (m_mappedWaterConstantData)
    {
        m_mappedWaterConstantData->Time = 0.0f;
        m_mappedWaterConstantData->WaveStrength = 0.4f;
        m_mappedWaterConstantData->WaveSpeed = 2.0f;
        m_mappedWaterConstantData->WaveFrequency = 1.2f;
    }
}

void DirectXApp::CreateWaterTessellationPipeline()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "WaterTessVS", "vs_5_0");
    auto hs = CompileShaderFile(filePath, "WaterTessHS", "hs_5_0");
    auto ds = CompileShaderFile(filePath, "WaterTessDS", "ds_5_0");
    auto ps = CompileShaderFile(filePath, "WaterPSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blendDesc = {};
    for (UINT i = 0; i < 4; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
    psoDesc.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    psoDesc.NumRenderTargets = GBuffer::TargetCount;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_waterTessPipeline)));
}

void DirectXApp::UpdateFrustum()
{
    float aspectRatio = (m_screenHeight > 0) ?
        static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;

    XMMATRIX view = m_camera.GetViewMatrix();
    XMMATRIX proj = m_camera.GetProjectionMatrix(aspectRatio);
    XMMATRIX viewProj = view * proj;

    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);

    // Извлечение плоскостей для DirectX
    // Левая:   Col4 + Col1
    m_frustum.Planes[0] = XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41);
    // Правая:  Col4 - Col1
    m_frustum.Planes[1] = XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41);
    // Нижняя:  Col4 + Col2
    m_frustum.Planes[2] = XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42);
    // Верхняя: Col4 - Col2
    m_frustum.Planes[3] = XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42);
    // Ближняя: Col4 + Col3
    m_frustum.Planes[4] = XMFLOAT4(m._14 + m._13, m._24 + m._23, m._34 + m._33, m._44 + m._43);
    // Дальняя: Col4 - Col3
    m_frustum.Planes[5] = XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43);

    // Нормализация
    for (int i = 0; i < 6; ++i)
    {
        float len = sqrtf(m_frustum.Planes[i].x * m_frustum.Planes[i].x +
            m_frustum.Planes[i].y * m_frustum.Planes[i].y +
            m_frustum.Planes[i].z * m_frustum.Planes[i].z);
        if (len > 0.0001f)
        {
            float invLen = 1.0f / len;
            m_frustum.Planes[i].x *= invLen;
            m_frustum.Planes[i].y *= invLen;
            m_frustum.Planes[i].z *= invLen;
            m_frustum.Planes[i].w *= invLen;
        }
    }
}

bool DirectXApp::IsSphereInFrustum(const DirectX::XMFLOAT3& center, float radius) const
{
    for (int i = 0; i < 6; ++i)
    {
        float distance = m_frustum.Planes[i].x * center.x +
            m_frustum.Planes[i].y * center.y +
            m_frustum.Planes[i].z * center.z +
            m_frustum.Planes[i].w;

        if (distance < -radius)
            return false; // Полностью вне frustum
    }
    return true;
}


void DirectXApp::SetCullingMode(CullingMode mode)
{
    m_cullingMode = mode;

    const wchar_t* modeName = L"Unknown";
    switch (mode)
    {
    case CullingMode::None:    modeName = L"NO CULLING (All cubes)"; break;
    case CullingMode::Frustum: modeName = L"FRUSTUM CULLING"; break;
    case CullingMode::Octree:  modeName = L"OCTREE CULLING"; break;
    }

    wchar_t title[256];
    swprintf_s(title, L"Culling Mode: %s", modeName);
    SetWindowTextW(m_windowHandle, title);
}

bool DirectXApp::IsAABBInFrustum(const AABB& aabb) const
{
    for (int i = 0; i < 6; ++i)
    {
        const XMFLOAT4& plane = m_frustum.Planes[i];

        // Выбираем самую дальнюю вершину AABB в направлении плоскости
        float px = (plane.x > 0.0f) ? aabb.Max.x : aabb.Min.x;
        float py = (plane.y > 0.0f) ? aabb.Max.y : aabb.Min.y;
        float pz = (plane.z > 0.0f) ? aabb.Max.z : aabb.Min.z;

        float distance = plane.x * px + plane.y * py + plane.z * pz + plane.w;

        if (distance < 0.0f)
            return false;  // Полностью вне frustum
    }
    return true;  // Пересекает или внутри
}

void DirectXApp::BuildKDTree()
{
    // Находим общие границы всех кубов
    AABB worldBounds;
    worldBounds.Min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    worldBounds.Max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (int i = 0; i < CUBE_COUNT; ++i)
    {
        worldBounds.Min.x = min(worldBounds.Min.x, m_cubeAABBs[i].Min.x);
        worldBounds.Min.y = min(worldBounds.Min.y, m_cubeAABBs[i].Min.y);
        worldBounds.Min.z = min(worldBounds.Min.z, m_cubeAABBs[i].Min.z);
        worldBounds.Max.x = max(worldBounds.Max.x, m_cubeAABBs[i].Max.x);
        worldBounds.Max.y = max(worldBounds.Max.y, m_cubeAABBs[i].Max.y);
        worldBounds.Max.z = max(worldBounds.Max.z, m_cubeAABBs[i].Max.z);
    }

    std::vector<int> allIndices(CUBE_COUNT);
    for (int i = 0; i < CUBE_COUNT; ++i)
        allIndices[i] = i;

    m_kdTreeRoot = std::make_unique<KDTreeNode>();
    BuildKDTreeNode(m_kdTreeRoot, allIndices, worldBounds, 0);
}

void DirectXApp::BuildKDTreeNode(std::unique_ptr<KDTreeNode>& node,
    std::vector<int>& indices,
    const AABB& bounds, int depth)
{
    node->Bounds = bounds;
    node->Axis = depth % 3;  // 0=X, 1=Y, 2=Z
    node->IsLeaf = false;

    // Условия остановки: мало объектов или достигли максимальной глубины
    if (indices.size() <= MIN_CUBES_PER_NODE || depth >= MAX_KD_DEPTH)
    {
        node->IsLeaf = true;
        node->CubeIndices = std::move(indices);
        return;
    }

    // Сортируем по центру AABB (более точно, чем по позиции)
    if (node->Axis == 0) // X
    {
        std::sort(indices.begin(), indices.end(), [this](int a, int b) {
            float centerA = (m_cubeAABBs[a].Min.x + m_cubeAABBs[a].Max.x) * 0.5f;
            float centerB = (m_cubeAABBs[b].Min.x + m_cubeAABBs[b].Max.x) * 0.5f;
            return centerA < centerB;
            });
    }
    else if (node->Axis == 1) // Y
    {
        std::sort(indices.begin(), indices.end(), [this](int a, int b) {
            float centerA = (m_cubeAABBs[a].Min.y + m_cubeAABBs[a].Max.y) * 0.5f;
            float centerB = (m_cubeAABBs[b].Min.y + m_cubeAABBs[b].Max.y) * 0.5f;
            return centerA < centerB;
            });
    }
    else // Z
    {
        std::sort(indices.begin(), indices.end(), [this](int a, int b) {
            float centerA = (m_cubeAABBs[a].Min.z + m_cubeAABBs[a].Max.z) * 0.5f;
            float centerB = (m_cubeAABBs[b].Min.z + m_cubeAABBs[b].Max.z) * 0.5f;
            return centerA < centerB;
            });
    }

    // Медиана
    size_t mid = indices.size() / 2;

    // Получаем позицию разделения
    if (node->Axis == 0)
        node->SplitPos = (m_cubeAABBs[indices[mid]].Min.x + m_cubeAABBs[indices[mid]].Max.x) * 0.5f;
    else if (node->Axis == 1)
        node->SplitPos = (m_cubeAABBs[indices[mid]].Min.y + m_cubeAABBs[indices[mid]].Max.y) * 0.5f;
    else
        node->SplitPos = (m_cubeAABBs[indices[mid]].Min.z + m_cubeAABBs[indices[mid]].Max.z) * 0.5f;

    // Разделяем индексы
    std::vector<int> leftIndices, rightIndices;
    for (int idx : indices)
    {
        float center;
        if (node->Axis == 0)
            center = (m_cubeAABBs[idx].Min.x + m_cubeAABBs[idx].Max.x) * 0.5f;
        else if (node->Axis == 1)
            center = (m_cubeAABBs[idx].Min.y + m_cubeAABBs[idx].Max.y) * 0.5f;
        else
            center = (m_cubeAABBs[idx].Min.z + m_cubeAABBs[idx].Max.z) * 0.5f;

        if (center < node->SplitPos)
            leftIndices.push_back(idx);
        else
            rightIndices.push_back(idx);
    }

    // Создаем границы для детей
    AABB leftBounds = bounds;
    AABB rightBounds = bounds;

    if (node->Axis == 0)
    {
        leftBounds.Max.x = node->SplitPos;
        rightBounds.Min.x = node->SplitPos;
    }
    else if (node->Axis == 1)
    {
        leftBounds.Max.y = node->SplitPos;
        rightBounds.Min.y = node->SplitPos;
    }
    else
    {
        leftBounds.Max.z = node->SplitPos;
        rightBounds.Min.z = node->SplitPos;
    }

    // Рекурсивно строим детей
    if (!leftIndices.empty())
    {
        node->Left = std::make_unique<KDTreeNode>();
        BuildKDTreeNode(node->Left, leftIndices, leftBounds, depth + 1);
    }
    if (!rightIndices.empty())
    {
        node->Right = std::make_unique<KDTreeNode>();
        BuildKDTreeNode(node->Right, rightIndices, rightBounds, depth + 1);
    }
}

bool DirectXApp::IsAABBInFrustum(const AABB& aabb, const Frustum& frustum) const
{
    for (int i = 0; i < 6; ++i)
    {
        const XMFLOAT4& plane = frustum.Planes[i];

        float px = (plane.x > 0.0f) ? aabb.Max.x : aabb.Min.x;
        float py = (plane.y > 0.0f) ? aabb.Max.y : aabb.Min.y;
        float pz = (plane.z > 0.0f) ? aabb.Max.z : aabb.Min.z;

        float distance = plane.x * px + plane.y * py + plane.z * pz + plane.w;

        if (distance < 0.0f)
            return false;
    }
    return true;
}

void DirectXApp::CullKDTree(KDTreeNode* node, const Frustum& frustum, std::vector<int>& outVisible)
{
    if (!node) return;

    // Используем переданный frustum
    if (!IsAABBInFrustum(node->Bounds, frustum))
        return;

    if (node->IsLeaf)
    {
        for (int idx : node->CubeIndices)
        {
            if (IsAABBInFrustum(m_cubeAABBs[idx], frustum))
                outVisible.push_back(idx);
        }
    }
    else
    {
        CullKDTree(node->Left.get(), frustum, outVisible);
        CullKDTree(node->Right.get(), frustum, outVisible);
    }
}

void DirectXApp::CreateInstancedPipeline()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    // ========== LOD0 Pipeline (full vertex) ==========
    {
        auto vs = CompileShaderFile(filePath, "CubeVSInstanced", "vs_5_0");
        auto ps = CompileShaderFile(filePath, "CubePSInstanced", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"INSTANCEPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCESCALE", 0, DXGI_FORMAT_R32_FLOAT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCECOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCEPADDING", 0, DXGI_FORMAT_R32_FLOAT, 1, 28, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        };

        D3D12_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
        rasterizerDesc.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
        depthStencilDesc.DepthEnable = TRUE;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        D3D12_BLEND_DESC blendDesc = {};
        for (UINT i = 0; i < 8; ++i)
        {
            blendDesc.RenderTarget[i].BlendEnable = FALSE;
            blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        psoDesc.RasterizerState = rasterizerDesc;
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = depthStencilDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = GBuffer::TargetCount;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_lod0PipelineState)));
    }

    // ========== LOD1 Pipeline (simplified vertex - no tangent/binormal) ==========
    {
        auto vs = CompileShaderFile(filePath, "LOD1VSInstanced", "vs_5_0");
        auto ps = CompileShaderFile(filePath, "LOD1PSInstanced", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"INSTANCEPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCESCALE", 0, DXGI_FORMAT_R32_FLOAT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCECOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCEPADDING", 0, DXGI_FORMAT_R32_FLOAT, 1, 28, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        };

        D3D12_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
        rasterizerDesc.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
        depthStencilDesc.DepthEnable = TRUE;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        D3D12_BLEND_DESC blendDesc = {};
        for (UINT i = 0; i < 8; ++i)
        {
            blendDesc.RenderTarget[i].BlendEnable = FALSE;
            blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        psoDesc.RasterizerState = rasterizerDesc;
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = depthStencilDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = GBuffer::TargetCount;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_lod1PipelineState)));
    }

    // ========== LOD2 Pipeline (position only) ==========
    {
        auto vs = CompileShaderFile(filePath, "LOD2VSInstanced", "vs_5_0");
        auto ps = CompileShaderFile(filePath, "LOD2PSInstanced", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"INSTANCEPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCESCALE", 0, DXGI_FORMAT_R32_FLOAT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCECOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"INSTANCEPADDING", 0, DXGI_FORMAT_R32_FLOAT, 1, 28, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        };

        D3D12_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
        rasterizerDesc.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
        depthStencilDesc.DepthEnable = TRUE;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        D3D12_BLEND_DESC blendDesc = {};
        for (UINT i = 0; i < 8; ++i)
        {
            blendDesc.RenderTarget[i].BlendEnable = FALSE;
            blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        psoDesc.RasterizerState = rasterizerDesc;
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = depthStencilDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = GBuffer::TargetCount;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R32_FLOAT;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_lod2PipelineState)));
    }
}
void DirectXApp::UpdateInstanceBuffer(const std::vector<int>& visibleIndices)
{
    // Заполняем данные только для видимых кубов
    for (size_t i = 0; i < visibleIndices.size(); ++i)
    {
        int idx = visibleIndices[i];
        m_visibleInstanceData[i].Position = m_cubes[idx].Position;
        m_visibleInstanceData[i].Scale = m_cubes[idx].Scale;
        m_visibleInstanceData[i].Color = m_cubes[idx].Color;
        m_visibleInstanceData[i].Padding = 0.0f;
    }

    UINT64 dataSize = visibleIndices.size() * sizeof(InstanceData);

    // Копируем в upload buffer
    void* mappedData = nullptr;
    m_instanceUploadBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, m_visibleInstanceData.data(), (size_t)dataSize);
    m_instanceUploadBuffer->Unmap(0, nullptr);

    // Копируем в default buffer
    m_cmdList->CopyBufferRegion(m_instanceBuffer.Get(), 0,
        m_instanceUploadBuffer.Get(), 0, dataSize);
}

void DirectXApp::CreateKDLinePipeline()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "LineVS", "vs_5_0");
    auto ps = CompileShaderFile(filePath, "LinePS", "ps_5_0");

    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.Descriptor.RegisterSpace = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParam;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) MessageBoxA(m_windowHandle, (char*)errorBlob->GetBufferPointer(), "Root Sig Error", MB_OK);
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_d3dDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_kdLineRootSignature)));

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = 0;
    rasterizerDesc.DepthBiasClamp = 0.0f;
    rasterizerDesc.SlopeScaledDepthBias = 0.0f;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i)
    {
        blendDesc.RenderTarget[i].BlendEnable = FALSE;
        blendDesc.RenderTarget[i].LogicOpEnable = FALSE;
        blendDesc.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_kdLineRootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_kdLinePSO)));

    UINT64 cbSize = (sizeof(DirectX::XMMATRIX) + 255) & ~255;
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = cbSize;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc = { 1, 0 };
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_kdLineConstantBuffer)));
    ThrowIfFailed(m_kdLineConstantBuffer->Map(0, nullptr, &m_mappedKDLineCB));
}

void DirectXApp::RenderKDTreeLines()
{
    if (m_cullingMode != CullingMode::Octree || m_kdLineVertexCount == 0)
        return;

    m_cmdList->SetPipelineState(m_kdLinePSO.Get());
    m_cmdList->SetGraphicsRootSignature(m_kdLineRootSignature.Get());

    float aspectRatio = (m_screenHeight > 0) ? static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
    DirectX::XMMATRIX view = m_camera.GetViewMatrix();
    DirectX::XMMATRIX proj = m_camera.GetProjectionMatrix(aspectRatio);
    DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(view, proj);
    viewProj = DirectX::XMMatrixTranspose(viewProj);
    memcpy(m_mappedKDLineCB, &viewProj, sizeof(DirectX::XMMATRIX));

    m_cmdList->SetGraphicsRootConstantBufferView(0, m_kdLineConstantBuffer->GetGPUVirtualAddress());
    m_cmdList->IASetVertexBuffers(0, 1, &m_kdLineBufferView);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    m_cmdList->DrawInstanced(m_kdLineVertexCount, 1, 0, 0);
}

void DirectXApp::BuildKDTreeLines()
{
    if (!m_kdTreeRoot) return;

    struct LineVertex { DirectX::XMFLOAT3 Position; DirectX::XMFLOAT4 Color; };
    std::vector<LineVertex> vertices;

    std::function<void(KDTreeNode*)> collect = [&](KDTreeNode* node) {
        if (!node) return;

        const AABB& b = node->Bounds;
        DirectX::XMFLOAT3 min = b.Min;
        DirectX::XMFLOAT3 max = b.Max;

        DirectX::XMFLOAT4 color(0.2f, 0.8f, 0.2f, 1.0f);

        vertices.push_back({ {min.x, min.y, min.z}, color });
        vertices.push_back({ {max.x, min.y, min.z}, color });

        vertices.push_back({ {max.x, min.y, min.z}, color });
        vertices.push_back({ {max.x, min.y, max.z}, color });

        vertices.push_back({ {max.x, min.y, max.z}, color });
        vertices.push_back({ {min.x, min.y, max.z}, color });

        vertices.push_back({ {min.x, min.y, max.z}, color });
        vertices.push_back({ {min.x, min.y, min.z}, color });

        vertices.push_back({ {min.x, max.y, min.z}, color });
        vertices.push_back({ {max.x, max.y, min.z}, color });

        vertices.push_back({ {max.x, max.y, min.z}, color });
        vertices.push_back({ {max.x, max.y, max.z}, color });

        vertices.push_back({ {max.x, max.y, max.z}, color });
        vertices.push_back({ {min.x, max.y, max.z}, color });

        vertices.push_back({ {min.x, max.y, max.z}, color });
        vertices.push_back({ {min.x, max.y, min.z}, color });

        vertices.push_back({ {min.x, min.y, min.z}, color });
        vertices.push_back({ {min.x, max.y, min.z}, color });

        vertices.push_back({ {max.x, min.y, min.z}, color });
        vertices.push_back({ {max.x, max.y, min.z}, color });

        vertices.push_back({ {max.x, min.y, max.z}, color });
        vertices.push_back({ {max.x, max.y, max.z}, color });

        vertices.push_back({ {min.x, min.y, max.z}, color });
        vertices.push_back({ {min.x, max.y, max.z}, color });

        collect(node->Left.get());
        collect(node->Right.get());
        };

    collect(m_kdTreeRoot.get());

    m_kdLineVertexCount = (UINT)vertices.size();
    if (m_kdLineVertexCount == 0) return;

    UINT64 bufferSize = vertices.size() * sizeof(LineVertex);
    CreateUploadBuffer(vertices.data(), bufferSize, m_kdLineVertexBuffer);

    m_kdLineBufferView.BufferLocation = m_kdLineVertexBuffer->GetGPUVirtualAddress();
    m_kdLineBufferView.SizeInBytes = static_cast<UINT>(bufferSize);
    m_kdLineBufferView.StrideInBytes = sizeof(LineVertex);
}

void DirectXApp::UpdateCubeColorsByDistance()
{
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();
    float maxDistance = 80.0f;

    for (int i = 0; i < CUBE_COUNT; ++i)
    {
        float dx = m_cubes[i].Position.x - cameraPos.x;
        float dy = m_cubes[i].Position.y - cameraPos.y;
        float dz = m_cubes[i].Position.z - cameraPos.z;
        float distance = sqrt(dx * dx + dy * dy + dz * dz);

        float t = min(1.0f, distance / maxDistance);

        float hue = t * 0.7f;

        float r, g, b;
        int segment = (int)(hue * 6);
        float f = hue * 6 - segment;

        switch (segment)
        {
        case 0: r = 1; g = f; b = 0; break;
        case 1: r = 1 - f; g = 1; b = 0; break;
        case 2: r = 0; g = 1; b = f; break;
        case 3: r = 0; g = 1 - f; b = 1; break;
        case 4: r = f; g = 0; b = 1; break;
        default: r = 1; g = 0; b = 1 - f; break;
        }

        m_cubes[i].Color = DirectX::XMFLOAT3(r, g, b);
    }
}

// LOD0: Полный куб (как сейчас)
void DirectXApp::CreateLOD0Mesh()
{
    auto vertices = GenerateCubeMesh();
    auto indices = GenerateCubeIndices();

    m_lod0IndexCount = static_cast<UINT>(indices.size());

    UINT64 vertexBufferSize = vertices.size() * sizeof(Vertex);
    UINT64 indexBufferSize = indices.size() * sizeof(UINT);

    CreateUploadBuffer(vertices.data(), vertexBufferSize, m_lod0VertexBuffer);
    CreateUploadBuffer(indices.data(), indexBufferSize, m_lod0IndexBuffer);

    m_lod0VertexBufferView.BufferLocation = m_lod0VertexBuffer->GetGPUVirtualAddress();
    m_lod0VertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_lod0VertexBufferView.StrideInBytes = sizeof(Vertex);

    m_lod0IndexBufferView.BufferLocation = m_lod0IndexBuffer->GetGPUVirtualAddress();
    m_lod0IndexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_lod0IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

// LOD1: Упрощенный куб (только позиция, нормаль, цвет - без текстур и tangent)
void DirectXApp::CreateLOD1Mesh()
{
    struct LOD1Vertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT4 Color;
    };

    float halfSize = 0.5f;

    std::vector<LOD1Vertex> vertices;
    std::vector<UINT> indices;

    vertices.push_back({ {-halfSize, -halfSize,  halfSize}, {0,0,1}, {1,1,1,1} });
    vertices.push_back({ { halfSize, -halfSize,  halfSize}, {0,0,1}, {1,1,1,1} });
    vertices.push_back({ { halfSize,  halfSize,  halfSize}, {0,0,1}, {1,1,1,1} });
    vertices.push_back({ {-halfSize,  halfSize,  halfSize}, {0,0,1}, {1,1,1,1} });

    vertices.push_back({ { halfSize, -halfSize, -halfSize}, {0,0,-1}, {1,1,1,1} });
    vertices.push_back({ {-halfSize, -halfSize, -halfSize}, {0,0,-1}, {1,1,1,1} });
    vertices.push_back({ {-halfSize,  halfSize, -halfSize}, {0,0,-1}, {1,1,1,1} });
    vertices.push_back({ { halfSize,  halfSize, -halfSize}, {0,0,-1}, {1,1,1,1} });

    vertices.push_back({ {-halfSize, -halfSize, -halfSize}, {-1,0,0}, {1,1,1,1} });
    vertices.push_back({ {-halfSize, -halfSize,  halfSize}, {-1,0,0}, {1,1,1,1} });
    vertices.push_back({ {-halfSize,  halfSize,  halfSize}, {-1,0,0}, {1,1,1,1} });
    vertices.push_back({ {-halfSize,  halfSize, -halfSize}, {-1,0,0}, {1,1,1,1} });

    vertices.push_back({ { halfSize, -halfSize,  halfSize}, {1,0,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize, -halfSize, -halfSize}, {1,0,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize,  halfSize, -halfSize}, {1,0,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize,  halfSize,  halfSize}, {1,0,0}, {1,1,1,1} });

    vertices.push_back({ {-halfSize,  halfSize,  halfSize}, {0,1,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize,  halfSize,  halfSize}, {0,1,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize,  halfSize, -halfSize}, {0,1,0}, {1,1,1,1} });
    vertices.push_back({ {-halfSize,  halfSize, -halfSize}, {0,1,0}, {1,1,1,1} });

    vertices.push_back({ {-halfSize, -halfSize, -halfSize}, {0,-1,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize, -halfSize, -halfSize}, {0,-1,0}, {1,1,1,1} });
    vertices.push_back({ { halfSize, -halfSize,  halfSize}, {0,-1,0}, {1,1,1,1} });
    vertices.push_back({ {-halfSize, -halfSize,  halfSize}, {0,-1,0}, {1,1,1,1} });

    for (UINT face = 0; face < 6; ++face)
    {
        UINT baseIndex = face * 4;
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 3);
    }

    m_lod1IndexCount = static_cast<UINT>(indices.size());

    UINT64 vertexBufferSize = vertices.size() * sizeof(LOD1Vertex);
    UINT64 indexBufferSize = indices.size() * sizeof(UINT);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = vertexBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lod1VertexBuffer)));

    void* mappedData = nullptr;
    m_lod1VertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices.data(), (size_t)vertexBufferSize);
    m_lod1VertexBuffer->Unmap(0, nullptr);

    bufferDesc.Width = indexBufferSize;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lod1IndexBuffer)));

    m_lod1IndexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, indices.data(), (size_t)indexBufferSize);
    m_lod1IndexBuffer->Unmap(0, nullptr);

    m_lod1VertexBufferView.BufferLocation = m_lod1VertexBuffer->GetGPUVirtualAddress();
    m_lod1VertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_lod1VertexBufferView.StrideInBytes = sizeof(LOD1Vertex);

    m_lod1IndexBufferView.BufferLocation = m_lod1IndexBuffer->GetGPUVirtualAddress();
    m_lod1IndexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_lod1IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

// LOD2: Самый простой куб (только позиция)
void DirectXApp::CreateLOD2Mesh()
{
    struct LOD2Vertex
    {
        DirectX::XMFLOAT3 Position;
    };

    float halfSize = 0.5f;

    std::vector<LOD2Vertex> vertices;
    std::vector<UINT> indices;

    vertices.push_back({ {-halfSize, -halfSize, -halfSize} });
    vertices.push_back({ { halfSize, -halfSize, -halfSize} });
    vertices.push_back({ { halfSize, -halfSize,  halfSize} });
    vertices.push_back({ {-halfSize, -halfSize,  halfSize} });
    vertices.push_back({ {-halfSize,  halfSize, -halfSize} });
    vertices.push_back({ { halfSize,  halfSize, -halfSize} });
    vertices.push_back({ { halfSize,  halfSize,  halfSize} });
    vertices.push_back({ {-halfSize,  halfSize,  halfSize} });

    indices = {
        0,1,2, 0,2,3,
        4,6,5, 4,7,6,
        0,4,1, 1,4,5,
        1,5,2, 2,5,6,
        2,6,3, 3,6,7,
        3,7,0, 0,7,4
    };

    m_lod2IndexCount = static_cast<UINT>(indices.size());

    UINT64 vertexBufferSize = vertices.size() * sizeof(LOD2Vertex);
    UINT64 indexBufferSize = indices.size() * sizeof(UINT);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = vertexBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lod2VertexBuffer)));

    void* mappedData = nullptr;
    m_lod2VertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices.data(), (size_t)vertexBufferSize);
    m_lod2VertexBuffer->Unmap(0, nullptr);

    bufferDesc.Width = indexBufferSize;
    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lod2IndexBuffer)));

    m_lod2IndexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, indices.data(), (size_t)indexBufferSize);
    m_lod2IndexBuffer->Unmap(0, nullptr);

    m_lod2VertexBufferView.BufferLocation = m_lod2VertexBuffer->GetGPUVirtualAddress();
    m_lod2VertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_lod2VertexBufferView.StrideInBytes = sizeof(LOD2Vertex);

    m_lod2IndexBufferView.BufferLocation = m_lod2IndexBuffer->GetGPUVirtualAddress();
    m_lod2IndexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_lod2IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

// Определение LOD уровня по расстоянию
int DirectXApp::GetLODLevel(float distance)
{
    if (distance < 30.0f) return 0;      // LOD0 - полный куб
    if (distance < 70.0f) return 1;      // LOD1 - упрощенный куб
    return 2;                             // LOD2 - простой куб (только позиция)
}

void DirectXApp::CreateInstanceBuffer()
{
    UINT64 bufferSize = sizeof(InstanceData) * CUBE_COUNT;

    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    for (int lod = 0; lod < 3; ++lod)
    {
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
            &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr,
            IID_PPV_ARGS(&m_instanceBuffers[lod])));

        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
            &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_instanceUploadBuffers[lod])));

        m_instanceBufferViews[lod].BufferLocation = m_instanceBuffers[lod]->GetGPUVirtualAddress();
        m_instanceBufferViews[lod].SizeInBytes = static_cast<UINT>(bufferSize);
        m_instanceBufferViews[lod].StrideInBytes = sizeof(InstanceData);
    }

    m_visibleInstanceData.resize(CUBE_COUNT);
}

void DirectXApp::UpdateInstanceBufferForLOD(int lod, const std::vector<int>& indices)
{
    if (indices.empty()) return;

    for (size_t i = 0; i < indices.size(); ++i)
    {
        int idx = indices[i];
        m_visibleInstanceData[i].Position = m_cubes[idx].Position;
        m_visibleInstanceData[i].Scale = m_cubes[idx].Scale;
        m_visibleInstanceData[i].Color = m_cubes[idx].Color;
        m_visibleInstanceData[i].Padding = 0.0f;
    }

    UINT64 dataSize = indices.size() * sizeof(InstanceData);

    void* mappedData = nullptr;
    m_instanceUploadBuffers[lod]->Map(0, nullptr, &mappedData);
    memcpy(mappedData, m_visibleInstanceData.data(), (size_t)dataSize);
    m_instanceUploadBuffers[lod]->Unmap(0, nullptr);

    m_cmdList->CopyBufferRegion(m_instanceBuffers[lod].Get(), 0,
        m_instanceUploadBuffers[lod].Get(), 0, dataSize);
}

// ========== PARTICLE SYSTEM IMPLEMENTATION ==========

void DirectXApp::CreateParticleQuadGeometry()
{
    struct QuadVertex { DirectX::XMFLOAT2 Corner; };
    QuadVertex vertices[4] = {
        { {-0.5f, -0.5f} },
        { {-0.5f,  0.5f} },
        { { 0.5f,  0.5f} },
        { { 0.5f, -0.5f} }
    };
    uint16_t indices[6] = { 0,1,2, 0,2,3 };

    CreateUploadBuffer(vertices, sizeof(vertices), m_particleQuadVB);
    CreateUploadBuffer(indices, sizeof(indices), m_particleQuadIB);

    m_particleQuadVbView.BufferLocation = m_particleQuadVB->GetGPUVirtualAddress();
    m_particleQuadVbView.StrideInBytes = sizeof(QuadVertex);
    m_particleQuadVbView.SizeInBytes = sizeof(vertices);

    m_particleQuadIbView.BufferLocation = m_particleQuadIB->GetGPUVirtualAddress();
    m_particleQuadIbView.Format = DXGI_FORMAT_R16_UINT;
    m_particleQuadIbView.SizeInBytes = sizeof(indices);
}

void DirectXApp::CreateParticleRootSignatures()
{
    // Compute Root Signature
    {
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 3;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &uavRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_d3dDevice->CreateRootSignature(
            0,
            sig->GetBufferPointer(),
            sig->GetBufferSize(),
            IID_PPV_ARGS(&m_particleComputeRS)));
    }

    // Render Root Signature
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 2; // t0 = particle pool, t1 = sort list
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_d3dDevice->CreateRootSignature(
            0,
            sig->GetBufferPointer(),
            sig->GetBufferSize(),
            IID_PPV_ARGS(&m_particleRenderRS)));
    }
}

void DirectXApp::CreateParticlePipelines()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    const D3D_SHADER_MACRO initDefines[] =
    {
        { "PARTICLE_INIT", "1" },
        { nullptr, nullptr }
    };

    const D3D_SHADER_MACRO emitDefines[] =
    {
        { "PARTICLE_EMIT", "1" },
        { nullptr, nullptr }
    };

    const D3D_SHADER_MACRO updateDefines[] =
    {
        { "PARTICLE_UPDATE", "1" },
        { nullptr, nullptr }
    };

    const D3D_SHADER_MACRO sortDefines[] =
    {
        { "PARTICLE_SORT", "1" },
        { nullptr, nullptr }
    };

    const D3D_SHADER_MACRO clearSortDefines[] =
    {
        { "PARTICLE_CLEAR_SORT", "1" },
        { nullptr, nullptr }
    };

    auto initCS = CompileShaderFile(filePath, "ParticleInitDeadListCS", "cs_5_0", initDefines);
    auto emitCS = CompileShaderFile(filePath, "ParticleEmitCS", "cs_5_0", emitDefines);
    auto updateCS = CompileShaderFile(filePath, "ParticleUpdateCS", "cs_5_0", updateDefines);
    auto sortCS = CompileShaderFile(filePath, "BitonicSortCS", "cs_5_0", sortDefines);
    auto clearSortCS = CompileShaderFile(filePath, "ClearSortListCS", "cs_5_0", clearSortDefines);

    auto vs = CompileShaderFile(filePath, "ParticleVSMain", "vs_5_0");
    auto gs = CompileShaderFile(filePath, "ParticleGSMain", "gs_5_0");
    auto ps = CompileShaderFile(filePath, "ParticlePSMain", "ps_5_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpsd = {};
    cpsd.pRootSignature = m_particleComputeRS.Get();

    cpsd.CS = { initCS->GetBufferPointer(), initCS->GetBufferSize() };
    ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(&m_particleInitDeadPSO)));

    cpsd.CS = { emitCS->GetBufferPointer(), emitCS->GetBufferSize() };
    ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(&m_particleEmitPSO)));

    cpsd.CS = { updateCS->GetBufferPointer(), updateCS->GetBufferSize() };
    ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(&m_particleUpdatePSO)));

    cpsd.CS = { sortCS->GetBufferPointer(), sortCS->GetBufferSize() };
    ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(&m_particleSortPSO)));

    cpsd.CS = { clearSortCS->GetBufferPointer(), clearSortCS->GetBufferSize() };
    ThrowIfFailed(m_d3dDevice->CreateComputePipelineState(
        &cpsd,
        IID_PPV_ARGS(&m_particleClearSortPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpsd = {};
    gpsd.InputLayout = { nullptr, 0 };
    gpsd.pRootSignature = m_particleRenderRS.Get();
    gpsd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    gpsd.GS = { gs->GetBufferPointer(), gs->GetBufferSize() };
    gpsd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

    gpsd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpsd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpsd.RasterizerState.FrontCounterClockwise = FALSE;
    gpsd.RasterizerState.DepthBias = 0;
    gpsd.RasterizerState.DepthBiasClamp = 0.0f;
    gpsd.RasterizerState.SlopeScaledDepthBias = 0.0f;
    gpsd.RasterizerState.DepthClipEnable = TRUE;
    gpsd.RasterizerState.MultisampleEnable = FALSE;
    gpsd.RasterizerState.AntialiasedLineEnable = FALSE;
    gpsd.RasterizerState.ForcedSampleCount = 0;
    gpsd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    gpsd.BlendState.AlphaToCoverageEnable = FALSE;
    gpsd.BlendState.IndependentBlendEnable = FALSE;
    gpsd.BlendState.RenderTarget[0].BlendEnable = TRUE;
    gpsd.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    gpsd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    gpsd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    gpsd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    gpsd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    gpsd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    gpsd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    gpsd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    gpsd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    gpsd.DepthStencilState.DepthEnable = TRUE;
    gpsd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gpsd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    gpsd.DepthStencilState.StencilEnable = FALSE;

    gpsd.SampleMask = UINT_MAX;
    gpsd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gpsd.NumRenderTargets = 1;
    gpsd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpsd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gpsd.SampleDesc.Count = 1;
    gpsd.SampleDesc.Quality = 0;

    for (UINT i = 1; i < 8; ++i)
        gpsd.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&gpsd, IID_PPV_ARGS(&m_particleRenderPSO)));
}

void DirectXApp::CreateParticleResources()
{
    // Куча создаётся в CreateDescriptorHeaps()

    auto makeBuffer = [&](UINT64 size, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& out, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr, IID_PPV_ARGS(&out)));
        };

    makeBuffer(MAX_PARTICLES * sizeof(GpuParticle), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_particlePool);
    makeBuffer(MAX_PARTICLES * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_deadList);
    makeBuffer(MAX_PARTICLES * sizeof(ParticleSortEntry), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_sortList);
    makeBuffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_deadListCounter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    makeBuffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_sortListCounter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Upload buffer для сброса счётчиков
    {
        D3D12_HEAP_PROPERTIES hpUpload = {};
        hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(uint32_t);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_counterResetUpload)));
    }

    // Readback buffers для чтения счётчиков
    for (int i = 0; i < 2; ++i)
    {
        D3D12_HEAP_PROPERTIES hpReadback = {};
        hpReadback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(uint32_t);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&hpReadback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_counterReadback[i])));
        m_counterReadback[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_counterReadbackMapped[i]));
    }

    // Constant buffers
    auto createUploadCB = [&](UINT size, ComPtr<ID3D12Resource>& out) {
        size = (size + 255) & ~255;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_d3dDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out)));
        };

    createUploadCB(sizeof(ParticleEmitConstants), m_particleEmitCB);
    createUploadCB(sizeof(ParticleUpdateConstants), m_particleUpdateCB);
    createUploadCB(sizeof(ParticleRenderConstants), m_particleRenderCB);
    createUploadCB(sizeof(ParticleSortConstants), m_particleSortCB);

    // Create views
    auto cpuHandle = m_particleHeap->GetCPUDescriptorHandleForHeapStart();

    // u0 - Particle pool UAV
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer.NumElements = MAX_PARTICLES;
        uav.Buffer.StructureByteStride = sizeof(GpuParticle);
        m_d3dDevice->CreateUnorderedAccessView(m_particlePool.Get(), nullptr, &uav, cpuHandle);
        cpuHandle.ptr += m_particleDescSize;
    }

    // u1 - Dead list UAV (with counter)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer.NumElements = MAX_PARTICLES;
        uav.Buffer.StructureByteStride = sizeof(uint32_t);
        uav.Buffer.CounterOffsetInBytes = 0;
        m_d3dDevice->CreateUnorderedAccessView(m_deadList.Get(), m_deadListCounter.Get(), &uav, cpuHandle);
        cpuHandle.ptr += m_particleDescSize;
    }

    // u2 - Sort list UAV (with counter)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer.NumElements = MAX_PARTICLES;
        uav.Buffer.StructureByteStride = sizeof(ParticleSortEntry);
        uav.Buffer.CounterOffsetInBytes = 0;
        m_d3dDevice->CreateUnorderedAccessView(m_sortList.Get(), m_sortListCounter.Get(), &uav, cpuHandle);
        cpuHandle.ptr += m_particleDescSize;
    }

    // t0 - Particle pool SRV
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.NumElements = MAX_PARTICLES;
        srv.Buffer.StructureByteStride = sizeof(GpuParticle);
        m_d3dDevice->CreateShaderResourceView(m_particlePool.Get(), &srv, cpuHandle);
        cpuHandle.ptr += m_particleDescSize;
    }

    // t1 - Sort list SRV
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.NumElements = MAX_PARTICLES;
        srv.Buffer.StructureByteStride = sizeof(ParticleSortEntry);
        m_d3dDevice->CreateShaderResourceView(m_sortList.Get(), &srv, cpuHandle);
    }
}

void DirectXApp::ResetParticleCounter(ID3D12Resource* counterResource, uint32_t value)
{
    void* mapped = nullptr;
    m_counterResetUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, &value, sizeof(uint32_t));
    m_counterResetUpload->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = counterResource;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toCopy);

    m_cmdList->CopyBufferRegion(counterResource, 0, m_counterResetUpload.Get(), 0, sizeof(uint32_t));

    D3D12_RESOURCE_BARRIER toUav = {};
    toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav.Transition.pResource = counterResource;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toUav);
}

void DirectXApp::CopyParticleCounterToReadback(ID3D12Resource* counterResource, ID3D12Resource* readbackResource)
{
    D3D12_RESOURCE_BARRIER toSrc = {};
    toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrc.Transition.pResource = counterResource;
    toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toSrc);

    m_cmdList->CopyBufferRegion(readbackResource, 0, counterResource, 0, sizeof(uint32_t));

    D3D12_RESOURCE_BARRIER toUav = {};
    toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav.Transition.pResource = counterResource;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toUav);
}

void DirectXApp::TransitionParticleComputeResources(D3D12_RESOURCE_STATES state)
{
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    if (m_particlePoolState != state)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_particlePool.Get();
        b.Transition.StateBefore = m_particlePoolState;
        b.Transition.StateAfter = state;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(b);
        m_particlePoolState = state;
    }
    if (m_deadListState != state)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_deadList.Get();
        b.Transition.StateBefore = m_deadListState;
        b.Transition.StateAfter = state;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(b);
        m_deadListState = state;
    }
    if (m_sortListState != state)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_sortList.Get();
        b.Transition.StateBefore = m_sortListState;
        b.Transition.StateAfter = state;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(b);
        m_sortListState = state;
    }
    if (!barriers.empty())
        m_cmdList->ResourceBarrier((UINT)barriers.size(), barriers.data());
}

void DirectXApp::InitializeParticleSystem()
{
    CreateParticleRootSignatures();
    CreateParticlePipelines();
    CreateParticleResources();
    //  CreateParticleQuadGeometry();

    m_particlesInitialized = true;
    m_particlesNeedReinit = true;
    m_particleFrameCounter = 0;
    m_aliveParticleCount = 0;
}


void DirectXApp::ReinitializeParticles()
{
    if (!m_particlesInitialized)
        return;

    TransitionParticleComputeResources(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ResetParticleCounter(m_deadListCounter.Get(), 0);
    ResetParticleCounter(m_sortListCounter.Get(), 0);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetComputeRootSignature(m_particleComputeRS.Get());
    m_cmdList->SetComputeRootDescriptorTable(
        1,
        m_particleHeap->GetGPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr;

    // Clear sort list before the first render. Resetting the append counter does not
    // clear old entries inside m_sortList.
    {
        ParticleSortConstants clear = {};
        clear.ElementCount = MAX_PARTICLES;

        void* mapped = nullptr;
        m_particleSortCB->Map(0, nullptr, &mapped);
        memcpy(mapped, &clear, sizeof(clear));
        m_particleSortCB->Unmap(0, nullptr);

        m_cmdList->SetPipelineState(m_particleClearSortPSO.Get());
        m_cmdList->SetComputeRootConstantBufferView(
            0,
            m_particleSortCB->GetGPUVirtualAddress());

        m_cmdList->Dispatch(
            (MAX_PARTICLES + PARTICLE_THREADS_PER_GROUP - 1) / PARTICLE_THREADS_PER_GROUP,
            1,
            1);
    }

    m_cmdList->ResourceBarrier(1, &uavBarrier);

    ParticleUpdateConstants initCb = {};
    initCb.MaxParticles = MAX_PARTICLES;

    void* mapped = nullptr;
    m_particleUpdateCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &initCb, sizeof(initCb));
    m_particleUpdateCB->Unmap(0, nullptr);

    m_cmdList->SetComputeRootConstantBufferView(
        0,
        m_particleUpdateCB->GetGPUVirtualAddress());

    m_cmdList->SetPipelineState(m_particleInitDeadPSO.Get());
    m_cmdList->Dispatch(
        (MAX_PARTICLES + PARTICLE_THREADS_PER_GROUP - 1) / PARTICLE_THREADS_PER_GROUP,
        1,
        1);

    m_cmdList->ResourceBarrier(1, &uavBarrier);

    // Сразу копируем стартовый dead counter в readback slot 0.
    CopyParticleCounterToReadback(
        m_deadListCounter.Get(),
        m_counterReadback[0].Get());

    // Этот readback станет валиден, когда завершится текущий кадр.
    m_counterReadbackFenceValues[0] = m_fenceValues[m_currentBackBuffer];
    m_counterReadbackFenceValues[1] = 0;

    m_lastKnownDeadCount = MAX_PARTICLES;
    m_aliveParticleCount = 0;
}

void DirectXApp::UpdateParticles(float deltaTime)
{
    if (!m_particlesInitialized || !m_particlesEnabled)
        return;

    if (m_particlesNeedReinit)
    {
        ReinitializeParticles();
        m_particlesNeedReinit = false;
        m_particleFrameCounter = 0;
        m_particleTotalTime = 0.0f;
        m_aliveParticleCount = 0;
        return;
    }

    m_particleTotalTime += deltaTime;

    // Читаем "старый" readback slot, а в другой slot пишем новый.
    const uint32_t readIndex = m_particleFrameCounter % 2;
    const uint32_t writeIndex = (m_particleFrameCounter + 1) % 2;

    uint32_t deadCount = m_lastKnownDeadCount;

    // Читаем только если GPU уже закончил писать в этот readback slot.
    if (m_counterReadbackFenceValues[readIndex] != 0 &&
        m_fence->GetCompletedValue() >= m_counterReadbackFenceValues[readIndex] &&
        m_counterReadbackMapped[readIndex])
    {
        deadCount = *m_counterReadbackMapped[readIndex];
        m_lastKnownDeadCount = deadCount;
    }

    const uint32_t requestedEmitCount = 4;
    const uint32_t actualEmitCount =
        (requestedEmitCount < deadCount) ? requestedEmitCount : deadCount;

    TransitionParticleComputeResources(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ResetParticleCounter(m_sortListCounter.Get(), 0);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetComputeRootSignature(m_particleComputeRS.Get());
    m_cmdList->SetComputeRootDescriptorTable(
        1,
        m_particleHeap->GetGPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr;

    // Clear sort list, because resetting the append counter does not clear old entries.
    {
        ParticleSortConstants clear = {};
        clear.ElementCount = MAX_PARTICLES;

        void* mapped = nullptr;
        m_particleSortCB->Map(0, nullptr, &mapped);
        memcpy(mapped, &clear, sizeof(clear));
        m_particleSortCB->Unmap(0, nullptr);

        m_cmdList->SetPipelineState(m_particleClearSortPSO.Get());
        m_cmdList->SetComputeRootConstantBufferView(
            0,
            m_particleSortCB->GetGPUVirtualAddress());

        m_cmdList->Dispatch(
            (MAX_PARTICLES + PARTICLE_THREADS_PER_GROUP - 1) / PARTICLE_THREADS_PER_GROUP,
            1,
            1);
    }

    m_cmdList->ResourceBarrier(1, &uavBarrier);

    // Emit
    {
        ParticleEmitConstants emit = {};
        emit.EmitterPosition = DirectX::XMFLOAT3(0.0f, 8.0f, 0.0f);
        emit.EmitCount = actualEmitCount;
        emit.BaseVelocity = DirectX::XMFLOAT3(0.0f, 20.0f, 0.0f);
        emit.Time = m_particleTotalTime;
        emit.VelocityRandomness = DirectX::XMFLOAT3(10.0f, 8.0f, 10.0f);
        emit.MinLifeSpan = 2.0f;
        emit.MaxLifeSpan = 5.0f;
        emit.MinSize = 1.0f;
        emit.MaxSize = 3.0f;
        emit.RandomSeed = 1337u + m_particleFrameCounter;

        emit.StartColorA = DirectX::XMFLOAT4(1.0f, 0.1f, 0.1f, 1.0f);
        emit.StartColorB = DirectX::XMFLOAT4(0.1f, 0.3f, 1.0f, 1.0f);

        emit.EmitterRadius = 2.0f;

        void* mapped = nullptr;
        m_particleEmitCB->Map(0, nullptr, &mapped);
        memcpy(mapped, &emit, sizeof(emit));
        m_particleEmitCB->Unmap(0, nullptr);

        if (emit.EmitCount > 0)
        {
            m_cmdList->SetPipelineState(m_particleEmitPSO.Get());
            m_cmdList->SetComputeRootConstantBufferView(
                0,
                m_particleEmitCB->GetGPUVirtualAddress());

            m_cmdList->Dispatch(
                (emit.EmitCount + PARTICLE_THREADS_PER_GROUP - 1) / PARTICLE_THREADS_PER_GROUP,
                1,
                1);
        }
    }

    m_cmdList->ResourceBarrier(1, &uavBarrier);

    // Update
    {
        DirectX::XMFLOAT3 camPos = m_camera.GetPosition();

        ParticleUpdateConstants update = {};
        update.DeltaTime = deltaTime;
        update.TotalTime = m_particleTotalTime;
        update.MaxParticles = MAX_PARTICLES;
        update.Gravity = DirectX::XMFLOAT3(0.0f, -9.8f, 0.0f);
        update.GroundY = 0.0f;
        update.CameraPosition = camPos;
        update.EnableGroundCollision = 1;

        void* mapped = nullptr;
        m_particleUpdateCB->Map(0, nullptr, &mapped);
        memcpy(mapped, &update, sizeof(update));
        m_particleUpdateCB->Unmap(0, nullptr);

        m_cmdList->SetPipelineState(m_particleUpdatePSO.Get());
        m_cmdList->SetComputeRootConstantBufferView(
            0,
            m_particleUpdateCB->GetGPUVirtualAddress());

        m_cmdList->Dispatch(
            (MAX_PARTICLES + PARTICLE_THREADS_PER_GROUP - 1) / PARTICLE_THREADS_PER_GROUP,
            1,
            1);
    }

    m_cmdList->ResourceBarrier(1, &uavBarrier);

    // Sort alive entries by distance. We sort the full MAX_PARTICLES array because the
    // current render path draws MAX_PARTICLES and inactive entries were cleared above.
    {
        const uint32_t elementCount = MAX_PARTICLES;
        const uint32_t sortDescending = 1; // farther particles first for alpha blending

        m_cmdList->SetPipelineState(m_particleSortPSO.Get());

        for (uint32_t subArray = 2; subArray <= elementCount; subArray <<= 1)
        {
            for (uint32_t compareDistance = subArray >> 1;
                compareDistance > 0;
                compareDistance >>= 1)
            {
                ParticleSortConstants sort = {};
                sort.ElementCount = elementCount;
                sort.SubArray = subArray;
                sort.CompareDistance = compareDistance;
                sort.SortDescending = sortDescending;

                void* mapped = nullptr;
                m_particleSortCB->Map(0, nullptr, &mapped);
                memcpy(mapped, &sort, sizeof(sort));
                m_particleSortCB->Unmap(0, nullptr);

                m_cmdList->SetComputeRootConstantBufferView(
                    0,
                    m_particleSortCB->GetGPUVirtualAddress());

                m_cmdList->Dispatch(
                    (elementCount + PARTICLE_THREADS_PER_GROUP - 1) / PARTICLE_THREADS_PER_GROUP,
                    1,
                    1);

                m_cmdList->ResourceBarrier(1, &uavBarrier);
            }
        }
    }

    // Копируем новый dead counter в другой readback slot.
    CopyParticleCounterToReadback(
        m_deadListCounter.Get(),
        m_counterReadback[writeIndex].Get());

    // Этот slot станет валиден, когда текущий кадр завершится и будет засигнален fence.
    m_counterReadbackFenceValues[writeIndex] = m_fenceValues[m_currentBackBuffer];

    ++m_particleFrameCounter;

    // Это значение может слегка запаздывать, но для отладки/статистики норм.
    m_aliveParticleCount = MAX_PARTICLES - deadCount;
}

void DirectXApp::RenderParticles()
{
    if (!m_particlesInitialized || !m_particlesEnabled)
        return;

    std::vector<D3D12_RESOURCE_BARRIER> barriers;

    if (m_particlePoolState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_particlePool.Get();
        b.Transition.StateBefore = m_particlePoolState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(b);
        m_particlePoolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (m_sortListState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_sortList.Get();
        b.Transition.StateBefore = m_sortListState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(b);
        m_sortListState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (!barriers.empty())
        m_cmdList->ResourceBarrier((UINT)barriers.size(), barriers.data());

    float aspect = (m_screenHeight > 0)
        ? (float)m_screenWidth / m_screenHeight
        : 1.0f;

    DirectX::XMMATRIX view = m_camera.GetViewMatrix();
    DirectX::XMMATRIX proj = m_camera.GetProjectionMatrix(aspect);
    DirectX::XMMATRIX viewProj = view * proj;
    DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);

    DirectX::XMFLOAT4X4 vp;
    DirectX::XMStoreFloat4x4(&vp, DirectX::XMMatrixTranspose(viewProj));

    DirectX::XMFLOAT3 right, up;
    DirectX::XMStoreFloat3(&right, DirectX::XMVector3Normalize(invView.r[0]));
    DirectX::XMStoreFloat3(&up, DirectX::XMVector3Normalize(invView.r[1]));

    ParticleRenderConstants rc = {};
    rc.ViewProj = vp;
    rc.CameraRight = right;
    rc.CameraUp = up;
    rc.DirectionalLightDir = DirectX::XMFLOAT3(0.3f, -1.0f, 0.25f);
    rc.DirectionalLightIntensity = 2.2f;
    rc.DirectionalLightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    rc.AmbientColor = DirectX::XMFLOAT4(0.28f, 0.28f, 0.3f, 1.0f);

    void* mapped = nullptr;
    m_particleRenderCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &rc, sizeof(rc));
    m_particleRenderCB->Unmap(0, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += m_currentBackBuffer * m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_cmdList->SetGraphicsRootSignature(m_particleRenderRS.Get());
    m_cmdList->SetPipelineState(m_particleRenderPSO.Get());
    m_cmdList->SetGraphicsRootConstantBufferView(0, m_particleRenderCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvTable = m_particleHeap->GetGPUDescriptorHandleForHeapStart();
    srvTable.ptr += 3 * m_particleDescSize;
    m_cmdList->SetGraphicsRootDescriptorTable(1, srvTable);

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    m_cmdList->DrawInstanced(MAX_PARTICLES, 1, 0, 0);
}

void DirectXApp::CreateShadowMap()
{
    m_shadowViewport.TopLeftX = 0.0f;
    m_shadowViewport.TopLeftY = 0.0f;
    m_shadowViewport.Width = static_cast<float>(SHADOW_MAP_SIZE);
    m_shadowViewport.Height = static_cast<float>(SHADOW_MAP_SIZE);
    m_shadowViewport.MinDepth = 0.0f;
    m_shadowViewport.MaxDepth = 1.0f;

    m_shadowScissor = { 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = SHADOW_MAP_SIZE;
    texDesc.Height = SHADOW_MAP_SIZE;
    texDesc.DepthOrArraySize = SHADOW_CASCADE_COUNT;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc = { 1, 0 };
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&m_shadowMap)));

    m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = SHADOW_CASCADE_COUNT;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

    ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
        &dsvHeapDesc,
        IID_PPV_ARGS(&m_shadowDsvHeap)));

    UINT dsvSize = m_d3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = cascade;
        dsvDesc.Texture2DArray.ArraySize = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsvHandle.ptr += cascade * dsvSize;

        m_d3dDevice->CreateDepthStencilView(
            m_shadowMap.Get(),
            &dsvDesc,
            dsvHandle);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;

    m_gbuffer->CreateExternalSRV(
        SHADOW_SRV_SLOT,
        m_shadowMap.Get(),
        srvDesc);
}

void DirectXApp::CreateShadowPipeline()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"shaders.hlsl";

    auto vs = CompileShaderFile(filePath, "ShadowVSMain", "vs_5_0");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.DepthClipEnable = TRUE;

    rasterizerDesc.DepthBias = 10000;
    rasterizerDesc.SlopeScaledDepthBias = 2.0f;
    rasterizerDesc.DepthBiasClamp = 0.01f;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { nullptr, 0 };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(
        &psoDesc,
        IID_PPV_ARGS(&m_shadowPSO)));
}

void DirectXApp::UpdateShadowConstants()
{
    float aspectRatio = (m_screenHeight > 0)
        ? static_cast<float>(m_screenWidth) / m_screenHeight
        : 1.0f;

    const float cascadeNear[SHADOW_CASCADE_COUNT] = { 0.1f, 10.0f, 30.0f, 80.0f };
    const float cascadeFar[SHADOW_CASCADE_COUNT] = { 10.0f, 30.0f, 80.0f, 300.0f };

    m_cascadeSplits = DirectX::XMFLOAT4(
        cascadeFar[0],
        cascadeFar[1],
        cascadeFar[2],
        cascadeFar[3]);

    DirectX::XMVECTOR lightDir = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(-0.5f, -1.0f, -0.3f, 0.0f));

    DirectX::XMMATRIX cameraView = m_camera.GetViewMatrix();

    for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade)
    {
        DirectX::XMMATRIX cascadeProj = DirectX::XMMatrixPerspectiveFovLH(
            DirectX::XMConvertToRadians(60.0f),
            aspectRatio,
            cascadeNear[cascade],
            cascadeFar[cascade]);

        DirectX::XMMATRIX invViewProj =
            DirectX::XMMatrixInverse(nullptr, cameraView * cascadeProj);

        DirectX::XMVECTOR frustumCorners[8] =
        {
            DirectX::XMVectorSet(-1.0f,  1.0f, 0.0f, 1.0f),
            DirectX::XMVectorSet(1.0f,  1.0f, 0.0f, 1.0f),
            DirectX::XMVectorSet(1.0f, -1.0f, 0.0f, 1.0f),
            DirectX::XMVectorSet(-1.0f, -1.0f, 0.0f, 1.0f),
            DirectX::XMVectorSet(-1.0f,  1.0f, 1.0f, 1.0f),
            DirectX::XMVectorSet(1.0f,  1.0f, 1.0f, 1.0f),
            DirectX::XMVectorSet(1.0f, -1.0f, 1.0f, 1.0f),
            DirectX::XMVectorSet(-1.0f, -1.0f, 1.0f, 1.0f),
        };

        DirectX::XMVECTOR center = DirectX::XMVectorZero();

        for (int i = 0; i < 8; ++i)
        {
            frustumCorners[i] = DirectX::XMVector3TransformCoord(
                frustumCorners[i],
                invViewProj);

            center += frustumCorners[i];
        }

        center /= 8.0f;

        DirectX::XMVECTOR lightPos = center - lightDir * 250.0f;

        DirectX::XMMATRIX lightView = DirectX::XMMatrixLookAtLH(
            lightPos,
            center,
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

        DirectX::XMVECTOR minBounds =
            DirectX::XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 1.0f);
        DirectX::XMVECTOR maxBounds =
            DirectX::XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.0f);

        for (int i = 0; i < 8; ++i)
        {
            DirectX::XMVECTOR cornerLS =
                DirectX::XMVector3TransformCoord(frustumCorners[i], lightView);

            minBounds = DirectX::XMVectorMin(minBounds, cornerLS);
            maxBounds = DirectX::XMVectorMax(maxBounds, cornerLS);
        }

        DirectX::XMFLOAT3 minLS;
        DirectX::XMFLOAT3 maxLS;
        DirectX::XMStoreFloat3(&minLS, minBounds);
        DirectX::XMStoreFloat3(&maxLS, maxBounds);

        float zMult = 10.0f;
        if (minLS.z < 0.0f)
            minLS.z *= zMult;
        else
            minLS.z /= zMult;

        if (maxLS.z < 0.0f)
            maxLS.z /= zMult;
        else
            maxLS.z *= zMult;

        DirectX::XMMATRIX lightProj = DirectX::XMMatrixOrthographicOffCenterLH(
            minLS.x,
            maxLS.x,
            minLS.y,
            maxLS.y,
            minLS.z,
            maxLS.z);

        DirectX::XMMATRIX lightViewProj = lightView * lightProj;

        DirectX::XMStoreFloat4x4(
            &m_lightView[cascade],
            DirectX::XMMatrixTranspose(lightView));

        DirectX::XMStoreFloat4x4(
            &m_lightProj[cascade],
            DirectX::XMMatrixTranspose(lightProj));

        DirectX::XMStoreFloat4x4(
            &m_lightViewProj[cascade],
            DirectX::XMMatrixTranspose(lightViewProj));
    }
}

void DirectXApp::RenderShadowPass()
{
    UpdateShadowConstants();

    if (!m_shadowConstantBuffer || !m_mappedShadowConstantData)
        return;

    if (m_shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_shadowMap.Get();
        barrier.Transition.StateBefore = m_shadowMapState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(1, &barrier);
        m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    m_cmdList->RSSetViewports(1, &m_shadowViewport);
    m_cmdList->RSSetScissorRects(1, &m_shadowScissor);

    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_cmdList->SetPipelineState(m_shadowPSO.Get());

    UINT dsvSize = m_d3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    const UINT64 cbStride = (sizeof(ConstantBufferData) + 255) & ~255ull;

    for (UINT cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv =
            m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        shadowDsv.ptr += cascade * dsvSize;

        m_cmdList->ClearDepthStencilView(
            shadowDsv,
            D3D12_CLEAR_FLAG_DEPTH,
            1.0f,
            0,
            0,
            nullptr);

        m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);

        ConstantBufferData shadowData = {};
        shadowData.World = DirectX::XMMatrixTranspose(
            DirectX::XMMatrixScaling(0.1f, 0.1f, 0.1f));

        shadowData.View = DirectX::XMLoadFloat4x4(&m_lightView[cascade]);
        shadowData.Proj = DirectX::XMLoadFloat4x4(&m_lightProj[cascade]);
        shadowData.LightPos = DirectX::XMFLOAT4(-0.5f, -1.0f, -0.3f, 0.0f);
        shadowData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        shadowData.CameraPos = DirectX::XMFLOAT4(0, 0, 0, 0);
        shadowData.Tiling = DirectX::XMFLOAT2(1, 1);
        shadowData.UVOffset = DirectX::XMFLOAT2(0, 0);

        memcpy(
            m_mappedShadowConstantData + cascade * cbStride,
            &shadowData,
            sizeof(shadowData));

        m_cmdList->SetGraphicsRootConstantBufferView(
            0,
            m_shadowConstantBuffer->GetGPUVirtualAddress() + cascade * cbStride);

        if (renderSponza)
        {
            m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_cmdList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
            m_cmdList->IASetIndexBuffer(&m_indexBufferView);
            m_cmdList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
        }
    }

    if (m_shadowMapState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_shadowMap.Get();
        barrier.Transition.StateBefore = m_shadowMapState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(1, &barrier);
        m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void DirectXApp::CreateShadowConstantBuffer()
{
    const UINT64 cbStride = (sizeof(ConstantBufferData) + 255) & ~255ull;
    const UINT64 bufferSize = cbStride * SHADOW_CASCADE_COUNT;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = { 1, 0 };
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_shadowConstantBuffer)));

    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(m_shadowConstantBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&m_mappedShadowConstantData)));
}