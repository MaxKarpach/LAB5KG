cbuffer ConstantBuffer : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4   LightPos;
    float4   LightColor;
    float4   CameraPos;
    float2   Tiling;
    float2   UVOffset;
};

Texture2D gTextureFirst  : register(t0);
Texture2D gTextureSecond : register(t1);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct PSInput
{
    float4 ClipPos  : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos  = mul(worldPos, View);
    float4 clipPos  = mul(viewPos,  Proj);

    output.ClipPos  = clipPos;
    output.WorldPos = worldPos.xyz;
    output.Normal   = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    output.Color    = input.Color;
    
    output.TexCoord = input.TexCoord;

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 N = normalize(input.Normal);
    float3 L = normalize(LightPos.xyz - input.WorldPos);
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 R = reflect(-L, N);

    // ---- Ambient ----
    float  ambientStrength = 0.2f;
    float3 ambient = ambientStrength * LightColor.rgb;

    // ---- Diffuse ----
    float  diff = max(dot(N, L), 0.0f);
    float3 diffuse = diff * LightColor.rgb;

    // ---- Specular ----
    float  shininess = 32.0f;
    float  spec = pow(max(dot(V, R), 0.0f), shininess);
    float3 specular = 0.3f * spec * LightColor.rgb;

    float cellsPerSide = 6.0f;
    
    float2 animatedUV = input.TexCoord + UVOffset * 0.5f;  // Множитель регулирует скорость
    
    float2 cellIndex = floor(animatedUV * cellsPerSide);
    float2 cellUV = frac(animatedUV * cellsPerSide);
    
    float isEven = fmod(cellIndex.x + cellIndex.y, 2.0f);
    
    // Выбираем текстуру
    float4 texColor;
    if (isEven < 0.5f)
    {
        texColor = gTextureFirst.Sample(gSampler, cellUV);
    }
    else
    {
        texColor = gTextureSecond.Sample(gSampler, cellUV);
    }

    // ---- Combine ----
    float3 lighting = ambient + diffuse + specular;
    float3 result = lighting * texColor.rgb;

    return float4(result, 1.0f);
}