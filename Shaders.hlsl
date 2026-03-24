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

cbuffer TessellationCB : register(b2)
{
    float TessellationFactor;
    float DisplacementStrength;
    float TessMinDist;
    float TessMaxDist;
};

cbuffer DeferredLightCB : register(b1)
{
    float4 DirectionalLightDirection;
    float4 DirectionalLightColor;
    float4 AmbientColor;
    float4 LightCounts;
    
    float4 PointLightPositionRange[32];
    float4 PointLightColorIntensity[32];
    float4 SpotLightPositionRange[4];
    float4 SpotLightDirectionCosine[4];
    float4 SpotLightColorIntensity[4];
    float4 ScreenSize;
    float4x4 InvView;
    float4x4 InvProj;
};

Texture2D gMainTexture : register(t0);
SamplerState gSampler : register(s0);
Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos : register(t1);
Texture2D<float4> GNormal : register(t2);
Texture2D<float4> GDepth : register(t3);

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
    output.TexCoord = input.TexCoord;
    
    return output;
}

float4 ForwardPSMain(ForwardPSInput input) : SV_TARGET
{
    float3 N = normalize(input.Normal);
    float3 L = normalize(LightPos.xyz - input.WorldPos);
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 R = reflect(-L, N);
    
    float ambientStrength = 0.2f;
    float3 ambient = ambientStrength * DiffuseLightColor.rgb;
    
    float diff = max(dot(N, L), 0.0f);
    float3 diffuse = diff * DiffuseLightColor.rgb;
    
    float shininess = 32.0f;
    float spec = pow(max(dot(V, R), 0.0f), shininess);
    float3 specular = 0.3f * spec * DiffuseLightColor.rgb;
    
    float4 texColor = gMainTexture.Sample(gSampler, input.TexCoord);
    
    float3 lighting = ambient + diffuse + specular;
    float3 result = lighting * texColor.rgb;
    
    return float4(result, 1.0f);
}

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
    o.UV = input.TexCoord;
    
    return o;
}

GBufferOutput GeometryPSMain(GeometryPSInput input)
{
    GBufferOutput o;
    
    float4 albedo = gMainTexture.Sample(gSampler, input.UV);
    
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

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
    float3 lit = AmbientColor.rgb * albedo.rgb;
    
    // Directional light
    float3 directionalL = normalize(-DirectionalLightDirection.xyz);
    float directionalNdotL = saturate(dot(normal, directionalL));
    lit += DirectionalLightColor.rgb * directionalNdotL * albedo.rgb * DirectionalLightColor.a;
    
    // Static point lights
    int staticPointCount = 6;
    for (int i = 0; i < staticPointCount; ++i)
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
    
    int dynamicCount = (int) LightCounts.z;
    for (int i = 0; i < dynamicCount; ++i)
    {
        int idx = staticPointCount + i;
        float3 toLight = PointLightPositionRange[idx].xyz - worldPos;
        float dist = length(toLight);
        float range = max(PointLightPositionRange[idx].w, 0.0001f);
        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;
        
        float3 L = toLight / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, L));
        float intensity = PointLightColorIntensity[idx].a;
        lit += PointLightColorIntensity[idx].rgb * ndotl * attenuation * intensity * albedo.rgb;
    }
    
    // Spot lights
    int spotCount = (int) LightCounts.y;
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

// ========== ТЕССЕЛЯЦИЯ ==========

struct VS_IN
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct HS_OUT
{
    float3 WorldPos : WORLDPOS;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct HS_CONST
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

struct DS_OUT
{
    float4 Pos : SV_POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
};

// Vertex Shader
HS_OUT VS(VS_IN input)
{
    HS_OUT output;
    output.WorldPos = mul(float4(input.Pos, 1), World).xyz;
    output.Normal = normalize(mul(float4(input.Normal, 0), World).xyz);
    output.Color = input.Color;
    output.Tex = input.Tex;
    output.Tangent = input.Tangent;
    output.Binormal = input.Binormal;
    return output;
}

// Hull Shader - константы патча (ИСПРАВЛЕНО)
HS_CONST HSConst(InputPatch<HS_OUT, 3> ip, uint pid : SV_PrimitiveID)
{
    HS_CONST output;
    
    // Используем значение из константного буфера
    float factor = TessellationFactor;
    
    output.edges[0] = factor;
    output.edges[1] = factor;
    output.edges[2] = factor;
    output.inside = factor;
    
    return output;
}

// Hull Shader - контрольные точки
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConst")]
HS_OUT HS(InputPatch<HS_OUT, 3> ip, uint id : SV_OutputControlPointID)
{
    return ip[id];
}

// Domain Shader (с добавленным визуальным смещением для проверки)
[domain("tri")]
DS_OUT DS(HS_CONST input, float3 bary : SV_DomainLocation, const OutputPatch<HS_OUT, 3> patch)
{
    DS_OUT output;
    
    // Интерполяция позиции
    float3 worldPos = bary.x * patch[0].WorldPos +
                      bary.y * patch[1].WorldPos +
                      bary.z * patch[2].WorldPos;
    
    // Интерполяция нормали
    float3 normal = bary.x * patch[0].Normal +
                    bary.y * patch[1].Normal +
                    bary.z * patch[2].Normal;
    normal = normalize(normal);
    
    // ВИЗУАЛЬНАЯ ПРОВЕРКА ТЕССЕЛЯЦИИ - смещаем вершины по нормали
    // Чем выше фактор тесселяции, тем сильнее смещение
    // Если хотите убрать, закомментируйте следующую строку
    worldPos += normal * 0.02f * TessellationFactor;
    
    float2 tex = bary.x * patch[0].Tex +
                 bary.y * patch[1].Tex +
                 bary.z * patch[2].Tex;
    
    // В КЛИП
    float4 viewPos = mul(float4(worldPos, 1), View);
    output.Pos = mul(viewPos, Proj);
    
    output.Normal = normal;
    output.Color = patch[0].Color;
    output.Tex = tex;
    
    return output;
}

// Pixel Shader
float4 PS(DS_OUT input) : SV_TARGET
{
    float3 lightDir = normalize(LightPos.xyz);
    float diff = max(dot(input.Normal, lightDir), 0);
    float3 color = diff * DiffuseLightColor.rgb;
    return float4(color, 1);
}

// ========== ТЕССЕЛЯЦИЯ ДЛЯ DEFERRED GEOMETRY PASS ==========

struct TessVS_IN
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct TessHS_OUT
{
    float3 WorldPos : WORLDPOS;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct TessHS_CONST
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

struct TessDS_Output
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

struct TessGeometryPSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

// Vertex Shader
TessHS_OUT TessVS(VS_IN input)
{
    TessHS_OUT output;
    output.WorldPos = mul(float4(input.Pos, 1), World).xyz;
    output.Normal = normalize(mul(float4(input.Normal, 0), World).xyz);
    output.Color = input.Color;
    output.Tex = input.Tex;
    output.Tangent = input.Tangent;
    output.Binormal = input.Binormal;
    return output;
}

// Hull Shader - константы патча (ИСПРАВЛЕНО)
TessHS_CONST TessHSConst(InputPatch<TessHS_OUT, 3> ip, uint pid : SV_PrimitiveID)
{
    TessHS_CONST output;
    
    // Используем значение из константного буфера
    float factor = TessellationFactor;
    
    output.edges[0] = factor;
    output.edges[1] = factor;
    output.edges[2] = factor;
    output.inside = factor;
    
    return output;
}

// Hull Shader - контрольные точки
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("TessHSConst")]
TessHS_OUT TessHS(InputPatch<TessHS_OUT, 3> ip, uint id : SV_OutputControlPointID)
{
    return ip[id];
}

// Domain Shader (с добавленным визуальным смещением для проверки)
[domain("tri")]
TessDS_Output TessDS(TessHS_CONST input, float3 bary : SV_DomainLocation, const OutputPatch<TessHS_OUT, 3> patch)
{
    TessDS_Output output;
    
    float3 worldPos = bary.x * patch[0].WorldPos +
                      bary.y * patch[1].WorldPos +
                      bary.z * patch[2].WorldPos;
    
    float3 normal = bary.x * patch[0].Normal +
                    bary.y * patch[1].Normal +
                    bary.z * patch[2].Normal;
    normal = normalize(normal);
    
    // ВИЗУАЛЬНАЯ ПРОВЕРКА ТЕССЕЛЯЦИИ - смещаем вершины по нормали
    // Чем выше фактор тесселяции, тем сильнее смещение
    // Если хотите убрать, закомментируйте следующую строку
    worldPos += normal * 0.02f * TessellationFactor;
    
    float2 tex = bary.x * patch[0].Tex +
                 bary.y * patch[1].Tex +
                 bary.z * patch[2].Tex;
    
    float4 viewPos = mul(float4(worldPos, 1), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normal;
    output.UV = tex;
    output.ViewDepth = viewPos.z;
    
    return output;
}

// Pixel Shader для Geometry Pass
GBufferOutput TessGeometryPSMain(TessGeometryPSInput input)
{
    GBufferOutput o;
    
    float4 albedo = gMainTexture.Sample(gSampler, input.UV);
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}