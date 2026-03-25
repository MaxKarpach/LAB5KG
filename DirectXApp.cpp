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
    // Изменяем количество дескрипторов с 1 на 3 (main, normal, displacement)
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

    D3D12_ROOT_PARAMETER rootParams[3] = {};

    // Слот 0: Константный буфер объекта (b0)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Слот 1: Дескрипторная таблица для текстур (3 текстуры)
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 3;  // Было 1, стало 3
    rootParams[1].DescriptorTable.pDescriptorRanges = srvRanges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Слот 2: Константный буфер тесселяции (b2)
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 2;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
    desc.NumParameters = 3;  // Было 2, стало 3
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
    else if (textureSlot == 2)
    {
        targetTexture = &m_normalTexture;
        uploadBuffer = &m_normalTextureUpload;
    }
    else // slot 3
    {
        targetTexture = &m_displacementTexture;
        uploadBuffer = &m_displacementTextureUpload;
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
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        IID_PPV_ARGS(targetTexture->GetAddressOf())));  // Изменено!

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
        IID_PPV_ARGS(uploadBuffer->GetAddressOf())));  // Изменено!

    // Копируем данные в буфер
    uint8_t* mappedMemory = nullptr;
    (*uploadBuffer)->Map(0, nullptr, reinterpret_cast<void**>(&mappedMemory));
    UINT sourceRowPitch = texData.width * 4;
    for (UINT row = 0; row < texData.height; ++row)
    {
        memcpy(mappedMemory + (UINT64)footprint.Footprint.RowPitch * row,
            texData.pixels.data() + (UINT64)sourceRowPitch * row,
            sourceRowPitch);
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
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    // Кулдаун
    if (m_shootCooldown > 0.0f)
    {
        m_shootCooldown -= deltaTime;
    }

    // Стрельба по пробелу
    if (m_inputDevice && m_inputDevice->IsKeyDown(VK_SPACE) && m_shootCooldown <= 0.0f)
    {
        Shoot();
        m_shootCooldown = SHOOT_COOLDOWN_TIME;
    }


    // Модель не вращается
    float scale = 0.1f;
    DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(scale, scale, scale);
    DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationY(0.0f);
    DirectX::XMMATRIX worldMatrix = scaleMatrix * rotationMatrix;

    // Получаем матрицы камеры
    float aspectRatio = (m_screenHeight > 0) ?
        static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;

    DirectX::XMMATRIX viewMatrix = m_camera.GetViewMatrix();
    DirectX::XMMATRIX projMatrix = m_camera.GetProjectionMatrix(aspectRatio);

    // Позиция камеры для освещения
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    ConstantBufferData constantData = {};
    constantData.World = DirectX::XMMatrixTranspose(worldMatrix);
    constantData.View = DirectX::XMMatrixTranspose(viewMatrix);
    constantData.Proj = DirectX::XMMatrixTranspose(projMatrix);
    constantData.LightPos = DirectX::XMFLOAT4(2.0f, 5.0f, -2.0f, 0.0f);
    constantData.LightColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constantData.CameraPos = DirectX::XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 1.0f);
    constantData.Tiling = DirectX::XMFLOAT2(1.0f, 1.0f);
    constantData.UVOffset = DirectX::XMFLOAT2(0.0f, 0.0f);

    memcpy(m_mappedConstantData, &constantData, sizeof(constantData));

    UpdateLightingConstants();
}

void DirectXApp::RenderGeometryPass()
{
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // Обновляем константы тесселяции
    TessellationConstants tessConsts;
    tessConsts.TessellationFactor = GetAdaptiveTessellationFactor();
    tessConsts.DisplacementStrength = 0.8f;
    tessConsts.TessMinDist = 0.0f;
    tessConsts.TessMaxDist = 30.0f;
    memcpy(m_mappedTessellationData, &tessConsts, sizeof(tessConsts));

    m_renderingSystem->RenderGeometryPass(
        m_cmdList.Get(),
        m_rootSignature.Get(),
        m_tessGeometryPSO.Get(),
        m_vertexBufferView,
        m_indexBufferView,
        m_indexCount,
        m_srvHeap.Get(),
        m_constantBuffer.Get(),
        m_tessellationCB.Get(),  // ПЕРЕДАЕМ КОНСТАНТНЫЙ БУФЕР ТЕССЕЛЯЦИИ
        m_gbuffer.get(),
        dsvHandle);
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

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_currentBackBuffer].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);

    // Обновляем константы тесселяции
    TessellationConstants tessConsts;
    tessConsts.TessellationFactor = GetAdaptiveTessellationFactor();
    tessConsts.DisplacementStrength = 0.8f;
    tessConsts.TessMinDist = 0.0f;
    tessConsts.TessMaxDist = 30.0f;
    memcpy(m_mappedTessellationData, &tessConsts, sizeof(tessConsts));

    m_renderingSystem->RenderForward(
        m_cmdList.Get(),
        m_rootSignature.Get(),
        m_tessPipeline.Get(),
        m_vertexBufferView,
        m_indexBufferView,
        m_indexCount,
        m_srvHeap.Get(),
        m_constantBuffer.Get(),
        m_tessellationCB.Get(),  // ПЕРЕДАЕМ КОНСТАНТНЫЙ БУФЕР ТЕССЕЛЯЦИИ
        rtvHandle,
        dsvHandle,
        m_screenWidth, m_screenHeight,
        m_currentBackBuffer,
        m_rtvDescriptorSize);

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

            // Загрузка модели
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

            // ========== ЗАГРУЗКА ТЕКСТУР ==========

            // Текстура 0: Основная текстура (diffuse/albedo)
            TextureData texData0 = LoadTextureWIC(workingDir + L"texture_first.png");
            if (!texData0.valid)
                texData0 = LoadTextureWIC(workingDir + L"texture_first.jpg");
            if (texData0.valid)
            {
                UploadTexture(texData0, 0, false);
                MessageBoxA(m_windowHandle, "Main texture loaded!", "Info", MB_OK);
            }
            else
            {
                // Создаем текстуру-заглушку (серая)
                TextureData dummy;
                dummy.width = 2;
                dummy.height = 2;
                dummy.valid = true;
                dummy.pixels.resize(2 * 2 * 4);
                for (size_t i = 0; i < dummy.pixels.size(); i += 4)
                {
                    dummy.pixels[i + 0] = 128;
                    dummy.pixels[i + 1] = 128;
                    dummy.pixels[i + 2] = 128;
                    dummy.pixels[i + 3] = 255;
                }
                UploadTexture(dummy, 0, false);
                MessageBoxA(m_windowHandle, "Using dummy main texture", "Warning", MB_OK);
            }

            // Текстура 1: Карта нормалей (normal map)
            TextureData normalData = LoadTextureWIC(workingDir + L"normal.png");
            if (!normalData.valid)
                normalData = LoadTextureWIC(workingDir + L"normal.jpg");
            if (normalData.valid)
            {
                UploadTexture(normalData, 1, true); // Слот 1 для карты нормалей
                m_useNormalMap = true;
                MessageBoxA(m_windowHandle, "Normal map loaded!", "Info", MB_OK);
            }
            else
            {
                // Создаем фиктивную карту нормалей (RGB 128,128,255 -> нормаль 0,0,1)
                TextureData dummyNormal;
                dummyNormal.width = 2;
                dummyNormal.height = 2;
                dummyNormal.valid = true;
                dummyNormal.pixels.resize(2 * 2 * 4);
                for (size_t i = 0; i < dummyNormal.pixels.size(); i += 4)
                {
                    dummyNormal.pixels[i + 0] = 128; // R = 0.5
                    dummyNormal.pixels[i + 1] = 128; // G = 0.5
                    dummyNormal.pixels[i + 2] = 255; // B = 1.0
                    dummyNormal.pixels[i + 3] = 255;
                }
                UploadTexture(dummyNormal, 1, true);
                m_useNormalMap = false;
                MessageBoxA(m_windowHandle, "Using dummy normal map", "Warning", MB_OK);
            }

            // Текстура 2: Карта смещения (displacement map)
            TextureData displacementData = LoadTextureWIC(workingDir + L"displacement.png");
            if (!displacementData.valid)
                displacementData = LoadTextureWIC(workingDir + L"displacement.jpg");
            if (displacementData.valid)
            {
                UploadTexture(displacementData, 2, false); // Слот 2 для карты смещения
                m_useDisplacement = true;
                MessageBoxA(m_windowHandle, "Displacement map loaded!", "Info", MB_OK);
            }
            else
            {
                // Создаем фиктивную карту смещения (серый 128 -> нет смещения)
                TextureData dummyDisplacement;
                dummyDisplacement.width = 2;
                dummyDisplacement.height = 2;
                dummyDisplacement.valid = true;
                dummyDisplacement.pixels.resize(2 * 2 * 4);
                for (size_t i = 0; i < dummyDisplacement.pixels.size(); i += 4)
                {
                    dummyDisplacement.pixels[i + 0] = 128; // R = 0.5 (нет смещения)
                    dummyDisplacement.pixels[i + 1] = 128;
                    dummyDisplacement.pixels[i + 2] = 128;
                    dummyDisplacement.pixels[i + 3] = 255;
                }
                UploadTexture(dummyDisplacement, 2, false);
                m_useDisplacement = false;
                MessageBoxA(m_windowHandle, "Using dummy displacement map", "Warning", MB_OK);
            }

            // Текстура 3: Вторая текстура (опционально, для слота 3)
            TextureData texData3 = LoadTextureWIC(workingDir + L"texture_second.png");
            if (!texData3.valid)
                texData3 = LoadTextureWIC(workingDir + L"texture_second.jpg");
            if (texData3.valid)
            {
                UploadTexture(texData3, 3, false);
            }
        }

        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, commandLists);
        FlushCommandQueue();

        CreateConstantBuffer();
        UpdateLightingConstants();

        // Выводим итоговую информацию
        std::string info = "Initialization complete!\n";
        info += "Normal map: " + std::string(m_useNormalMap ? "YES" : "NO (using dummy)") + "\n";
        info += "Displacement: " + std::string(m_useDisplacement ? "YES" : "NO (using dummy)") + "\n";
        info += "Tessellation factor: " + std::to_string(GetAdaptiveTessellationFactor()) + "\n";
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
    // Центр модели
    DirectX::XMFLOAT3 modelCenter = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    // Позиция камеры
    DirectX::XMFLOAT3 cameraPos = m_camera.GetPosition();

    // Расстояние
    float dx = cameraPos.x - modelCenter.x;
    float dy = cameraPos.y - modelCenter.y;
    float dz = cameraPos.z - modelCenter.z;
    float distance = sqrt(dx * dx + dy * dy + dz * dz);

    // Параметры
    float maxTessFactor = 16.0f;   // близко
    float minTessFactor = 1.0f;    // далеко
    float maxDistance = 50.0f;
    float minDistance = 5.0f;

    // Интерполяция
    float t = (distance - minDistance) / (maxDistance - minDistance);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float factor = maxTessFactor * (1.0f - t) + minTessFactor * t;
    return factor;
}