#include "DirectXApp.h"
#include "ThrowIfFailed.h"
#include <cassert>
#include <memory>

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
        m_rtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // DSV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_dsvHeap)));
    }

    // SRV/CBV/UAV heap - увеличиваем до 8 для всех текстур
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 8;  // Увеличено для 4 текстур + запас
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)));
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
ComPtr<ID3DBlob> DirectXApp::CompileShaderFile(const std::wstring& filePath,
    const std::string& entryPoint, const std::string& shaderModel)
{
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::string errorMsg = "Shader file missing:\n";
        int charCount = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1,
            nullptr, 0, nullptr, nullptr);
        std::string narrowPath(charCount, 0);
        WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1,
            &narrowPath[0], charCount, nullptr, nullptr);
        errorMsg += narrowPath;
        MessageBoxA(m_windowHandle, errorMsg.c_str(), "Shader Error", MB_OK | MB_ICONERROR);
        ThrowIfFailed(E_FAIL, "Shader compilation failed - file not found");
    }

    ComPtr<ID3DBlob> shaderBlob, errorBlob;
    HRESULT hr = D3DCompileFromFile(
        filePath.c_str(), nullptr, nullptr,
        entryPoint.c_str(), shaderModel.c_str(), compileFlags, 0,
        &shaderBlob, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
            MessageBoxA(m_windowHandle,
                static_cast<char*>(errorBlob->GetBufferPointer()),
                "Shader Compile Error", MB_OK | MB_ICONERROR);
        ThrowIfFailed(hr, "Shader compilation failed");
    }
    return shaderBlob;
}

void DirectXApp::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRanges[3] = {};

    // SRV для основной текстуры (t0)
    srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[0].NumDescriptors = 1;
    srvRanges[0].BaseShaderRegister = 0;
    srvRanges[0].RegisterSpace = 0;
    srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // SRV для карты нормалей (t1)
    srvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[1].NumDescriptors = 1;
    srvRanges[1].BaseShaderRegister = 1;
    srvRanges[1].RegisterSpace = 0;
    srvRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // SRV для карты смещения (t2)
    srvRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[2].NumDescriptors = 1;
    srvRanges[2].BaseShaderRegister = 2;
    srvRanges[2].RegisterSpace = 0;
    srvRanges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[4] = {};

    // Слот 0: Константный буфер объекта (b0)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Слот 1: Дескрипторная таблица для текстур
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 3;
    rootParams[1].DescriptorTable.pDescriptorRanges = srvRanges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Слот 2: Константный буфер тесселяции (b2)
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 2;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Слот 3: Константный буфер воды (b3)
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 3;
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // <

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        if (error)
        {
            std::string errorMsg = (char*)error->GetBufferPointer();
            MessageBoxA(m_windowHandle, errorMsg.c_str(), "Root Signature Error", MB_OK);
        }
        ThrowIfFailed(hr);
    }

    hr = m_d3dDevice->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    ThrowIfFailed(hr);
}

void DirectXApp::CreateDeferredRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};

    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = rootParams;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr))
    {
        if (error)
        {
            std::string errorMsg = (char*)error->GetBufferPointer();
            MessageBoxA(m_windowHandle, errorMsg.c_str(), "Deferred Root Signature Error", MB_OK);
        }
        ThrowIfFailed(hr);
    }

    hr = m_d3dDevice->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_deferredLightingRootSignature));
    ThrowIfFailed(hr);
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

    // Обновляем время воды для анимации
    m_waterTime += deltaTime;
    if (m_waterTime > 1000.0f) m_waterTime -= 1000.0f;

    if (m_mappedWaterConstantData)
    {
        m_mappedWaterConstantData->Time = m_waterTime;
        m_mappedWaterConstantData->WaveStrength = 0.6f;
        m_mappedWaterConstantData->WaveSpeed = 2.5f;
        m_mappedWaterConstantData->WaveFrequency = 1.5f;
    }

    // Матрица модели
    float scale = 0.1f;
    DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(scale, scale, scale);
    DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationY(0.0f);
    DirectX::XMMATRIX worldMatrix = scaleMatrix * rotationMatrix;

    // В конце Update() добавьте:
// В Update() измените:
    float cubeScale = 7.0f;      // Увеличим размер
    float cubeY = 25.0f;         // Поднимем над водой (вода на Y=20)
    DirectX::XMMATRIX cubeScaleMatrix = DirectX::XMMatrixScaling(cubeScale, cubeScale, cubeScale);
    DirectX::XMMATRIX cubeTranslationMatrix = DirectX::XMMatrixTranslation(0.0f, cubeY, 0.0f);
    m_cubeWorldMatrix = cubeScaleMatrix * cubeTranslationMatrix;
    // Матрицы камеры
    float aspectRatio = (m_screenHeight > 0) ? static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);

    // Позиция камеры
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    // Константный буфер
    ConstantBufferData constantData = {};
    constantData.World = DirectX::XMMatrixTranspose(worldMatrix);
    constantData.View = DirectX::XMMatrixTranspose(viewMatrix);
    constantData.Proj = DirectX::XMMatrixTranspose(projMatrix);
    constantData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
    constantData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constantData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, m_waterTime);
    constantData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
    constantData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);

    memcpy(m_mappedConstantData, &constantData, sizeof(constantData));

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

    // ========== 1. РЕНДЕРИМ ВСЕ 1000 КУБОВ ==========
    m_cmdList->SetPipelineState(m_cubePipelineState.Get());

    float aspectRatio = (m_screenHeight > 0) ? static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    for (int i = 0; i < CUBE_COUNT; ++i)
    {
        ConstantBufferData cubeData = {};

        DirectX::XMMATRIX cubeWorld = DirectX::XMMatrixScaling(m_cubes[i].Scale, m_cubes[i].Scale, m_cubes[i].Scale) *
            DirectX::XMMatrixTranslation(m_cubes[i].Position.x, m_cubes[i].Position.y, m_cubes[i].Position.z);

        cubeData.World = DirectX::XMMatrixTranspose(cubeWorld);
        cubeData.View = DirectX::XMMatrixTranspose(viewMatrix);
        cubeData.Proj = DirectX::XMMatrixTranspose(projMatrix);
        cubeData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
        cubeData.LightColor = DirectX::XMFLOAT4(m_cubes[i].Color.x, m_cubes[i].Color.y, m_cubes[i].Color.z, 1.0f);
        cubeData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        cubeData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
        cubeData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);

        memcpy(m_mappedCubeConstantDataArray[i], &cubeData, sizeof(ConstantBufferData));

        m_cmdList->SetGraphicsRootConstantBufferView(0, m_cubeConstantBuffers[i]->GetGPUVirtualAddress());
        m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        m_cmdList->SetGraphicsRootConstantBufferView(2, m_tessellationCB->GetGPUVirtualAddress());

        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmdList->IASetVertexBuffers(0, 1, &m_cubeVertexBufferView);
        m_cmdList->IASetIndexBuffer(&m_cubeIndexBufferView);
        m_cmdList->DrawIndexedInstanced(m_cubeIndexCount, 1, 0, 0, 0);
    }

    // ========== 2. РЕНДЕРИМ ВОДУ ==========
    m_cmdList->SetPipelineState(m_waterTessPipeline.Get());

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

    if (m_waterConstantBuffer)
    {
        m_cmdList->SetGraphicsRootConstantBufferView(3, m_waterConstantBuffer->GetGPUVirtualAddress());
    }

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_waterVertexBufferView);
    m_cmdList->IASetIndexBuffer(&m_waterIndexBufferView);
    m_cmdList->DrawIndexedInstanced(m_waterIndexCount, 1, 0, 0, 0);

    // ========== 3. РЕНДЕРИМ МОДЕЛЬ ДВОРЦА ==========
    TessellationConstants tessConsts;
    tessConsts.TessellationFactor = GetAdaptiveTessellationFactor();
    tessConsts.DisplacementStrength = 0.02f;
    tessConsts.TessMinDist = 0.0f;
    tessConsts.TessMaxDist = 30.0f;
    memcpy(m_mappedTessellationData, &tessConsts, sizeof(tessConsts));

    ConstantBufferData palaceData = {};
    palaceData.World = DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(0.1f, 0.1f, 0.1f));
    palaceData.View = DirectX::XMMatrixTranspose(viewMatrix);
    palaceData.Proj = DirectX::XMMatrixTranspose(projMatrix);
    palaceData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
    palaceData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    palaceData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, m_waterTime);

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

    RenderGeometryPass();
    RenderLightingPass();

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
                MessageBoxA(m_windowHandle, "Model loaded successfully!", "Info", MB_OK);
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
                MessageBoxA(m_windowHandle, "Main texture loaded!", "Info", MB_OK);
            }

            TextureData normalData = LoadTextureWIC(workingDir + L"normal.png");
            if (!normalData.valid)
                normalData = LoadTextureWIC(workingDir + L"normal.jpg");
            if (normalData.valid)
            {
                UploadTexture(normalData, 1, true);
                m_useNormalMap = true;
                MessageBoxA(m_windowHandle, "Normal map loaded!", "Info", MB_OK);
            }

            TextureData displacementData = LoadTextureWIC(workingDir + L"displacement.png");
            if (!displacementData.valid)
                displacementData = LoadTextureWIC(workingDir + L"displacement.jpg");
            if (displacementData.valid)
            {
                UploadTexture(displacementData, 2, false);
                m_useDisplacement = true;
                MessageBoxA(m_windowHandle, "Displacement map loaded!", "Info", MB_OK);
            }

            TextureData texData3 = LoadTextureWIC(workingDir + L"texture_second.png");
            if (!texData3.valid)
                texData3 = LoadTextureWIC(workingDir + L"texture_second.jpg");
            if (texData3.valid)
            {
                UploadTexture(texData3, 3, false);
                MessageBoxA(m_windowHandle, "Second texture loaded!", "Info", MB_OK);
            }
        }

        CreateWaterPlane();
        CreateWaterPipelineState();
        CreateWaterConstantBuffer();
        CreateCubeGeometry();
        CreateCubePipelineState();

        // ========== СОЗДАЕМ 1000 КОНСТАНТНЫХ БУФЕРОВ ==========
        m_cubes.resize(CUBE_COUNT);
        m_cubeConstantBuffers.resize(CUBE_COUNT);
        m_mappedCubeConstantDataArray.resize(CUBE_COUNT);

        // Создаем константные буферы для всех 1000 кубов
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

        // ========== ГЕНЕРИРУЕМ 1000 КУБОВ В СЕТКЕ 33x33 ==========
        int gridSize = 33;  // 33x33 = 1089 кубов (больше 1000)
        float spacing = 2.5f;
        float startX = -(gridSize - 1) * spacing / 2.0f;
        float startZ = -(gridSize - 1) * spacing / 2.0f;

        int index = 0;
        for (int x = 0; x < gridSize && index < CUBE_COUNT; ++x)
        {
            for (int z = 0; z < gridSize && index < CUBE_COUNT; ++z)
            {
                float posX = startX + x * spacing;
                float posZ = startZ + z * spacing;
                float posY = 0.0f;

                // Случайный размер от 0.5 до 1.5
                float scale = 0.8f + (float)(rand() % 70) / 100.0f;

                // Радужные цвета на основе позиции
                float r = (float)x / gridSize;
                float g = (float)z / gridSize;
                float b = (float)((x + z) % gridSize) / gridSize;

                m_cubes[index] = { DirectX::XMFLOAT3(posX, posY, posZ), scale, DirectX::XMFLOAT3(r, g, b) };
                index++;
            }
        }

        // Если нужно заполнить оставшиеся (на случай если 33x33 не хватило)
        while (index < CUBE_COUNT)
        {
            float angle = (float)index * 3.14159f * 2.0f / 50.0f;
            float radius = 40.0f;
            m_cubes[index] = {
                DirectX::XMFLOAT3(cos(angle) * radius, sin(angle * 5.0f) * 3.0f, sin(angle) * radius),
                0.8f,
                DirectX::XMFLOAT3(0.5f + sin(angle) * 0.5f, 0.5f + cos(angle * 2.0f) * 0.5f, 0.5f + sin(angle * 3.0f) * 0.5f)
            };
            index++;
        }

        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, commandLists);
        FlushCommandQueue();

        CreateConstantBuffer();
        UpdateLightingConstants();

        std::string info = "Initialization complete!\n";
        info += "Cubes: " + std::to_string(CUBE_COUNT) + "\n";
        info += "Normal map: " + std::string(m_useNormalMap ? "YES" : "NO") + "\n";
        info += "Displacement: " + std::string(m_useDisplacement ? "YES" : "NO") + "\n";
        MessageBoxA(m_windowHandle, info.c_str(), "Initialization Info", MB_OK);
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

    // Используем CubeVSMain вместо GeometryVSMain
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