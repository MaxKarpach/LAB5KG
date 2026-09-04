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
    void CreateTessellationConstantBuffer();

private:
    ComPtr<ID3D12Resource> m_cubeVertexBuffer;
    ComPtr<ID3D12Resource> m_cubeIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_cubeVertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_cubeIndexBufferView = {};
    UINT m_cubeIndexCount = 0;
    DirectX::XMMATRIX m_cubeWorldMatrix;

    void CreateCubeGeometry();
    void CreateCubePipelineState();
    ComPtr<ID3D12PipelineState> m_cubePipelineState;
    ComPtr<ID3D12Resource> m_normalTexture;      // Карта нормалей
    ComPtr<ID3D12RootSignature> m_tessellationRootSignature;
    void CreateTessellationRootSignature();
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
    void UploadTexture(const TextureData& texData, int textureSlot = 0, bool isNormalMap = false);
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

    float GetAdaptiveTessellationFactor();

    struct TessellationConstants
    {
        float TessellationFactor;
        float DisplacementStrength;
        float TessMinDist;
        float TessMaxDist;
        float Padding[4];
    };

    ComPtr<ID3D12Resource> m_waterVertexBuffer;
    ComPtr<ID3D12Resource> m_waterIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_waterVertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_waterIndexBufferView = {};
    UINT m_waterIndexCount = 0;
    DirectX::XMMATRIX m_waterWorldMatrix;
    float m_waterTime = 0.0f;

    void CreateWaterPlane();
    void CreateWaterPipelineState();
    ComPtr<ID3D12PipelineState> m_waterPipelineState;
    // Tessellation constant buffer
    ComPtr<ID3D12Resource> m_tessellationCB;
    TessellationConstants* m_mappedTessellationData = nullptr;

    // Rendering mode
    bool m_useDeferredRendering = true;
    ComPtr<ID3D12Resource> m_cubeConstantBuffer;
    ConstantBufferData* m_mappedCubeConstantData = nullptr;
    void CreateCubeConstantBuffer();

    struct CubeInstance
    {
        DirectX::XMFLOAT3 Position;
        float Scale;
        DirectX::XMFLOAT3 Color;
    };

    static const int CUBE_COUNT = 1000;
    std::vector<CubeInstance> m_cubes;
    std::vector<ComPtr<ID3D12Resource>> m_cubeConstantBuffers;
    std::vector<ConstantBufferData*> m_mappedCubeConstantDataArray;

    void CreateWaterConstantBuffer();

    struct WaterConstantData
    {
        float Time;
        float WaveStrength;
        float WaveSpeed;
        float WaveFrequency;
        float Padding[4];
    };

    ComPtr<ID3D12Resource> m_waterConstantBuffer;
    WaterConstantData* m_mappedWaterConstantData = nullptr;
    // В private секцию добавить:
    ComPtr<ID3D12PipelineState> m_waterTessPipeline;
    void CreateWaterTessellationPipeline();

    // Frustum culling
    struct Frustum
    {
        DirectX::XMFLOAT4 Planes[6]; // Left, Right, Top, Bottom, Near, Far
    };

    void UpdateFrustum();
    bool IsSphereInFrustum(const DirectX::XMFLOAT3& center, float radius) const;
    Frustum m_frustum;
    float m_frustumNearPlane = 0.1f;
    float m_frustumFarPlane = 1000.0f;

    // Для отладки
    int m_visibleCubesCount = 0;
    float m_cullingUpdateTimer = 0.0f;

    struct AABB
    {
        DirectX::XMFLOAT3 Min;
        DirectX::XMFLOAT3 Max;

        AABB() : Min(0, 0, 0), Max(0, 0, 0) {}
        AABB(const DirectX::XMFLOAT3& min, const DirectX::XMFLOAT3& max) : Min(min), Max(max) {}

        void Transform(const DirectX::XMMATRIX& matrix)
        {
            // Трансформируем 8 вершин и находим новые min/max
            DirectX::XMFLOAT3 corners[8];
            corners[0] = DirectX::XMFLOAT3(Min.x, Min.y, Min.z);
            corners[1] = DirectX::XMFLOAT3(Max.x, Min.y, Min.z);
            corners[2] = DirectX::XMFLOAT3(Min.x, Max.y, Min.z);
            corners[3] = DirectX::XMFLOAT3(Max.x, Max.y, Min.z);
            corners[4] = DirectX::XMFLOAT3(Min.x, Min.y, Max.z);
            corners[5] = DirectX::XMFLOAT3(Max.x, Min.y, Max.z);
            corners[6] = DirectX::XMFLOAT3(Min.x, Max.y, Max.z);
            corners[7] = DirectX::XMFLOAT3(Max.x, Max.y, Max.z);

            DirectX::XMVECTOR vecMin = DirectX::XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 1);
            DirectX::XMVECTOR vecMax = DirectX::XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 1);

            for (int i = 0; i < 8; ++i)
            {
                DirectX::XMVECTOR corner = DirectX::XMLoadFloat3(&corners[i]);
                corner = DirectX::XMVector3TransformCoord(corner, matrix);
                vecMin = DirectX::XMVectorMin(vecMin, corner);
                vecMax = DirectX::XMVectorMax(vecMax, corner);
            }

            DirectX::XMStoreFloat3(&Min, vecMin);
            DirectX::XMStoreFloat3(&Max, vecMax);
        }

        bool Intersects(const Frustum& frustum) const
        {
            // Проверяем AABB против 6 плоскостей
            for (int i = 0; i < 6; ++i)
            {
                const DirectX::XMFLOAT4& plane = frustum.Planes[i];

                // Находим самую дальнюю вершину AABB в направлении плоскости
                float px = plane.x > 0 ? Max.x : Min.x;
                float py = plane.y > 0 ? Max.y : Min.y;
                float pz = plane.z > 0 ? Max.z : Min.z;

                float distance = plane.x * px + plane.y * py + plane.z * pz + plane.w;

                if (distance < 0)
                    return false;  // Полностью вне frustum
            }
            return true;
        }
    };

    std::vector<AABB> m_cubeAABBs;
    bool IsAABBInFrustum(const AABB& aabb) const;
    bool IsAABBInFrustum(const AABB& aabb, const Frustum& frustum) const;
    // В DirectXApp.h добавить:
    struct KDTreeNode
    {
        AABB Bounds;                           // Bounding box узла
        int Axis;                              // Ось разделения (0=X,1=Y,2=Z)
        float SplitPos;                        // Позиция разделения
        bool IsLeaf;                           // Флаг листа
        std::vector<int> CubeIndices;          // Индексы кубов (только для листьев)
        std::unique_ptr<KDTreeNode> Left;      // Левый ребенок
        std::unique_ptr<KDTreeNode> Right;     // Правый ребенок

        KDTreeNode() : Axis(0), SplitPos(0.0f), IsLeaf(false) {}
    };

    std::unique_ptr<KDTreeNode> m_kdTreeRoot;
    void BuildKDTree();
    void BuildKDTreeNode(std::unique_ptr<KDTreeNode>& node, std::vector<int>& indices,
        const AABB& bounds, int depth);
    void CullKDTree(KDTreeNode* node, const Frustum& frustum, std::vector<int>& outVisible);
    // В DirectXApp.h, в private секцию:
    static constexpr int MIN_CUBES_PER_NODE = 10;  // Минимум кубов в листе
    static constexpr int MAX_KD_DEPTH = 12;        // Максимальная глубина

    enum class CullingMode
    {
        None,      // Рендерим все кубы
        Frustum,   // Простой frustum culling
        Octree     // Octree culling
    };

    // Флаги для управления рендером
    const bool renderSponza = false;
    const bool renderWater = true;

    void SetCullingMode(CullingMode mode);
    CullingMode GetCullingMode() const { return m_cullingMode; }

    CullingMode m_cullingMode = CullingMode::Octree;  // По умолчанию Octree
    // Для обработки нажатий клавиш
    bool m_prevCKey = false;
    bool m_prevVKey = false;

    void CreateInstancedPipeline();
    void CreateInstanceBuffer();
    void UpdateInstanceBuffer(const std::vector<int>& visibleIndices);

    ComPtr<ID3D12Resource> m_instanceBuffer;
    ComPtr<ID3D12Resource> m_instanceUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_instanceBufferView = {};
    ComPtr<ID3D12PipelineState> m_instancedPipelineState;
    bool m_useInstancing = true;

    // Буфер для временного хранения данных видимых инстансов
    std::vector<InstanceData> m_visibleInstanceData;
};