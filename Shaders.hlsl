// ============================================================
// CONSTANT BUFFERS
// ============================================================

cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4 LightPos;
    float4 DiffuseLightColor;
    float4 CameraPos;
    float2 Tiling;
    float2 UVOffset;
};

cbuffer DeferredLightCB : register(b1)
{
    float4 DirectionalLightDirection;
    float4 DirectionalLightColor;
    float4 AmbientColor;
    float4 LightCounts;
    
    float4 PointLightPositionRange[6];
    float4 PointLightColorIntensity[6];
    float4 SpotLightPositionRange[4];
    float4 SpotLightDirectionCosine[4];
    float4 SpotLightColorIntensity[4];
    float4 ScreenSize;
    float4x4 InvView;
    float4x4 InvProj;
};

// ============================================================
// TEXTURES AND SAMPLERS
// ============================================================

Texture2D gMainTexture : register(t0); // ОДНА текстура
SamplerState gSampler : register(s0);

Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos : register(t1);
Texture2D<float4> GNormal : register(t2);
Texture2D<float4> GDepth : register(t3);

// ============================================================
// FORWARD RENDERING SHADERS
// ============================================================

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct ForwardPSInput
{
    float4 ClipPos : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD2;
};

ForwardPSInput ForwardVSMain(VSInput input)
{
    ForwardPSInput output;
    
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    float4 clipPos = mul(viewPos, Proj);
    
    output.ClipPos = clipPos;
    output.WorldPos = worldPos.xyz;
    output.Normal = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.Color = input.Color;
    output.TexCoord = input.TexCoord; // БЕЗ UVOffset
    
    return output;
}

float4 ForwardPSMain(ForwardPSInput input) : SV_TARGET
{
    float3 N = normalize(input.Normal);
    float3 L = normalize(LightPos.xyz - input.WorldPos);
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 R = reflect(-L, N);
    
    // Ambient
    float ambientStrength = 0.2f;
    float3 ambient = ambientStrength * DiffuseLightColor.rgb;
    
    // Diffuse
    float diff = max(dot(N, L), 0.0f);
    float3 diffuse = diff * DiffuseLightColor.rgb;
    
    // Specular
    float shininess = 32.0f;
    float spec = pow(max(dot(V, R), 0.0f), shininess);
    float3 specular = 0.3f * spec * DiffuseLightColor.rgb;
    
    // ОДНА текстура, БЕЗ анимации
    float4 texColor = gMainTexture.Sample(gSampler, input.TexCoord);
    
    float3 lighting = ambient + diffuse + specular;
    float3 result = lighting * texColor.rgb;
    
    return float4(result, 1.0f);
}

// ============================================================
// DEFERRED GEOMETRY PASS SHADERS
// ============================================================

struct GeometryPSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

struct GBufferOutput
{
    float4 AlbedoSpec : SV_Target0;
    float4 WorldPos : SV_Target1;
    float4 Normal : SV_Target2;
    float4 Depth : SV_Target3;
};

GeometryPSInput GeometryVSMain(VSInput input)
{
    GeometryPSInput o;
    
    float4 posW = mul(float4(input.Position, 1.0f), World);
    float4 posV = mul(posW, View);
    o.PosH = mul(posV, Proj);
    o.WorldPos = posW.xyz;
    o.ViewDepth = posV.z;
    
    float3x3 W3 = (float3x3) World;
    o.NormalW = normalize(mul(W3, input.Normal));
    o.UV = input.TexCoord; // БЕЗ UVOffset
    
    return o;
}

GBufferOutput GeometryPSMain(GeometryPSInput input)
{
    GBufferOutput o;
    
    // ОДНА текстура, БЕЗ анимации
    float4 albedo = gMainTexture.Sample(gSampler, input.UV);
    
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

// ============================================================
// DEFERRED LIGHTING PASS SHADERS
// ============================================================

struct LightPassInput
{
    float4 Position : SV_POSITION;
};

LightPassInput LightVSMain(uint vertexId : SV_VertexID)
{
    LightPassInput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 LightPSMain(LightPassInput input) : SV_TARGET
{
    int2 pixelPos = int2(input.Position.xy);
    
    float4 albedo = GAlbedoSpec.Load(int3(pixelPos, 0));
    float3 worldPos = GWorldPos.Load(int3(pixelPos, 0)).xyz;
    float3 normalEncoded = GNormal.Load(int3(pixelPos, 0)).xyz;
    float depth = GDepth.Load(int3(pixelPos, 0)).x;
    
    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    
    float3 normal = normalize(normalEncoded * 2.0f - 1.0f);
    float3 ambient = AmbientColor.rgb * albedo.rgb;
    float3 lit = ambient;
    
    // Directional light
    float3 directionalL = normalize(-DirectionalLightDirection.xyz);
    float directionalNdotL = saturate(dot(normal, directionalL));
    lit += DirectionalLightColor.rgb * directionalNdotL * albedo.rgb * DirectionalLightColor.a;
    
    // Point lights
    int pointCount = (int) LightCounts.x;
    [loop]
    for (int i = 0; i < pointCount; ++i)
    {
        float3 toLight = PointLightPositionRange[i].xyz - worldPos;
        float dist = length(toLight);
        float range = max(PointLightPositionRange[i].w, 0.0001f);
        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;
        
        float3 L = toLight / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, L));
        float intensity = PointLightColorIntensity[i].a;
        lit += PointLightColorIntensity[i].rgb * ndotl * attenuation * intensity * albedo.rgb;
    }
    
    // Spot lights
    int spotCount = (int) LightCounts.y;
    [loop]
    for (int i = 0; i < spotCount; ++i)
    {
        float3 toLight = SpotLightPositionRange[i].xyz - worldPos;
        float dist = length(toLight);
        float range = max(SpotLightPositionRange[i].w, 0.0001f);
        float3 L = toLight / max(dist, 0.0001f);
        
        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;
        
        float3 spotDir = normalize(SpotLightDirectionCosine[i].xyz);
        float coneCos = SpotLightDirectionCosine[i].w;
        float spotAmount = saturate((dot(-L, spotDir) - coneCos) / max(1.0f - coneCos, 0.0001f));
        spotAmount = spotAmount * spotAmount * spotAmount;
        
        float ndotl = saturate(dot(normal, L));
        float intensity = SpotLightColorIntensity[i].a;
        lit += SpotLightColorIntensity[i].rgb * ndotl * attenuation * spotAmount * intensity * albedo.rgb;
    }
    
    float3 litSRGB = pow(saturate(lit), 1.0f / 2.2f);
    return float4(litSRGB, albedo.a);
}