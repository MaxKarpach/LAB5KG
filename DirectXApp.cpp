#include "DirectXApp.h"
#include "Utils.h"
#include <cassert>

//  Constructor / Destructor
DirectXApp::DirectXApp(HWND windowHandle, int windowWidth, int windowHeight)
    : m_windowHandle(windowHandle)
    , m_screenWidth(windowWidth)
    , m_screenHeight(windowHeight)
{
    for (UINT i = 0; i < BACK_BUFFER_COUNT; ++i)
        m_fenceValues[i] = 0;
}

DirectXApp::~DirectXApp()
{
    if (m_cmdQueue && m_fence && m_fenceEvent)
    {
        try { WaitForGPU(); }
        catch (...) {}
    }

    if (m_constantBuffer && m_mappedConstantData)
        m_constantBuffer->Unmap(0, nullptr);

    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
}

//  Start - initialization entry point
bool DirectXApp::Start()
{
    try
    {
        // Initialize D3D12 components
        CreateD3DDevice();
        CreateCommandQueue();
        CreateSwapChain();
        CreateDescriptorHeaps();
        CreateRenderTargets();
        CreateDepthBuffer();
        CreateCommandList();
        CreateSyncObjects();
        CreateRootSignature();
        CreatePipelineState();

        // Open command list for uploading
        ThrowIfFailed(m_cmdAllocators[0]->Reset());
        ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

        // Load model and textures
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring workingDir(exePath);
            workingDir = workingDir.substr(0, workingDir.find_last_of(L"\\/") + 1);

            // Load the head model
// Load the head model
            ObjResult modelData = LoadObj(workingDir + L"model.obj");  // верни как было
            if (modelData.valid)
                BuildMeshBuffers(modelData.vertices, modelData.indices);
            else
                CreateDefaultGeometry();

            // First texture (slot 0)
            TextureData texData1 = LoadTextureWIC(workingDir + L"texture_first.png");
            if (!texData1.valid) texData1 = LoadTextureWIC(workingDir + L"texture_first.jpg");
            if (!texData1.valid)
            {
                MessageBoxW(m_windowHandle,
                    (L"Missing texture file!\n\nExpected:\n" + workingDir + L"texture_first.jpg").c_str(),
                    L"Texture Error", MB_OK | MB_ICONERROR);
                return false;
            }
            UploadTextureData(texData1, 0);
        }

        // Execute upload commands
        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, commandLists);
        WaitForGPU();

        // Release upload buffers
        m_textureFirstUpload.Reset();
        m_textureSecondUpload.Reset();

        CreateConstantBuffer();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(m_windowHandle, e.what(), "Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

//  Device Creation
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

//  Command Queue
void DirectXApp::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_cmdQueue)));
}

//  Swap Chain
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
//  Descriptor Heaps
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

    // SRV heap (2 textures)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)));
    }
}

//  Render Target Views
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

//  Depth Stencil Buffer
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

//  Command List and Allocators
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

//  Fence and Sync Objects
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

//  Root Signature
void DirectXApp::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER rootParams[2] = {};

    // Parameter 0: Constant Buffer View
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: Shader Resource View table (textures)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &staticSampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_d3dDevice->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

//  Pipeline State Object
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

void DirectXApp::CreatePipelineState()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring filePath(exePath);
    filePath = filePath.substr(0, filePath.find_last_of(L"\\/") + 1) + L"Shaders.hlsl";

    auto vertexShader = CompileShaderFile(filePath, "VSMain", "vs_5_0");
    auto pixelShader = CompileShaderFile(filePath, "PSMain", "ps_5_0");

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

//  Buffer Helpers
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

//  Geometry Generation
std::vector<Vertex> DirectXApp::GenerateCubeMesh()
{
    float halfSize = 0.5f;
    Vertex vertexData[] =
    {
        { {-halfSize,-halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {0,1} },
        { { halfSize,-halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {1,1} },
        { { halfSize, halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {1,0} },
        { {-halfSize, halfSize, halfSize}, {0,0,1}, {0.9f,0.2f,0.2f,1}, {0,0} },
        { { halfSize,-halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {0,1} },
        { {-halfSize,-halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {1,1} },
        { {-halfSize, halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {1,0} },
        { { halfSize, halfSize,-halfSize}, {0,0,-1}, {0.2f,0.8f,0.2f,1}, {0,0} },
        { {-halfSize,-halfSize,-halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {0,1} },
        { {-halfSize,-halfSize, halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {1,1} },
        { {-halfSize, halfSize, halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {1,0} },
        { {-halfSize, halfSize,-halfSize}, {-1,0,0}, {0.2f,0.2f,0.9f,1}, {0,0} },
        { { halfSize,-halfSize, halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {0,1} },
        { { halfSize,-halfSize,-halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {1,1} },
        { { halfSize, halfSize,-halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {1,0} },
        { { halfSize, halfSize, halfSize}, {1,0,0}, {0.9f,0.9f,0.1f,1}, {0,0} },
        { {-halfSize, halfSize, halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {0,1} },
        { { halfSize, halfSize, halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {1,1} },
        { { halfSize, halfSize,-halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {1,0} },
        { {-halfSize, halfSize,-halfSize}, {0,1,0}, {0.1f,0.9f,0.9f,1}, {0,0} },
        { {-halfSize,-halfSize,-halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {0,1} },
        { { halfSize,-halfSize,-halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {1,1} },
        { { halfSize,-halfSize, halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {1,0} },
        { {-halfSize,-halfSize, halfSize}, {0,-1,0}, {0.9f,0.1f,0.9f,1}, {0,0} },
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

//  Texture Upload
void DirectXApp::UploadTextureData(const TextureData& texData, int textureSlot)
{
    if (!texData.valid || texData.width == 0 || texData.height == 0)
        ThrowIfFailed(E_INVALIDARG, "Invalid texture data provided");

    ComPtr<ID3D12Resource>& targetTexture = (textureSlot == 0) ? m_textureFirst : m_textureSecond;
    ComPtr<ID3D12Resource>& uploadBuffer = (textureSlot == 0) ? m_textureFirstUpload : m_textureSecondUpload;

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

    ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&targetTexture)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowPitch = 0, totalBytes = 0;
    m_d3dDevice->GetCopyableFootprints(
        &textureDesc, 0, 1, 0, &footprint, &numRows, &rowPitch, &totalBytes);

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
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuffer)));

    uint8_t* mappedMemory = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedMemory));
    UINT sourceRowPitch = texData.width * 4;
    for (UINT row = 0; row < texData.height; ++row)
    {
        memcpy(mappedMemory + (UINT64)footprint.Footprint.RowPitch * row,
            texData.pixels.data() + (UINT64)sourceRowPitch * row,
            sourceRowPitch);
    }
    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
    sourceLocation.pResource = uploadBuffer.Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION destLocation = {};
    destLocation.pResource = targetTexture.Get();
    destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destLocation.SubresourceIndex = 0;

    m_cmdList->CopyTextureRegion(&destLocation, 0, 0, 0, &sourceLocation, nullptr);

    D3D12_RESOURCE_BARRIER transitionBarrier = {};
    transitionBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transitionBarrier.Transition.pResource = targetTexture.Get();
    transitionBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    transitionBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    transitionBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &transitionBarrier);

    UINT srvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += textureSlot * srvDescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_d3dDevice->CreateShaderResourceView(targetTexture.Get(), &srvDesc, srvHandle);
}

//  Constant Buffer
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

//  Update
void DirectXApp::OnUpdate(float deltaTime)
{
    m_rotationAngle += 0.8f * deltaTime;
    m_uvOffsetX += 0.05f * deltaTime;
    m_uvOffsetY += 0.02f * deltaTime;

    if (m_uvOffsetX > 1.0f) m_uvOffsetX -= 1.0f;
    if (m_uvOffsetY > 1.0f) m_uvOffsetY -= 1.0f;

    // Вместо XMMatrixIdentity() используй поворот на 180 градусов
    XMMATRIX worldMatrix = XMMatrixRotationY(XM_PI);  // XM_PI = 180 градусов
    XMVECTOR cameraPos = XMVectorSet(0.0f, 1.0f, -3.0f, 1.0f);
    XMVECTOR targetPos = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR upVector = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX viewMatrix = XMMatrixLookAtLH(cameraPos, targetPos, upVector);

    float aspectRatio = (m_screenHeight > 0) ?
        static_cast<float>(m_screenWidth) / m_screenHeight : 1.0f;
    XMMATRIX projMatrix = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(60.0f), aspectRatio, 0.1f, 100.0f);

    ConstantBufferData constantData = {};
    constantData.World = XMMatrixTranspose(worldMatrix);
    constantData.View = XMMatrixTranspose(viewMatrix);
    constantData.Proj = XMMatrixTranspose(projMatrix);
    constantData.LightPos = XMFLOAT4(2.0f, 3.0f, -2.0f, 0.0f);
    constantData.LightColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constantData.CameraPos = XMFLOAT4(0.0f, 1.0f, -3.0f, 1.0f);
    constantData.Tiling = XMFLOAT2(8.0f, 8.0f);
    constantData.UVOffset = XMFLOAT2(m_uvOffsetX, m_uvOffsetY);

    memcpy(m_mappedConstantData, &constantData, sizeof(constantData));
}

//  Render
void DirectXApp::OnRender()
{
    BuildCommandList();

    ID3D12CommandList* commandLists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, commandLists);

    ThrowIfFailed(m_swapChain->Present(1, 0));
    AdvanceToNextFrame();
}

void DirectXApp::BuildCommandList()
{
    ThrowIfFailed(m_cmdAllocators[m_currentBackBuffer]->Reset());
    ThrowIfFailed(m_cmdList->Reset(
        m_cmdAllocators[m_currentBackBuffer].Get(), m_pipelineState.Get()));

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_screenWidth);
    viewport.Height = static_cast<float>(m_screenHeight);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = { 0, 0, m_screenWidth, m_screenHeight };
    m_cmdList->RSSetViewports(1, &viewport);
    m_cmdList->RSSetScissorRects(1, &scissorRect);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_currentBackBuffer].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_currentBackBuffer * m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_cmdList->IASetIndexBuffer(&m_indexBufferView);
    m_cmdList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_cmdList->Close());
}

//  Resize
void DirectXApp::OnResize(int newWidth, int newHeight)
{
    if (newWidth <= 0 || newHeight <= 0) return;
    if (!m_swapChain || !m_d3dDevice) return;

    WaitForGPU();

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
}

//  Synchronization Methods
void DirectXApp::WaitForGPU()
{
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), m_fenceValues[m_currentBackBuffer]));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_currentBackBuffer], m_fenceEvent));
    ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    ++m_fenceValues[m_currentBackBuffer];
}

void DirectXApp::AdvanceToNextFrame()
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