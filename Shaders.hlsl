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
Texture2D gNormalMap : register(t1); // НОВО: карта нормалей
Texture2D gDisplacementMap : register(t2); // НОВО: карта смещения
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
    
    // Static point lights - используем staticIdx вместо i
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
        lit += PointLightColorIntensity[staticIdx].rgb * ndotl * attenuation * intensity * albedo.rgb;
    }
    
    // Dynamic lights - используем dynamicIdx вместо i
    int dynamicCount = (int) LightCounts.z;
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
        lit += PointLightColorIntensity[idx].rgb * ndotl * attenuation * intensity * albedo.rgb;
    }
    
    // Spot lights - используем spotIdx вместо i
    int spotCount = (int) LightCounts.y;
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
        float spotAmount = saturate((dot(-L, spotDir) - coneCos) / max(1.0f - coneCos, 0.0001f));
        spotAmount = spotAmount * spotAmount * spotAmount;
        
        float ndotl = saturate(dot(normal, L));
        float intensity = SpotLightColorIntensity[spotIdx].a;
        lit += SpotLightColorIntensity[spotIdx].rgb * ndotl * attenuation * spotAmount * intensity * albedo.rgb;
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
    float3 PosW : POSITION; // ДОБАВИТЬ эту строку
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float3 Binormal : BINORMAL;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD;
};
// Vertex Shader
// Vertex Shader
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
    
    // ===== DISPLACEMENT MAPPING =====
    // Простой Sample (без LOD)
    // ===== DISPLACEMENT MAPPING - ТОЛЬКО ЧТЕНИЕ =====
    float height = gDisplacementMap.Sample(gSampler, tex).r;
    // ПОКА НЕ СМЕЩАЕМ
    float displacement = (height - 0.5f) * DisplacementStrength;
   //  worldPos += normal * displacement;
    
    // В клип
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
// Pixel Shader
// Pixel Shader с normal mapping
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
    float3 TangentW : TEXCOORD4; // ДОБАВИТЬ
    float3 BinormalW : TEXCOORD5; // ДОБАВИТЬ
    float2 UV : TEXCOORD2;
    float ViewDepth : TEXCOORD3;
};

struct TessGeometryPSInput
{
    float4 PosH : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float3 TangentW : TEXCOORD4; // ДОБАВИТЬ
    float3 BinormalW : TEXCOORD5; // ДОБАВИТЬ
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
    
    // КЛЮЧЕВОЕ: зажимаем UV в допустимый диапазон
    float2 clampedTex = clamp(tex, 0.001f, 0.999f);
    
    // Используем SampleLevel с LOD = 0
    float height = gDisplacementMap.SampleLevel(gSampler, clampedTex, 0).r;
    
    // Используем очень маленькое смещение для теста
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