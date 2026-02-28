#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#include "Types.h"
#include "TextureLoader.h"
#include "ObjLoader.h"

using Microsoft::WRL::ComPtr;

static const UINT BACK_BUFFER_COUNT = 2;

class DirectXApp
{
public:
    DirectXApp(HWND windowHandle, int windowWidth, int windowHeight);
    ~DirectXApp();

    bool Initialize();
    void Update(float deltaTime);
    void Render();
    void Resize(int newWidth, int newHeight);

private:
    // ----- Setup methods -----
    void CreateD3DDevice();
    void CreateCommandQueue();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthBuffer();
    void CreateCommandList();
    void CreateSyncObjects();
    void CreateRootSignature();
    void CreatePipelineState();
    void CreateDefaultGeometry();
    void ImportModel(const std::wstring& modelPath);
    void UploadTexture(const TextureData& texData, int textureSlot = 0);
    void CreateConstantBuffer();

    // ----- Synchronization -----
    void FlushCommandQueue();
    void MoveToNextFrame();

    // ----- Rendering -----
    void BuildCommandList();

    // ----- Geometry utilities -----
    std::vector<Vertex> GenerateCubeMesh();
    std::vector<UINT>   GenerateCubeIndices();
    void BuildMeshBuffers(const std::vector<Vertex>& vertices,
        const std::vector<UINT>& indices);
    void CreateUploadBuffer(const void* data, UINT64 dataSize,
        ComPtr<ID3D12Resource>& outputBuffer);

    ComPtr<ID3DBlob> CompileShaderFile(const std::wstring& filePath,
        const std::string& entryPoint,
        const std::string& shaderModel);

    // Window properties
    HWND m_windowHandle;
    int  m_screenWidth;
    int  m_screenHeight;

    // Animation
    float m_rotationAngle = 0.0f;
    float m_uvOffsetX = 0.0f;
    float m_uvOffsetY = 0.0f;

    // D3D12 core
    ComPtr<ID3D12Device>              m_d3dDevice;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<IDXGISwapChain3>           m_swapChain;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap>      m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap>      m_srvHeap;
    UINT                              m_rtvDescriptorSize = 0;

    // Render targets
    ComPtr<ID3D12Resource>            m_backBuffers[BACK_BUFFER_COUNT];
    ComPtr<ID3D12Resource>            m_depthStencil;

    // Command objects
    ComPtr<ID3D12CommandAllocator>    m_cmdAllocators[BACK_BUFFER_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    // Pipeline
    ComPtr<ID3D12RootSignature>       m_rootSignature;
    ComPtr<ID3D12PipelineState>       m_pipelineState;

    // Geometry
    ComPtr<ID3D12Resource>            m_vertexBuffer;
    ComPtr<ID3D12Resource>            m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW          m_vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW           m_indexBufferView = {};
    UINT                              m_indexCount = 0;

    // Textures
    ComPtr<ID3D12Resource>            m_textureFirst;
    ComPtr<ID3D12Resource>            m_textureFirstUpload;
    ComPtr<ID3D12Resource>            m_textureSecond;
    ComPtr<ID3D12Resource>            m_textureSecondUpload;

    // Constant buffer
    ComPtr<ID3D12Resource>            m_constantBuffer;
    ConstantBufferData* m_mappedConstantData = nullptr;

    // Fence
    ComPtr<ID3D12Fence>               m_fence;
    UINT64                            m_fenceValues[BACK_BUFFER_COUNT] = {};
    HANDLE                            m_fenceEvent = nullptr;
    UINT                              m_currentBackBuffer = 0;
};