#include <d3d11.h>
#include <d3dcompiler.h>
#include <psapi.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>
#include "riderduck/api/app/riderduck_app.h"

static void Check(HRESULT hr)
{
  if(FAILED(hr))
  {
    printf("HRESULT %08lx\n", hr);
    exit(2);
  }
}
template <class T>
static void Name(T *p, const char *name)
{
  Check(p->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(strlen(name)), name));
}
static const char *VS = R"(
cbuffer CameraConstants : register(b0) { row_major float4x4 ViewProjection; float4 CameraPosition; };
float4 main(uint id:SV_VertexID):SV_Position {
 float2 p[3]={float2(-.8,-.8),float2(0,.8),float2(.8,-.8)};
 return mul(ViewProjection,float4(p[id]+CameraPosition.xy*.001,DEPTH_VALUE,1));
})";
static const char *PS = R"(
cbuffer MaterialConstants : register(b0) {float4 Tint;};
float4 main():SV_Target {return float4(Tint.xyz,1);}
)";
static ID3DBlob *Compile(const char *src, const char *profile, int variant = 0)
{
  ID3DBlob *code = nullptr, *error = nullptr;
  D3D_SHADER_MACRO macros[] = {{"DEPTH_VALUE", variant ? ".3" : ".4"}, {nullptr, nullptr}};
  HRESULT hr =
      D3DCompile(src, strlen(src), "owned-metadata-fixture", macros, nullptr, "main", profile,
                 (strcmp(profile, "vs_5_0") ? D3DCOMPILE_DEBUG : 0) | D3DCOMPILE_SKIP_OPTIMIZATION,
                 0, &code, &error);
  if(FAILED(hr) && error)
    printf("%s\n", (const char *)error->GetBufferPointer());
  if(error)
    error->Release();
  Check(hr);
  return code;
}
static void Save(const char *file, ID3DBlob *blob)
{
  std::ofstream f(file, std::ios::binary);
  f.write((const char *)blob->GetBufferPointer(), blob->GetBufferSize());
}
int main(int argc, char **argv)
{
  if(argc < 3)
    return 1;
  if(!strcmp(argv[1], "prepare"))
  {
    auto code = Compile(VS, "vs_5_0", 1);
    ID3DBlob *stripped = nullptr;
    Check(D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                         D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &stripped));
    Save(argv[2], stripped);
    code->Release();
    stripped->Release();
    return 0;
  }
  if(argc < 5)
    return 1;
  const ULONGLONG started = GetTickCount64();
  HMODULE module = LoadLibraryA(argv[1]);
  if(!module)
    return 3;
  RIDERDUCK_API_1_7_0 *api = nullptr;
  auto get = (pRIDERDUCK_GetAPI)GetProcAddress(module, "RIDERDUCK_GetAPI");
  if(!get || !get(eRIDERDUCK_API_Version_1_7_0, (void **)&api))
    return 4;
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  auto create =
      (PFN_D3D11_CREATE_DEVICE)GetProcAddress(LoadLibraryA("d3d11.dll"), "D3D11CreateDevice");
  Check(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
               &device, nullptr, &ctx));
  auto original = Compile(VS, "vs_5_0");
  ID3DBlob *stripped = nullptr;
  Check(D3DStripShader(original->GetBufferPointer(), original->GetBufferSize(),
                       D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &stripped));
  // A separate process compiled this file. Its names are unavailable to this capture process.
  std::ifstream offlineFile(argv[3], std::ios::binary);
  std::vector<char> offline((std::istreambuf_iterator<char>(offlineFile)), {});
  ID3D11VertexShader *vertices[3] = {};
  Check(device->CreateVertexShader(original->GetBufferPointer(), original->GetBufferSize(), nullptr,
                                   &vertices[0]));
  Check(device->CreateVertexShader(stripped->GetBufferPointer(), stripped->GetBufferSize(), nullptr,
                                   &vertices[1]));
  Check(device->CreateVertexShader(offline.data(), offline.size(), nullptr, &vertices[2]));
  for(int i = 0; i < 3; i++)
    Name(vertices[i], i == 0 ? "NamedVS" : i == 1 ? "RuntimeStrippedVS" : "OfflineVS");
  ID3D11PixelShader *pixels[2] = {};
  auto pixel = Compile(PS, "ps_5_0");
  Check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr,
                                  &pixels[0]));
  Name(pixels[0], "MaterialPS");
  // Dynamic GetProcAddress and FromFile must also be observed, without counting nested compiles twice.
  auto compiler = LoadLibraryA("d3dcompiler_47.dll");
  auto compile2 = (decltype(&D3DCompile2))GetProcAddress(compiler, "D3DCompile2");
  auto fromFile = (decltype(&D3DCompileFromFile))GetProcAddress(compiler, "D3DCompileFromFile");
  const char *other =
      "cbuffer PassConstants:register(b0){float4 Colour;} float4 main():SV_Target{return "
      "float4(Colour.xyz,1);}";
  ID3DBlob *second = nullptr;
  Check(compile2(other, strlen(other), "shared-buffer-second-declaration", nullptr, nullptr, "main",
                 "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0, 0, nullptr, 0, &second, nullptr));
  Check(device->CreatePixelShader(second->GetBufferPointer(), second->GetBufferSize(), nullptr,
                                  &pixels[1]));
  Name(pixels[1], "PassPS");
  std::string file = std::string(argv[4]) + "/source.hlsl";
  {
    std::ofstream out(file);
    out << other;
  }
  std::wstring wide(file.begin(), file.end());
  ID3DBlob *fileCode = nullptr;
  Check(fromFile(wide.c_str(), nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0,
                 &fileCode, nullptr));
  fileCode->Release();
  std::vector<std::thread> workers;
  for(int t = 0; t < 4; t++)
    workers.emplace_back([]() {
      for(int i = 0; i < 3; i++)
      {
        auto b = Compile(PS, "ps_5_0");
        b->Release();
      }
    });
  for(auto &worker : workers)
    worker.join();

  // This compiler version is not imported by the fixture and is loaded after hook setup.
  bool wasLoaded43 = GetModuleHandleA("d3dcompiler_43.dll") != nullptr;
  HMODULE lateCompiler = LoadLibraryA("d3dcompiler_43.dll");
  ID3D11PixelShader *lateShader = nullptr;
  if(lateCompiler)
  {
    auto lateCompile = (decltype(&D3DCompile))GetProcAddress(lateCompiler, "D3DCompile");
    const char *lateSource =
        "cbuffer LateCamera:register(b0){float4 Position;} float4 main():SV_Target{return "
        "Position;}";
    ID3DBlob *lateCode = nullptr;
    Check(lateCompile(lateSource, strlen(lateSource), "late-compiler", nullptr, nullptr, "main",
                      "ps_5_0", 0, 0, &lateCode, nullptr));
    Check(device->CreatePixelShader(lateCode->GetBufferPointer(), lateCode->GetBufferSize(),
                                    nullptr, &lateShader));
    lateCode->Release();
    Name(lateShader, "LateCompilerPS");
  }

  // Create the other shader stages, then reference them during capture.
  const char *cs =
      "cbuffer ComputeCamera:register(b0){float4 Eye;} RWTexture2D<float4> dst:register(u0); "
      "[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){dst[id.xy]=Eye;}";
  const char *gs =
      "cbuffer GeometryCamera:register(b0){float4 Shift;} struct V{float4 p:SV_Position;}; "
      "[maxvertexcount(3)]void main(triangle V v[3],inout TriangleStream<V> s){for(int "
      "i=0;i<3;i++){v[i].p.xyz+=Shift.xyz*.001;s.Append(v[i]);}}";
  const char *hs =
      "cbuffer HullCamera:register(b0){float4 Scale;} struct V{float4 p:SV_Position;}; struct "
      "C{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;}; C pc(InputPatch<V,3> v){C "
      "c;c.e[0]=c.e[1]=c.e[2]=c.i=max(1,Scale.x);return c;} "
      "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")]["
      "outputcontrolpoints(3)][patchconstantfunc(\"pc\")]V main(InputPatch<V,3> v,uint "
      "i:SV_OutputControlPointID){return v[i];}";
  const char *ds =
      "cbuffer DomainCamera:register(b0){float4 Shift;} struct V{float4 p:SV_Position;}; struct "
      "C{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;}; [domain(\"tri\")]V main(C "
      "c,float3 b:SV_DomainLocation,const OutputPatch<V,3> v){V "
      "o;o.p=v[0].p*b.x+v[1].p*b.y+v[2].p*b.z;o.p.xyz+=Shift.xyz*.001;return o;}";
  ID3D11ComputeShader *compute = nullptr;
  ID3D11GeometryShader *geometry = nullptr;
  ID3D11GeometryShader *geometrySO = nullptr;
  ID3D11HullShader *hull = nullptr;
  ID3D11DomainShader *domain = nullptr;
  auto bc = Compile(cs, "cs_5_0");
  Check(device->CreateComputeShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &compute));
  bc->Release();
  bc = Compile(gs, "gs_5_0");
  Check(device->CreateGeometryShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &geometry));
  D3D11_SO_DECLARATION_ENTRY declaration = {0, "SV_POSITION", 0, 0, 4, 0};
  UINT stride = 16;
  Check(device->CreateGeometryShaderWithStreamOutput(bc->GetBufferPointer(), bc->GetBufferSize(),
                                                     &declaration, 1, &stride, 1, 0, nullptr,
                                                     &geometrySO));
  bc->Release();
  bc = Compile(hs, "hs_5_0");
  Check(device->CreateHullShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &hull));
  bc->Release();
  bc = Compile(ds, "ds_5_0");
  Check(device->CreateDomainShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &domain));
  bc->Release();
  Name(compute, "Compute");
  Name(geometry, "Geometry");
  Name(geometrySO, "GeometrySO");
  Name(hull, "Hull");
  Name(domain, "Domain");
  float data[64] = {};
  for(int i = 0; i < 4; i++)
    data[i * 5] = 1;
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(data);
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
  ID3D11Buffer *buffers[4] = {};
  for(auto &buffer : buffers)
    Check(device->CreateBuffer(&bd, &init, &buffer));
  Name(buffers[0], "OldSharedName");
  Name(buffers[0], "SharedBuffer");
  const wchar_t unicode[] = L"\u76f8\u673aBuffer";
  Check(buffers[1]->SetPrivateData(WKPDID_D3DDebugObjectNameW, sizeof(unicode) - sizeof(wchar_t),
                                   unicode));
  const char noTerminator[] = {'R', 'a', 'w', 'N', 'a', 'm', 'e'};
  Check(buffers[2]->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(noTerminator), noTerminator));
  Name(buffers[3], "MustBeCleared");
  Check(buffers[3]->SetPrivateData(WKPDID_D3DDebugObjectName, 0, nullptr));
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 64;
  td.MipLevels = td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *textures[4] = {};
  ID3D11RenderTargetView *rt[4] = {};
  for(int i = 0; i < 4; i++)
  {
    Check(device->CreateTexture2D(&td, nullptr, &textures[i]));
    Check(device->CreateRenderTargetView(textures[i], nullptr, &rt[i]));
    char name[20];
    sprintf_s(name, "Output%d", i);
    Name(textures[i], name);
  }
  api->SetCaptureFilePathTemplate(argv[2]);
  api->StartFrameCapture(device, nullptr);
  if(lateShader)
    ctx->PSSetShader(lateShader, nullptr, 0);
  ctx->GSSetShader(geometrySO, nullptr, 0);
  ctx->GSSetShader(geometry, nullptr, 0);
  ctx->HSSetShader(hull, nullptr, 0);
  ctx->DSSetShader(domain, nullptr, 0);
  ctx->CSSetShader(compute, nullptr, 0);
  ctx->ClearState();
  for(int i = 0; i < 4; i++)
  {
    float clear[4] = {.1f, .2f, .3f, 1};
    ctx->ClearRenderTargetView(rt[i], clear);
    ctx->OMSetRenderTargets(1, &rt[i], nullptr);
    D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vertices[i % 3], nullptr, 0);
    ctx->PSSetShader(pixels[i == 3 ? 1 : 0], nullptr, 0);
    // Bind all name variants so each Buffer is included in the capture.
    ctx->VSSetConstantBuffers(0, 4, buffers);
    ctx->PSSetConstantBuffers(0, 1, &buffers[0]);
    ctx->Draw(3, 0);
  }
  if(!api->EndFrameCapture(device, nullptr))
    return 5;
  PROCESS_MEMORY_COUNTERS_EX memory = {};
  memory.cb = sizeof(memory);
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory));
  printf(
      "{\"total_ms\":%llu,\"peak_working_set\":%llu,\"private_bytes\":%llu,\"late_compiler_"
      "present\":%s,\"compiler43_preloaded\":%s}\n",
      GetTickCount64() - started, (unsigned long long)memory.PeakWorkingSetSize,
      (unsigned long long)memory.PrivateUsage, lateCompiler ? "true" : "false",
      wasLoaded43 ? "true" : "false");
  // Save exact API outputs independently of diagnostics for identity comparisons.
  Save((std::string(argv[4]) + "/named.dxbc").c_str(), original);
  Save((std::string(argv[4]) + "/stripped.dxbc").c_str(), stripped);
  ctx->ClearState();
  ctx->Flush();
  return 0;
}
