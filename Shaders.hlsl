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
    float4 CameraPosition;
    float4x4 InvView;
    float4x4 InvProj;

float4x4 LightViewProj[4];
float4 CascadeSplits;
float4 ShadowParams; // x = bias, y = shadow map size, z = cascade count
float4 DebugParams; // x = shadow debug mode
};

cbuffer WaterCB : register(b3)
{
    float WaterTime;
    float WaveStrength;
    float WaveSpeed;
    float WaveFrequency;
    float Padding[4];
};

Texture2D gMainTexture : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);
Texture2D<float4> GAlbedoSpec : register(t0);
Texture2D<float4> GWorldPos : register(t1);
Texture2D<float4> GNormal : register(t2);
Texture2D<float4> GDepth : register(t3);
Texture2DArray<float> ShadowMap : register(t4);

SamplerState gSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct ShadowVSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

float4 ShadowVSMain(ShadowVSInput input) : SV_POSITION
{
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    return mul(viewPos, Proj);
}

int GetCascadeIndex(float viewDepth)
{
    if (viewDepth < CascadeSplits.x)
        return 0;

    if (viewDepth < CascadeSplits.y)
        return 1;

    if (viewDepth < CascadeSplits.z)
        return 2;

    return 3;
}

float GetShadowPCF(float3 worldPos, int cascadeIndex)
{
    float4 lightPos = mul(float4(worldPos, 1.0f), LightViewProj[cascadeIndex]);
    lightPos.xyz /= lightPos.w;

    float2 uv = lightPos.xy * float2(0.5f, -0.5f) + 0.5f;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;

    if (lightPos.z < 0.0f || lightPos.z > 1.0f)
        return 1.0f;

    float depth = lightPos.z - ShadowParams.x;
    float texelSize = 1.0f / ShadowParams.y;

    float result = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            result += ShadowMap.SampleCmpLevelZero(
                ShadowSampler,
                float3(uv + float2(x, y) * texelSize, cascadeIndex),
                depth);
        }
    }

    return result / 9.0f;
}

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

float CheckerPattern(float3 worldPos, float3 normal)
{
    float3 absN = abs(normal);
    float2 uv;

    // Выбираем плоскость проекции по направлению нормали
    if (absN.y >= absN.x && absN.y >= absN.z)
        uv = worldPos.xz; // пол / потолок
    else if (absN.x >= absN.y && absN.x >= absN.z)
        uv = worldPos.zy; // стены по X
    else
        uv = worldPos.xy; // стены по Z

    float scale = 1.5f;
    int2 cell = (int2)floor(uv * scale);

    return ((cell.x + cell.y) & 1) ? 1.0f : 0.0f;
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

    float3 directionalL = normalize(-DirectionalLightDirection.xyz);
    float directionalNdotL = saturate(dot(normal, directionalL));

    float viewDepthForCascade = depth * 100.0f;
    int cascadeIndex = GetCascadeIndex(viewDepthForCascade);
    float shadow = GetShadowPCF(worldPos, cascadeIndex);

    int debugMode = (int)DebugParams.x;

    if (debugMode == 1)
    {
        return float4(shadow, shadow, shadow, 1.0f);
    }

    if (debugMode == 2)
    {
        shadow = 1.0f;
    }

    if (debugMode == 3)
    {
        if (cascadeIndex == 0)
            return float4(1.0f, 0.0f, 0.0f, 1.0f);

        if (cascadeIndex == 1)
            return float4(0.0f, 1.0f, 0.0f, 1.0f);

        if (cascadeIndex == 2)
            return float4(0.0f, 0.0f, 1.0f, 1.0f);

        return float4(1.0f, 1.0f, 0.0f, 1.0f);
    }

    float3 lit = AmbientColor.rgb * albedo.rgb;

    float visibleShadow = lerp(0.25f, 1.0f, shadow);

    lit += DirectionalLightColor.rgb *
           directionalNdotL *
           albedo.rgb *
           DirectionalLightColor.a *
           visibleShadow;

    int staticPointCount = 6;
    for (int staticIdx = 0; staticIdx < staticPointCount; ++staticIdx)
    {
        float3 toLight = PointLightPositionRange[staticIdx].xyz - worldPos;
        float dist = length(toLight);
        float range = max(PointLightPositionRange[staticIdx].w, 0.0001f);
        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;

        float3 L = toLight / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, L));
        float intensity = PointLightColorIntensity[staticIdx].a;

        lit += PointLightColorIntensity[staticIdx].rgb *
               ndotl *
               attenuation *
               intensity *
               albedo.rgb;
    }

    int dynamicCount = (int)LightCounts.z;
    for (int dynamicIdx = 0; dynamicIdx < dynamicCount; ++dynamicIdx)
    {
        int idx = staticPointCount + dynamicIdx;

        float3 toLight = PointLightPositionRange[idx].xyz - worldPos;
        float dist = length(toLight);
        float range = max(PointLightPositionRange[idx].w, 0.0001f);
        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;

        float3 L = toLight / max(dist, 0.0001f);
        float ndotl = saturate(dot(normal, L));
        float intensity = PointLightColorIntensity[idx].a;

        lit += PointLightColorIntensity[idx].rgb *
               ndotl *
               attenuation *
               intensity *
               albedo.rgb;
    }

    int spotCount = (int)LightCounts.y;
    for (int spotIdx = 0; spotIdx < spotCount; ++spotIdx)
    {
        float3 toLight = SpotLightPositionRange[spotIdx].xyz - worldPos;
        float dist = length(toLight);
        float range = max(SpotLightPositionRange[spotIdx].w, 0.0001f);
        float3 L = toLight / max(dist, 0.0001f);

        float falloff = saturate(1.0f - dist / range);
        float attenuation = falloff * falloff;

        float3 spotDir = normalize(SpotLightDirectionCosine[spotIdx].xyz);
        float coneCos = SpotLightDirectionCosine[spotIdx].w;
        float spotAmount = saturate(
            (dot(-L, spotDir) - coneCos) / max(1.0f - coneCos, 0.0001f)
        );

        spotAmount = spotAmount * spotAmount * spotAmount;

        float ndotl = saturate(dot(normal, L));
        float intensity = SpotLightColorIntensity[spotIdx].a;

        lit += SpotLightColorIntensity[spotIdx].rgb *
               ndotl *
               attenuation *
               spotAmount *
               intensity *
               albedo.rgb;
    }

    float3 shadowedLit = lit * lerp(0.01f, 1.0f, shadow);

    if (debugMode == 4)
    {
        float shadowMask = 1.0f - smoothstep(0.65f, 0.98f, shadow);

        float checker = CheckerPattern(worldPos, normal);

        float3 checkerDark = float3(0.02f, 0.02f, 0.025f);
        float3 checkerLight = float3(0.85f, 0.85f, 0.85f);
        float3 checkerColor = lerp(checkerDark, checkerLight, checker);

        float3 checkerLit = lerp(shadowedLit, checkerColor * albedo.rgb, shadowMask);

        float3 checkerSRGB = pow(saturate(checkerLit), 1.0f / 2.2f);
        return float4(checkerSRGB, albedo.a);
    }

    float3 litSRGB = pow(saturate(shadowedLit), 1.0f / 2.2f);
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
    float3 PosW : POSITION; // ДОБАВИТЬ эту строку
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
};

HS_OUT VS(VS_IN input)
{
    HS_OUT output;
    output.WorldPos = mul(float4(input.Pos, 1), World).xyz;
    output.Normal = normalize(mul(float4(input.Normal, 0), World).xyz);
    output.Color = input.Color;
    output.Tex = input.Tex;
    
    // НОВО: преобразуем tangent и binormal в мировое пространство
    float3x3 W3 = (float3x3) World;
    output.Tangent = normalize(mul(W3, input.Tangent));
    output.Binormal = normalize(mul(W3, input.Binormal));
    
    return output;
}

// Hull Shader - константы патча с адаптивной тесселяцией
HS_CONST HSConst(InputPatch<HS_OUT, 3> ip, uint pid : SV_PrimitiveID)
{
    HS_CONST output;
    
    // Вычисляем центр патча
    float3 center = (ip[0].WorldPos + ip[1].WorldPos + ip[2].WorldPos) / 3.0f;
    float dist = length(center - CameraPos.xyz);
    
    // Адаптивный фактор на основе расстояния
    float factor = TessellationFactor;
    if (dist > TessMinDist && TessMaxDist > TessMinDist)
    {
        float t = saturate((dist - TessMinDist) / (TessMaxDist - TessMinDist));
        factor = lerp(TessellationFactor, 1.0f, t);
    }
    
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

// Domain Shader
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
    
    // Интерполяция tangent и binormal
    float3 tangent = bary.x * patch[0].Tangent +
                     bary.y * patch[1].Tangent +
                     bary.z * patch[2].Tangent;
    tangent = normalize(tangent);
    
    float3 binormal = bary.x * patch[0].Binormal +
                      bary.y * patch[1].Binormal +
                      bary.z * patch[2].Binormal;
    binormal = normalize(binormal);
    
    // Интерполяция UV
    float2 tex = bary.x * patch[0].Tex +
                 bary.y * patch[1].Tex +
                 bary.z * patch[2].Tex;
    
    float height = gDisplacementMap.Sample(gSampler, tex).r;
    float displacement = (height - 0.5f) * DisplacementStrength;
   //  worldPos += normal * displacement;
    
    float4 viewPos = mul(float4(worldPos, 1), View);
    output.Pos = mul(viewPos, Proj);
    
    output.PosW = worldPos;
    output.Normal = normal;
    output.Tangent = tangent;
    output.Binormal = binormal;
    output.Color = patch[0].Color;
    output.Tex = tex;
    
    return output;
}

float4 PS(DS_OUT input) : SV_TARGET
{
    float3 normalMap = gNormalMap.Sample(gSampler, input.Tex).rgb;
    float3 normalTS = normalMap * 2.0f - 1.0f;
    normalTS = normalize(normalTS);
    
    float3 N = normalize(input.Normal);
    float3 T = normalize(input.Tangent);
    float3 B = normalize(input.Binormal);
    
    float3x3 TBN = float3x3(T, B, N);
    float3 finalNormal = normalize(mul(normalTS, TBN));
    
    float3 lightDir = normalize(LightPos.xyz);
    float diff = max(dot(finalNormal, lightDir), 0);
    float3 color = diff * DiffuseLightColor.rgb;
    color += DiffuseLightColor.rgb * 0.2f;
    
    // Displacement визуализация
    float height = gDisplacementMap.Sample(gSampler, input.Tex).r;
    color = lerp(color, float3(1, 0, 0), height * 0.7f);
    
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
    float3 TangentW : TEXCOORD4;
    float3 BinormalW : TEXCOORD5;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

struct TessGeometryPSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float3 TangentW : TEXCOORD4;
    float3 BinormalW : TEXCOORD5; 
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
    
    // ДОБАВИТЬ
    float3x3 W3 = (float3x3) World;
    output.Tangent = normalize(mul(W3, input.Tangent));
    output.Binormal = normalize(mul(W3, input.Binormal));
    
    return output;
}

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
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("TessHSConst")]
TessHS_OUT TessHS(InputPatch<TessHS_OUT, 3> ip, uint id : SV_OutputControlPointID)
{
    return ip[id];
}

[domain("tri")]
TessDS_Output TessDS(TessHS_CONST input, float3 bary : SV_DomainLocation, const OutputPatch<TessHS_OUT, 3> patch)
{
    TessDS_Output output;
    
    float3 worldPos = bary.x * patch[0].WorldPos
                    + bary.y * patch[1].WorldPos
                    + bary.z * patch[2].WorldPos;
    
    float3 normal = bary.x * patch[0].Normal
                  + bary.y * patch[1].Normal
                  + bary.z * patch[2].Normal;
    
    float2 tex = bary.x * patch[0].Tex
               + bary.y * patch[1].Tex
               + bary.z * patch[2].Tex;
    
    normal = normalize(normal);
    
    float2 clampedTex = clamp(tex, 0.001f, 0.999f);
    
    // Используем SampleLevel с LOD = 0
    float height = gDisplacementMap.SampleLevel(gSampler, clampedTex, 0).r;
    
  //  float displacement = (height - 0.5f) * 0.01f; // Временно 0.01
    
    // Временно используем ВОЛНУ вместо текстуры для проверки
    float wave = sin(worldPos.x * 0.5f) * 0.05f;
    float displacement = wave;
    
    worldPos += normal * displacement;
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normal;
    output.TangentW = normalize(bary.x * patch[0].Tangent + bary.y * patch[1].Tangent + bary.z * patch[2].Tangent);
    output.BinormalW = normalize(bary.x * patch[0].Binormal + bary.y * patch[1].Binormal + bary.z * patch[2].Binormal);
    output.UV = tex;
    output.ViewDepth = viewPos.z;
    
    return output;
}

// Pixel Shader для Geometry Pass с normal mapping
GBufferOutput TessGeometryPSMain(TessGeometryPSInput input)
{
    GBufferOutput o;
    
    float4 albedo = gMainTexture.Sample(gSampler, input.UV);
    
    // ===== ВИЗУАЛИЗАЦИЯ HEIGHT =====
    float height = gDisplacementMap.Sample(gSampler, input.UV).r;
    
    // Временная визуализация: показываем height вместо albedo
    // Черный = 0, Белый = 1, Серый = 0.5
    albedo = float4(height, height, height, 1.0f);
    // ================================
    
    // ДОБАВИТЬ: normal mapping для deferred
    float3 normalMap = gNormalMap.Sample(gSampler, input.UV).rgb;
    float3 normalTS = normalMap * 2.0f - 1.0f;
    normalTS = normalize(normalTS);
    
    float3 N = normalize(input.NormalW);
    float3 T = normalize(input.TangentW);
    float3 B = normalize(input.BinormalW);
    
    float3x3 TBN = float3x3(T, B, N);
    float3 finalNormal = normalize(mul(normalTS, TBN));
    
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(finalNormal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

struct WaterPSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

GBufferOutput WaterPSMain(WaterPSInput input)
{
    GBufferOutput o;
    
    float4 albedo = float4(0.1f, 0.4f, 0.8f, 0.95f);
    
    float2 uv = input.UV * 8.0f;
    float variation = sin(uv.x * 10.0f + WaterTime) * cos(uv.y * 10.0f);
    albedo.rgb += variation * 0.1f;

    float3 normal = normalize(input.NormalW);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(saturate(input.ViewDepth / 100.0f), 0, 0, 1.0f);
    
    return o;
}

struct WaterVSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct WaterVSOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

WaterVSOutput WaterVSMain(VSInput input)
{
    WaterVSOutput output;

    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
    
    float time = WaterTime;
    float strength = WaveStrength;
    float speed = WaveSpeed;
    float freq = WaveFrequency;
    
    float wave1 = sin(worldPos.x * freq + time * speed) *
                  cos(worldPos.z * freq * 0.8f + time * speed * 1.2f);
    float wave2 = sin(worldPos.x * freq * 2.0f + time * speed * 1.5f) * 0.5f;
    float wave3 = cos(worldPos.z * freq * 1.5f - time * speed * 1.8f) * 0.3f;
    float wave4 = sin((worldPos.x * 0.5f + worldPos.z * 0.5f) * freq * 3.0f + time * speed * 2.0f) * 0.2f;
    
    float height = (wave1 + wave2 + wave3 + wave4) * strength;
    worldPos.y = height;
    
    float epsilon = 0.1f;
    float hx1 = sin((worldPos.x + epsilon) * freq + time * speed) *
                cos(worldPos.z * freq * 0.8f + time * speed * 1.2f);
    float hx2 = sin((worldPos.x + epsilon) * freq * 2.0f + time * speed * 1.5f) * 0.5f;
    float hx3 = cos(worldPos.z * freq * 1.5f - time * speed * 1.8f) * 0.3f;
    float hx4 = sin(((worldPos.x + epsilon) * 0.5f + worldPos.z * 0.5f) * freq * 3.0f + time * speed * 2.0f) * 0.2f;
    float heightX = (hx1 + hx2 + hx3 + hx4) * strength;
    
    float hz1 = sin(worldPos.x * freq + time * speed) *
                cos((worldPos.z + epsilon) * freq * 0.8f + time * speed * 1.2f);
    float hz2 = sin(worldPos.x * freq * 2.0f + time * speed * 1.5f) * 0.5f;
    float hz3 = cos((worldPos.z + epsilon) * freq * 1.5f - time * speed * 1.8f) * 0.3f;
    float hz4 = sin((worldPos.x * 0.5f + (worldPos.z + epsilon) * 0.5f) * freq * 3.0f + time * speed * 2.0f) * 0.2f;
    float heightZ = (hz1 + hz2 + hz3 + hz4) * strength;
    
    float3 gradient = float3(height - heightX, 0.0f, height - heightZ);
    float3 normal = normalize(float3(-gradient.x, 1.0f, -gradient.z));
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normal;
    output.UV = input.TexCoord;
    output.ViewDepth = viewPos.z;
    
    return output;
}
// ========== ТЕССЕЛЯЦИЯ ДЛЯ ВОДЫ ==========

struct WaterTessVS_IN
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct WaterTessHS_OUT
{
    float3 WorldPos : WORLDPOS;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
};

struct WaterTessHS_CONST
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

struct WaterTessDS_Output
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

WaterTessHS_OUT WaterTessVS(VS_IN input)
{
    WaterTessHS_OUT output;
    output.WorldPos = mul(float4(input.Pos, 1), World).xyz;
    output.Normal = normalize(mul(float4(input.Normal, 0), World).xyz);
    output.Color = float4(0, 0, 1, 1);
    output.Tex = input.Tex;
    output.Tangent = input.Tangent;
    output.Binormal = input.Binormal;
    return output;
}

WaterTessHS_CONST WaterTessHSConst(InputPatch<WaterTessHS_OUT, 3> ip, uint pid : SV_PrimitiveID)
{
    WaterTessHS_CONST output;
    
    float3 center = (ip[0].WorldPos + ip[1].WorldPos + ip[2].WorldPos) / 3.0f;
    float dist = length(center - CameraPos.xyz);
   
    float maxTess = 48.0f;
    float minTess = 8.0f;
    float maxDist = 50.0f;
    float minDist = 5.0f;
    
    float t = saturate((dist - minDist) / (maxDist - minDist));
    float factor = maxTess * (1.0f - t) + minTess * t;
    
    output.edges[0] = factor;
    output.edges[1] = factor;
    output.edges[2] = factor;
    output.inside = factor;
    
    return output;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("WaterTessHSConst")]
WaterTessHS_OUT WaterTessHS(InputPatch<WaterTessHS_OUT, 3> ip, uint id : SV_OutputControlPointID)
{
    return ip[id];
}

[domain("tri")]
WaterTessDS_Output WaterTessDS(WaterTessHS_CONST input, float3 bary : SV_DomainLocation, const OutputPatch<WaterTessHS_OUT, 3> patch)
{
    WaterTessDS_Output output;
    
    float3 worldPos = bary.x * patch[0].WorldPos +
                      bary.y * patch[1].WorldPos +
                      bary.z * patch[2].WorldPos;
    
    float3 normal = bary.x * patch[0].Normal +
                    bary.y * patch[1].Normal +
                    bary.z * patch[2].Normal;
    
    float2 tex = bary.x * patch[0].Tex +
                 bary.y * patch[1].Tex +
                 bary.z * patch[2].Tex;
    
    normal = normalize(normal);
    
    float time = WaterTime;
    float strength = WaveStrength;
    float speed = WaveSpeed;
    float freq = WaveFrequency;
    
    if (strength < 0.01f)
        strength = 0.6f;
    if (speed < 0.01f)
        speed = 2.5f;
    if (freq < 0.01f)
        freq = 1.5f;
    
    float wave1 = sin(worldPos.x * freq + time * speed) *
                  cos(worldPos.z * freq * 0.8f + time * speed * 1.2f);
    float wave2 = sin(worldPos.x * freq * 2.0f + time * speed * 1.5f) * 0.5f;
    float wave3 = cos(worldPos.z * freq * 1.5f - time * speed * 1.8f) * 0.4f;
    float wave4 = sin((worldPos.x + worldPos.z) * freq * 2.2f + time * speed * 2.2f) * 0.3f;
    
    float height = (wave1 + wave2 + wave3 + wave4) * strength;
    worldPos.y = height;
    
    float epsilon = 0.1f;
    float hx1 = sin((worldPos.x + epsilon) * freq + time * speed) *
                cos(worldPos.z * freq * 0.8f + time * speed * 1.2f);
    float hx2 = sin((worldPos.x + epsilon) * freq * 2.0f + time * speed * 1.5f) * 0.5f;
    float hx3 = cos(worldPos.z * freq * 1.5f - time * speed * 1.8f) * 0.4f;
    float heightX = (hx1 + hx2 + hx3) * strength;
    
    float hz1 = sin(worldPos.x * freq + time * speed) *
                cos((worldPos.z + epsilon) * freq * 0.8f + time * speed * 1.2f);
    float hz2 = sin(worldPos.x * freq * 2.0f + time * speed * 1.5f) * 0.5f;
    float hz3 = cos((worldPos.z + epsilon) * freq * 1.5f - time * speed * 1.8f) * 0.4f;
    float heightZ = (hz1 + hz2 + hz3) * strength;
    
    float3 gradient = float3(height - heightX, 0.0f, height - heightZ);
    float3 finalNormal = normalize(float3(-gradient.x, 1.0f, -gradient.z));
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = finalNormal;
    output.UV = tex;
    output.ViewDepth = viewPos.z;
    
    return output;
}

struct CubeVSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct CubeVSOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

CubeVSOutput CubeVSMain(VSInput input)
{
    CubeVSOutput output;
    
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos.xyz;
    output.NormalW = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.UV = input.TexCoord;
    output.ViewDepth = viewPos.z;
    
    return output;
}

struct CubePSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

GBufferOutput CubePSMain(CubePSInput input)
{
    GBufferOutput o;
    
    // Ярко-синий цвет
    float4 albedo = float4(0.0f, 0.0f, 1.0f, 1.0f);
    
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

// ========== ИНСТАНСИНГ ДЛЯ КУБОВ ==========

struct CubeVSInstancedInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
    
    // Instance data
    float3 InstancePos : INSTANCE_POS;
    float InstanceScale : INSTANCE_SCALE;
    float3 InstanceColor : INSTANCE_COLOR;
    float InstancePadding : TEXCOORD3;
};

struct CubeVSInstancedOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    float4 Color : COLOR;
};

CubeVSInstancedOutput CubeVSInstanced(CubeVSInstancedInput input)
{
    CubeVSInstancedOutput output;
    
    // Масштабируем позицию
    float3 scaledPos = input.Position * input.InstanceScale;
    float3 worldPos = scaledPos + input.InstancePos;
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.UV = input.TexCoord;
    output.ViewDepth = viewPos.z;
    output.Color = float4(input.InstanceColor, 1.0f);
    
    return output;
}

GBufferOutput CubePSInstanced(CubeVSInstancedOutput input) : SV_TARGET
{
    GBufferOutput o;
    
    float4 albedo = input.Color;
    
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

struct VSInstancedInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
    
    // Per-instance data - используем уникальные семантики
    float3 InstancePos : INSTANCEPOS;
    float InstanceScale : INSTANCESCALE;
    float3 InstanceColor : INSTANCECOLOR;
    float InstancePadding : INSTANCEPADDING;
};

struct VSInstancedOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    float4 Color : COLOR;
};

VSInstancedOutput CubeVSInstanced(VSInstancedInput input)
{
    VSInstancedOutput output;
    
    // Масштабируем позицию
    float3 scaledPos = input.Position * input.InstanceScale;
    float3 worldPos = scaledPos + input.InstancePos;
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.UV = input.TexCoord;
    output.ViewDepth = viewPos.z;
    output.Color = float4(input.InstanceColor, 1.0f);
    
    return output;
}

GBufferOutput CubePSInstanced(VSInstancedOutput input) : SV_TARGET
{
    GBufferOutput o;
    
    float4 albedo = input.Color;
    
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

struct LineVSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
};

struct LinePSInput
{
    float4 Position : SV_POSITION;
    float4 Color : COLOR;
};

cbuffer LineCB : register(b0)
{
    float4x4 ViewProj;
};

LinePSInput LineVS(LineVSInput input)
{
    LinePSInput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProj);
    output.Color = input.Color;
    return output;
}

float4 LinePS(LinePSInput input) : SV_TARGET
{
    return input.Color;
}

// ========== LOD1 MESH (Simplified - no tangents) ==========
struct LOD1VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct LOD1VSOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    float4 Color : COLOR;
};

LOD1VSOutput LOD1VSMain(LOD1VSInput input)
{
    LOD1VSOutput output;
    
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos.xyz;
    output.NormalW = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.UV = input.TexCoord;
    output.ViewDepth = viewPos.z;
    output.Color = input.Color;
    
    return output;
}

GBufferOutput LOD1PSMain(LOD1VSOutput input)
{
    GBufferOutput o;
    
    float4 albedo = input.Color;
    
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

// ========== LOD2 MESH (Position only) ==========
struct LOD2VSInput
{
    float3 Position : POSITION;
};

struct LOD2VSOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float ViewDepth : TEXCOORD3;
};

LOD2VSOutput LOD2VSMain(LOD2VSInput input)
{
    LOD2VSOutput output;
    
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos.xyz;
    output.ViewDepth = viewPos.z;
    
    return output;
}

GBufferOutput LOD2PSMain(LOD2VSOutput input)
{
    GBufferOutput o;
    
    // Use distance-based color for LOD2
    float depth = saturate(input.ViewDepth / 100.0f);
    float4 albedo = float4(depth, depth * 0.5f, 1.0f - depth, 1.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

struct LOD1VSInstancedInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD;
    
    float3 InstancePos : INSTANCEPOS;
    float InstanceScale : INSTANCESCALE;
    float3 InstanceColor : INSTANCECOLOR;
    float InstancePadding : INSTANCEPADDING;
};

struct LOD1VSInstancedOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
    float4 Color : COLOR;
};

LOD1VSInstancedOutput LOD1VSInstanced(LOD1VSInstancedInput input)
{
    LOD1VSInstancedOutput output;
    
    float3 scaledPos = input.Position * input.InstanceScale;
    float3 worldPos = scaledPos + input.InstancePos;
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.NormalW = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.UV = input.TexCoord;
    output.ViewDepth = viewPos.z;
    output.Color = float4(input.InstanceColor, 1.0f);
    
    return output;
}

GBufferOutput LOD1PSInstanced(LOD1VSInstancedOutput input)
{
    GBufferOutput o;
    
    float4 albedo = input.Color;
    float3 normal = normalize(input.NormalW);
    float depth = saturate(input.ViewDepth / 100.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}

struct LOD2VSInstancedInput
{
    float3 Position : POSITION;
    
    float3 InstancePos : INSTANCEPOS;
    float InstanceScale : INSTANCESCALE;
    float3 InstanceColor : INSTANCECOLOR;
    float InstancePadding : INSTANCEPADDING;
};

struct LOD2VSInstancedOutput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float ViewDepth : TEXCOORD3;
    float4 Color : COLOR;
};

LOD2VSInstancedOutput LOD2VSInstanced(LOD2VSInstancedInput input)
{
    LOD2VSInstancedOutput output;
    
    float3 scaledPos = input.Position * input.InstanceScale;
    float3 worldPos = scaledPos + input.InstancePos;
    
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    output.PosH = mul(viewPos, Proj);
    output.WorldPos = worldPos;
    output.ViewDepth = viewPos.z;
    output.Color = float4(input.InstanceColor, 1.0f);
    
    return output;
}

GBufferOutput LOD2PSInstanced(LOD2VSInstancedOutput input)
{
    GBufferOutput o;
    
    float depth = saturate(input.ViewDepth / 100.0f);
    float4 albedo = float4(depth, depth * 0.5f, 1.0f - depth, 1.0f);
    
    o.AlbedoSpec = albedo;
    o.WorldPos = float4(input.WorldPos, 1.0f);
    o.Normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
    o.Depth = float4(depth, depth, depth, 1.0f);
    
    return o;
}


struct Particle
{
    float3 Position;
    float Life;
    float3 Velocity;
    float LifeSpan;
    float4 Color;
    float Size;
    float Weight;
    float Age;
    float Rotation;
};

struct SortEntry
{
    uint ParticleIndex;
    float DistanceSq;
    float Padding0;
    float Padding1;
};

RWStructuredBuffer<Particle> g_Particles : register(u0);

#if defined(PARTICLE_INIT) || defined(PARTICLE_UPDATE)
AppendStructuredBuffer<uint> g_DeadListAppend : register(u1);
#endif

#if defined(PARTICLE_EMIT)
ConsumeStructuredBuffer<uint> g_DeadListConsume : register(u1);
#endif

#if defined(PARTICLE_INIT)
RWStructuredBuffer<SortEntry> g_SortList : register(u2);
#endif

#if defined(PARTICLE_UPDATE)
AppendStructuredBuffer<SortEntry> g_AliveSortList : register(u2);
#endif

#if defined(PARTICLE_CLEAR_SORT) || defined(PARTICLE_SORT)
RWStructuredBuffer<SortEntry> g_SortList : register(u2);
#endif

cbuffer ParticleCb : register(b0)
{
    float4 C0;
    float4 C1;
    float4 C2;
    float4 C3;
    float4 C4;
    float4 C5;
    float4 C6;
};

float HashFloat(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return (x & 0x00FFFFFFu) / 16777215.0f;
}

[numthreads(256,1,1)]
void ParticleInitDeadListCS(uint3 DTid : SV_DispatchThreadID)
{
    uint index = DTid.x;
    uint maxParticles = asuint(C0.z);
    if (index >= maxParticles)
        return;

    Particle p = (Particle)0;
    p.Life = -1.0f;
    p.LifeSpan = 0.0f;
    p.Age = 0.0f;
    p.Rotation = 0.0f;
    g_Particles[index] = p;

#if defined(PARTICLE_INIT)
    SortEntry s;
    s.ParticleIndex = 0xFFFFFFFFu;
    s.DistanceSq = -1.0f;
    s.Padding0 = 0.0f;
    s.Padding1 = 0.0f;
    g_SortList[index] = s;

    g_DeadListAppend.Append(index);
#endif
}

[numthreads(256,1,1)]
void ParticleEmitCS(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;

    float3 emitter = C0.xyz;
    uint emitCount = asuint(C0.w);
    float3 baseVel = C1.xyz;
    float time = C1.w;
    float3 velRnd = C2.xyz;
    float minLife = C2.w;
    float maxLife = C3.x;
    float minSize = C3.y;
    float maxSize = C3.z;
    uint seed = asuint(C3.w);
    float4 colorA = C4;
    float4 colorB = C5;
    float emitterRadius = C6.x;

    if (i >= emitCount)
        return;

#if defined(PARTICLE_EMIT)
    uint deadIndex = g_DeadListConsume.Consume();

    uint h0 = i * 1664525u + seed + asuint(time * 1000.0f);
    uint h1 = h0 * 22695477u + 1u;
    uint h2 = h1 * 22695477u + 1u;

    float rx = HashFloat(h0) * 2.0f - 1.0f;
    float ry = HashFloat(h1) * 2.0f - 1.0f;
    float rz = HashFloat(h2) * 2.0f - 1.0f;
    float rand0 = HashFloat(h0 ^ 0x68e31da4u);
    float rand1 = HashFloat(h1 ^ 0xb5297a4du);
    float randY = HashFloat(h2 ^ 0x1b56c4e9u);
    float t = HashFloat(h2 ^ 0x9e3779b9u);
    float angle = rand0 * 6.2831853f;
    float radius = sqrt(rand1) * emitterRadius;

    Particle p;
    p.Position = emitter + float3(cos(angle) * radius, randY * 3.0f, sin(angle) * radius);
    p.Velocity = baseVel + float3(rx * velRnd.x, abs(ry) * velRnd.y, rz * velRnd.z);
    p.LifeSpan = lerp(minLife, maxLife, t);
    p.Life = p.LifeSpan;
    p.Color = lerp(colorA, colorB, t);
    p.Size = lerp(minSize, maxSize, HashFloat(h1 ^ 0x85ebca6bu));
    p.Weight = 0.7f + HashFloat(h2 ^ 0xc2b2ae35u) * 0.8f;
    p.Age = 0.0f;
    p.Rotation = HashFloat(h0 ^ 0x27d4eb2du) * 6.2831853f;

    g_Particles[deadIndex] = p;
#endif
}

[numthreads(256,1,1)]
void ParticleUpdateCS(uint3 DTid : SV_DispatchThreadID)
{
    uint index = DTid.x;

    float dt = C0.x;
    uint maxParticles = asuint(C0.z);
    float3 gravity = C1.xyz;
    float groundY = C1.w;
    float3 cameraPos = C2.xyz;
    uint useGround = asuint(C2.w);

    if (index >= maxParticles)
        return;

    Particle p = g_Particles[index];
    if (p.Life <= 0.0f)
        return;

    p.Age += dt;
    p.Life -= dt;
    p.Velocity += gravity * p.Weight * dt;
    p.Position += p.Velocity * dt;

    if (p.Life <= 0.0f || (useGround != 0 && p.Position.y <= groundY))
    {
        p.Life = -1.0f;
        g_Particles[index] = p;
#if defined(PARTICLE_UPDATE)
        g_DeadListAppend.Append(index);
#endif
        return;
    }

    g_Particles[index] = p;

#if defined(PARTICLE_UPDATE)
    float3 toCam = p.Position - cameraPos;

    SortEntry s;
    s.ParticleIndex = index;
    s.DistanceSq = dot(toCam, toCam);
    s.Padding0 = 0.0f;
    s.Padding1 = 0.0f;
    g_AliveSortList.Append(s);
#endif
}

[numthreads(256,1,1)]
void BitonicSortCS(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    uint elementCount = asuint(C0.x);
    uint subArray = asuint(C0.y);
    uint compareDistance = asuint(C0.z);
    uint sortDescending = asuint(C0.w);

    if (i >= elementCount)
        return;

#if defined(PARTICLE_SORT)
    uint j = i ^ compareDistance;
    if (j > i && j < elementCount)
    {
        SortEntry a = g_SortList[i];
        SortEntry b = g_SortList[j];

        bool ascending = ((i & subArray) == 0);
        if (sortDescending != 0)
            ascending = !ascending;

        bool swapNeeded = ascending ? (a.DistanceSq > b.DistanceSq) : (a.DistanceSq < b.DistanceSq);
        if (swapNeeded)
        {
            g_SortList[i] = b;
            g_SortList[j] = a;
        }
    }
#endif
}

[numthreads(256,1,1)]
void ClearSortListCS(uint3 DTid : SV_DispatchThreadID)
{
    uint index = DTid.x;
    uint maxParticles = asuint(C0.x);

    if (index >= maxParticles)
        return;

#if defined(PARTICLE_CLEAR_SORT)
    SortEntry s;
    s.ParticleIndex = 0xFFFFFFFFu;
    s.DistanceSq = -1.0f;
    s.Padding0 = 0.0f;
    s.Padding1 = 0.0f;
    g_SortList[index] = s;
#endif
}

// =========================
// RENDER
// =========================

StructuredBuffer<Particle> g_ParticlesRO : register(t0);
StructuredBuffer<SortEntry> g_SortListRO : register(t1);

cbuffer ParticleRenderConstants : register(b0)
{
    float4x4 gParticleViewProj;
    float3 gParticleCameraRight;
    float gParticlePadding1;
    float3 gParticleCameraUp;
    float gParticlePadding2;
    float3 gParticleLightDir;
    float gParticleLightIntensity;
    float4 gParticleLightColor;
    float4 gParticleAmbient;
};

struct ParticleVSOutput
{
    float3 CenterW : TEXCOORD0;
    float4 Color : COLOR0;
    float Size : TEXCOORD1;
    uint Alive : TEXCOORD2;
};

ParticleVSOutput ParticleVSMain(uint vertexId : SV_VertexID)
{
    ParticleVSOutput o;

    SortEntry se = g_SortListRO[vertexId];
    if (se.ParticleIndex == 0xFFFFFFFFu)
    {
        o.CenterW = float3(0.0f, 0.0f, 0.0f);
        o.Color = float4(0, 0, 0, 0);
        o.Size = 0.0f;
        o.Alive = 0;
        return o;
    }

    Particle p = g_ParticlesRO[se.ParticleIndex];
    if (p.Life <= 0.0f)
    {
        o.CenterW = float3(0.0f, 0.0f, 0.0f);
        o.Color = float4(0, 0, 0, 0);
        o.Size = 0.0f;
        o.Alive = 0;
        return o;
    }

    float3 L = normalize(-gParticleLightDir);
    float ndl = saturate(dot(float3(0.0f, 1.0f, 0.0f), L) * 0.5f + 0.5f);
    float3 light = gParticleAmbient.rgb + gParticleLightColor.rgb * (gParticleLightIntensity * ndl);

    float lifeRatio = saturate(p.Life / max(p.LifeSpan, 0.001f));

    o.CenterW = p.Position;
    o.Color = float4(saturate(p.Color.rgb * light), lifeRatio);
    o.Size = p.Size;
    o.Alive = 1;
    return o;
}

struct ParticleGSOutput
{
    float4 PositionH : SV_POSITION;
    float4 Color : COLOR0;
};

[maxvertexcount(4)]
void ParticleGSMain(point ParticleVSOutput input[1], inout TriangleStream<ParticleGSOutput> triStream)
{
    if (input[0].Alive == 0 || input[0].Size <= 0.0f || input[0].Color.a <= 0.0f)
        return;

    float halfSize = input[0].Size;
    float3 center = input[0].CenterW;

    float3 corners[4];
    corners[0] = center + (-gParticleCameraRight - gParticleCameraUp) * halfSize;
    corners[1] = center + (-gParticleCameraRight + gParticleCameraUp) * halfSize;
    corners[2] = center + ( gParticleCameraRight - gParticleCameraUp) * halfSize;
    corners[3] = center + ( gParticleCameraRight + gParticleCameraUp) * halfSize;

    ParticleGSOutput v;

    v.Color = input[0].Color;
    v.PositionH = mul(float4(corners[0], 1.0f), gParticleViewProj);
    triStream.Append(v);

    v.Color = input[0].Color;
    v.PositionH = mul(float4(corners[1], 1.0f), gParticleViewProj);
    triStream.Append(v);

    v.Color = input[0].Color;
    v.PositionH = mul(float4(corners[2], 1.0f), gParticleViewProj);
    triStream.Append(v);

    v.Color = input[0].Color;
    v.PositionH = mul(float4(corners[3], 1.0f), gParticleViewProj);
    triStream.Append(v);

    triStream.RestartStrip();
}

float4 ParticlePSMain(ParticleGSOutput input) : SV_TARGET
{
    return input.Color;
}