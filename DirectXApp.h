#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#include "Types.h"
#include "TextureLoader.h"
#include "ObjLoader.h"
#include "GBuffer.h"
#include "RenderingSystem.h"
#include "Camera.h"
#include "InputDevice.h"

using Microsoft::WRL::ComPtr;

static const UINT BACK_BUFFER_COUNT = 2;

class DirectXApp
{
public:
    DirectXApp(HWND windowHandle, int windowWidth, int windowHeight, const InputDevice* inputDevice);
    ~DirectXApp();

    bool Initialize();
    void Update(float deltaTime);
    void Render();
    void Resize(int newWidth, int newHeight);
    // В public секцию
    float ReadDepthAtPixel(float screenX, float screenY);
    DirectX::XMFLOAT3 ScreenToWorld(float screenX, float screenY, float depth);

private:
    // DirectXApp.h - добавить в private секцию:
    // Добавьте в класс DirectXApp новые поля
    ComPtr<ID3D12Resource> m_normalTexture;      // Карта нормалей
    ComPtr<ID3D12Resource> m_normalTextureUpload;
    ComPtr<ID3D12Resource> m_displacementTexture; // Карта смещения
    ComPtr<ID3D12Resource> m_displacementTextureUpload;

    bool m_useNormalMap = true;
    bool m_useDisplacement = true;
    float m_tessellationFactor = 3.0f;
    // ----- Shooting system -----
    void Shoot();
    std::vector<DynamicLight> m_dynamicLights;
    float m_shootCooldown = 0.0f;
    static constexpr float SHOOT_COOLDOWN_TIME = 0.2f;
    static const int MAX_DYNAMIC_LIGHTS = 200;
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
    void UploadTexture(const TextureData& texData, int textureSlot = 0,bool isNormalMap = false);
    void CreateConstantBuffer();

    // ----- Deferred rendering methods -----
    void CreateDeferredRootSignatures();
    void CreateDeferredPipelines();
    void CreateLightingConstantBuffer();
    void RenderDeferredFrame();
    void RenderGeometryPass();
    void RenderLightingPass();
    void UpdateLightingConstants();

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

    // DirectXApp.h - обновить структуру DeferredLightCB:

    struct DeferredLightCB
    {
        DirectX::XMFLOAT4 DirectionalLightDirection;
        DirectX::XMFLOAT4 DirectionalLightColor;
        DirectX::XMFLOAT4 AmbientColor;
        DirectX::XMFLOAT4 LightCounts;  // x=static point, y=spot, z=dynamic, w=reserved
        DirectX::XMFLOAT4 PointLightPositionRange[32];
        DirectX::XMFLOAT4 PointLightColorIntensity[32];
        DirectX::XMFLOAT4 SpotLightPositionRange[4];
        DirectX::XMFLOAT4 SpotLightDirectionCosine[4];
        DirectX::XMFLOAT4 SpotLightColorIntensity[4];
        DirectX::XMFLOAT4 ScreenSize;
        DirectX::XMFLOAT4X4 InvView;
        DirectX::XMFLOAT4X4 InvProj;
    };

    // Window properties
    HWND m_windowHandle;
    int  m_screenWidth;
    int  m_screenHeight;

    // Input device (for camera control)
    const InputDevice* m_inputDevice;

    // Camera
    Camera m_camera;

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
    ComPtr<ID3D12RootSignature>       m_deferredLightingRootSignature;
    ComPtr<ID3D12PipelineState>       m_pipelineState;
    ComPtr<ID3D12PipelineState>       m_deferredGeometryPSO;
    ComPtr<ID3D12PipelineState>       m_deferredLightingPSO;

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

    // Constant buffers
    ComPtr<ID3D12Resource>            m_constantBuffer;
    ConstantBufferData* m_mappedConstantData = nullptr;

    ComPtr<ID3D12Resource>            m_deferredLightConstantBuffer;
    uint8_t* m_deferredLightCBMappedData = nullptr;

    // GBuffer
    std::unique_ptr<GBuffer>          m_gbuffer;
    // Добавьте в private секцию
    void CreateTessellationPipeline();
    void CreateTessellationGeometryPipeline();
    ComPtr<ID3D12PipelineState> m_tessPipeline;
    ComPtr<ID3D12PipelineState> m_tessGeometryPSO;

    // Rendering System
    std::unique_ptr<RenderingSystem>  m_renderingSystem;

    // Fence
    ComPtr<ID3D12Fence>               m_fence;
    UINT64                            m_fenceValues[BACK_BUFFER_COUNT] = {};
    HANDLE                            m_fenceEvent = nullptr;
    UINT                              m_currentBackBuffer = 0;

    // Rendering mode
    bool m_useDeferredRendering = true;
};