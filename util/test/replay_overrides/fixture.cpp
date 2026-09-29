// Standalone owned D3D11 fixture for replay overrides. Build with the Windows SDK.
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static void Name(T *p, const char *n)
{
  p->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(strlen(n)), n);
}
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11DeviceContext1 *ctx1;
static ID3DUserDefinedAnnotation *ann;
static ID3D11VertexShader *vs, *fullscreen;
static ID3D11PixelShader *ps, *taa, *post, *copyPS;
static ID3D11ComputeShader *taaCS;
static ID3D11SamplerState *sampler;
static ID3D11Buffer *cb, *dynamicCB, *rangeCB, *args, *indices, *effectCB;
static ID3D11DepthStencilView *sceneDepth, *referenceDepth, *rangeDepth;
struct Constants
{
  float view[16], tint[4], projection[16], offset[4];
};
struct Tex
{
  ID3D11Texture2D *t;
  ID3D11RenderTargetView *rt;
  ID3D11ShaderResourceView *srv;
  ID3D11UnorderedAccessView *uav;
  UINT w, h;
};
static Tex Texture(const char *name, UINT w = 64, UINT h = 64,
                   DXGI_FORMAT fmt = DXGI_FORMAT_R32G32B32A32_FLOAT)
{
  Tex t = {};
  t.w = w;
  t.h = h;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = d.ArraySize = 1;
  d.SampleDesc.Count = 1;
  d.Format = fmt;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  Check(dev->CreateTexture2D(&d, nullptr, &t.t));
  Name(t.t, name);
  Check(dev->CreateRenderTargetView(t.t, nullptr, &t.rt));
  Check(dev->CreateShaderResourceView(t.t, nullptr, &t.srv));
  Check(dev->CreateUnorderedAccessView(t.t, nullptr, &t.uav));
  return t;
}
static ID3DBlob *Shader(const char *source, const char *entry, const char *profile)
{
  ID3DBlob *b = nullptr, *err = nullptr;
  HRESULT hr = D3DCompile(source, strlen(source), "fixture", nullptr, nullptr, entry, profile,
                          D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &b, &err);
  if(FAILED(hr) && err)
    printf("%s\n", (const char *)err->GetBufferPointer());
  if(err)
    err->Release();
  Check(hr);
  return b;
}
static void Viewport(ID3D11DeviceContext *c, UINT w, UINT h)
{
  D3D11_VIEWPORT v = {0, 0, float(w), float(h), 0, 1};
  c->RSSetViewports(1, &v);
}
static ID3D11DepthStencilView *Depth(const char *name)
{
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = d.Height = 64;
  d.MipLevels = d.ArraySize = 1;
  d.SampleDesc.Count = 1;
  d.Format = DXGI_FORMAT_D32_FLOAT;
  d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D *tex = nullptr;
  Check(dev->CreateTexture2D(&d, nullptr, &tex));
  Name(tex, name);
  ID3D11DepthStencilView *view = nullptr;
  Check(dev->CreateDepthStencilView(tex, nullptr, &view));
  tex->Release();
  return view;
}
static void Scene(Tex &target, float shift, int ranged = 0)
{
  ctx->ClearState();
  float clear[4] = {0.04f, 0.08f, 0.12f, 1};
  ctx->ClearRenderTargetView(target.rt, clear);
  ID3D11DepthStencilView *depth = ranged ? rangeDepth : (shift != 0 ? referenceDepth : sceneDepth);
  ctx->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 1, 0);
  auto draw = [&](ID3D11DeviceContext *c, int i) {
    c->OMSetRenderTargets(1, &target.rt, depth);
    Viewport(c, target.w, target.h);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    Constants data = {};
    for(int n = 0; n < 4; n++)
    {
      data.view[n * 5] = 1;
      data.projection[n * 5] = 1;
    }
    data.view[3] = shift;
    data.tint[i % 3] = 0.8f;
    data.tint[3] = 1;
    data.offset[0] = float(i - 1) * 0.45f;
    data.offset[2] = 0.4f;
    ID3D11Buffer *b = (i == 2) ? dynamicCB : cb;
    if(ranged)
    {
      unsigned char bytes[1024] = {};
      memcpy(bytes + 256, &data, sizeof(data));
      ctx->UpdateSubresource(rangeCB, 0, nullptr, bytes, 0, 0);
      UINT first = 16, count = ranged == 2 ? 4096 : 16;
      ctx1->VSSetConstantBuffers1(0, 1, &rangeCB, &first, &count);
    }
    else
    {
      if(i == 2)
      {
        D3D11_MAPPED_SUBRESOURCE m = {};
        Check(c->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        memcpy(m.pData, &data, sizeof(data));
        c->Unmap(b, 0);
      }
      else
        c->UpdateSubresource(b, 0, nullptr, &data, 0, 0);
      c->VSSetConstantBuffers(0, 1, &b);
    }
    if(i == 0)
      c->Draw(3, 0);
    else if(i == 1)
      c->DrawInstancedIndirect(args, 0);
    else
    {
      c->IASetIndexBuffer(indices, DXGI_FORMAT_R32_UINT, 0);
      c->DrawIndexedInstanced(3, 1, 0, 0, 0);
    }
  };
  draw(ctx, 0);
  draw(ctx, 1);
  if(!ranged)
  {
    ID3D11DeviceContext *deferred = nullptr;
    Check(dev->CreateDeferredContext(0, &deferred));
    draw(deferred, 2);
    ID3D11CommandList *list = nullptr;
    Check(deferred->FinishCommandList(FALSE, &list));
    ctx->ExecuteCommandList(list, TRUE);
    list->Release();
    deferred->Release();
  }
  else
    draw(ctx, 2);
}
static void Fullscreen(Tex &input, Tex &history, Tex &output, ID3D11PixelShader *shader)
{
  ctx->ClearState();
  ctx->OMSetRenderTargets(1, &output.rt, nullptr);
  Viewport(ctx, output.w, output.h);
  ctx->VSSetShader(fullscreen, nullptr, 0);
  ctx->PSSetShader(shader, nullptr, 0);
  ID3D11ShaderResourceView *srvs[] = {input.srv, history.srv};
  ctx->PSSetShaderResources(0, 2, srvs);
  ctx->PSSetSamplers(0, 1, &sampler);
  ctx->PSSetConstantBuffers(1, 1, &effectCB);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->Draw(3, 0);
  ctx->ClearState();
}

int main(int argc, char **argv)
{
  if(argc < 3)
    return 1;
  HMODULE rd = LoadLibraryA(argv[1]);
  if(!rd)
    return 3;
  RIDERDUCK_API_1_7_0 *api = nullptr;
  auto get = (pRIDERDUCK_GetAPI)GetProcAddress(rd, "RIDERDUCK_GetAPI");
  if(!get || !get(eRIDERDUCK_API_Version_1_7_0, (void **)&api))
    return 4;
  auto create =
      (PFN_D3D11_CREATE_DEVICE)GetProcAddress(LoadLibraryA("d3d11.dll"), "D3D11CreateDevice");
  Check(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
               nullptr, &ctx));
  Check(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1));
  Check(ctx->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), (void **)&ann));
  const char *scene = R"(
cbuffer Camera : register(b0) { row_major float4x4 View; float4 Tint; row_major float4x4 Projection; float4 Offset; float4 Padding[54]; float4 OutOfBounds; };
struct V {float4 p:SV_Position;float4 c:COLOR;};
V VS(uint id:SV_VertexID){float2 p[3]={float2(-.22,-.65),float2(0,.65),float2(.22,-.65)};V v;v.p=mul(Projection,mul(View,float4(float3(p[id],0)+Offset.xyz,1)));v.c=Tint+OutOfBounds;return v;}
float4 PS(V v):SV_Target{return v.c;})";
  const char *effects = R"(
struct V {float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
cbuffer Temporal : register(b1) {float4 Blend;}
Texture2D<float4> scene:register(t0);Texture2D<float4> history:register(t1);SamplerState smp:register(s0);
float4 TAA(V v):SV_Target{return scene.SampleLevel(smp,v.uv,0)*(1-Blend.x)+history.SampleLevel(smp,v.uv,0)*Blend.x;}
float4 Copy(V v):SV_Target{return scene.SampleLevel(smp,v.uv,0);}
float4 Post(V v):SV_Target{float4 c=scene.SampleLevel(smp,v.uv,0);return float4(c.rgb/(c.rgb+1),1);}
RWTexture2D<float4> output:register(u0);
[numthreads(8,8,1)]void CS(uint3 id:SV_DispatchThreadID){output[id.xy]=scene.Load(int3(id.xy,0))*(1-Blend.y)+history.Load(int3(id.xy,0))*Blend.y;})";
  ID3DBlob *b = Shader(scene, "VS", "vs_5_0");
  Check(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs));
  b->Release();
  Name(vs, "SceneVS");
  b = Shader(scene, "PS", "ps_5_0");
  Check(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps));
  b->Release();
  b = Shader(effects, "VS", "vs_5_0");
  Check(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fullscreen));
  b->Release();
  for(auto entry :
      {std::pair<const char *, ID3D11PixelShader **>("TAA", &taa), {"Post", &post}, {"Copy", &copyPS}})
  {
    b = Shader(effects, entry.first, "ps_5_0");
    Check(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, entry.second));
    b->Release();
  }
  b = Shader(effects, "CS", "cs_5_0");
  Check(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &taaCS));
  b->Release();
  D3D11_SAMPLER_DESC sp = {};
  sp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sp.AddressU = sp.AddressV = sp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sp.MaxLOD = D3D11_FLOAT32_MAX;
  Check(dev->CreateSamplerState(&sp, &sampler));
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(Constants);
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  Check(dev->CreateBuffer(&bd, nullptr, &cb));
  Name(cb, "MixedConstants");
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  Check(dev->CreateBuffer(&bd, nullptr, &dynamicCB));
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.CPUAccessFlags = 0;
  bd.ByteWidth = 1024;
  Check(dev->CreateBuffer(&bd, nullptr, &rangeCB));
  UINT arguments[4] = {3, 1, 0, 0};
  D3D11_SUBRESOURCE_DATA init = {arguments, 0, 0};
  bd = {};
  bd.ByteWidth = 16;
  bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  Check(dev->CreateBuffer(&bd, &init, &args));
  UINT index[3] = {0, 1, 2};
  init.pSysMem = index;
  bd = {};
  bd.ByteWidth = 12;
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  Check(dev->CreateBuffer(&bd, &init, &indices));
  Tex input = Texture("Scene"), reference = Texture("Reference"), ranged = Texture("RangedScene"),
      history = Texture("History"), psOut = Texture("TAA_PS"), csOut = Texture("TAA_CS"),
      large = Texture("Upscaled", 128, 128), final = Texture("Final", 128, 128),
      expected = Texture("ExpectedFinal", 128, 128), copy = Texture("CopyScratch");
  sceneDepth = Depth("SceneDepth");
  referenceDepth = Depth("ReferenceDepth");
  rangeDepth = Depth("RangeDepth");
  float blend[4] = {.75f, .5f, 0, 0};
  bd = {};
  bd.ByteWidth = 16;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  init.pSysMem = blend;
  Check(dev->CreateBuffer(&bd, &init, &effectCB));
  Tex converted = Texture("Converted", 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
  ID3D11Texture2D *arraySource = nullptr, *arrayDest = nullptr, *mips = nullptr, *msaa = nullptr;
  ID3D11RenderTargetView *arraySourceRT = nullptr, *arrayDestRT = nullptr, *mip0 = nullptr,
                         *mip1 = nullptr, *msaaRT = nullptr;
  ID3D11ShaderResourceView *mipSRV = nullptr;
  D3D11_TEXTURE2D_DESC extra = {};
  extra.Width = extra.Height = 64;
  extra.MipLevels = 2;
  extra.ArraySize = 2;
  extra.SampleDesc.Count = 1;
  extra.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  extra.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Check(dev->CreateTexture2D(&extra, nullptr, &arraySource));
  Name(arraySource, "ArraySource");
  Check(dev->CreateTexture2D(&extra, nullptr, &arrayDest));
  Name(arrayDest, "ArrayDest");
  D3D11_RENDER_TARGET_VIEW_DESC view = {};
  view.Format = extra.Format;
  view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  view.Texture2DArray.MipSlice = 1;
  view.Texture2DArray.FirstArraySlice = 1;
  view.Texture2DArray.ArraySize = 1;
  Check(dev->CreateRenderTargetView(arraySource, &view, &arraySourceRT));
  view.Texture2DArray.FirstArraySlice = 0;
  Check(dev->CreateRenderTargetView(arrayDest, &view, &arrayDestRT));
  extra.ArraySize = 1;
  extra.MipLevels = 2;
  extra.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  Check(dev->CreateTexture2D(&extra, nullptr, &mips));
  Name(mips, "MipTexture");
  view = {};
  view.Format = extra.Format;
  view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  Check(dev->CreateRenderTargetView(mips, &view, &mip0));
  view.Texture2D.MipSlice = 1;
  Check(dev->CreateRenderTargetView(mips, &view, &mip1));
  Check(dev->CreateShaderResourceView(mips, nullptr, &mipSRV));
  extra.MipLevels = 1;
  extra.MiscFlags = 0;
  extra.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  extra.SampleDesc.Count = 4;
  extra.BindFlags = D3D11_BIND_RENDER_TARGET;
  Check(dev->CreateTexture2D(&extra, nullptr, &msaa));
  Name(msaa, "MSAA");
  Check(dev->CreateRenderTargetView(msaa, nullptr, &msaaRT));
  api->SetCaptureFilePathTemplate(argv[2]);
  api->StartFrameCapture(dev, nullptr);
  float hc[4] = {.8f, .1f, .9f, 1};
  ctx->ClearRenderTargetView(history.rt, hc);
  ann->BeginEvent(L"Scene");
  Scene(input, 0);
  ann->EndEvent();
  ann->BeginEvent(L"Reference");
  Scene(reference, .35f);
  ann->EndEvent();
  ann->BeginEvent(L"RangedScene");
  Scene(ranged, 0, true);
  ann->EndEvent();
  ann->BeginEvent(L"TAA_PS");
  Fullscreen(input, history, psOut, taa);
  ann->EndEvent();
  ann->BeginEvent(L"TAA_CS");
  ctx->CSSetShader(taaCS, nullptr, 0);
  ctx->CSSetConstantBuffers(1, 1, &effectCB);
  ID3D11ShaderResourceView *srvs[] = {psOut.srv, history.srv};
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &csOut.uav, nullptr);
  ctx->Dispatch(8, 8, 1);
  ctx->ClearState();
  ann->EndEvent();
  ann->BeginEvent(L"Upscale_TAA");
  Fullscreen(csOut, history, large, taa);
  ann->EndEvent();
  ann->BeginEvent(L"ToneMap");
  Fullscreen(large, history, final, post);
  ann->EndEvent();
  ann->BeginEvent(L"ExpectedFinal");
  Fullscreen(reference, history, expected, post);
  ann->EndEvent();
  float ui[4] = {1, 1, 0, 1};
  D3D11_RECT rect = {0, 0, 8, 8};
  ann->BeginEvent(L"UI");
  ctx1->ClearView(final.rt, ui, &rect, 1);
  ctx1->ClearView(expected.rt, ui, &rect, 1);
  ann->EndEvent();
  ann->BeginEvent(L"CopiesAndClears");
  ctx->CopyResource(copy.t, input.t);
  ctx->ClearRenderTargetView(input.rt, hc);
  ann->EndEvent();
  ann->BeginEvent(L"InPlace");
  ctx->ClearRenderTargetView(final.rt, hc);
  ann->EndEvent();
  ann->BeginEvent(L"ExtraInit");
  float green[4] = {.25f, .5f, .75f, 1}, red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
  ctx->ClearRenderTargetView(arraySourceRT, green);
  ctx->ClearRenderTargetView(arrayDestRT, red);
  ctx->ClearRenderTargetView(converted.rt, red);
  ctx->ClearRenderTargetView(mip0, blue);
  ctx->ClearRenderTargetView(mip1, red);
  ctx->ClearRenderTargetView(msaaRT, blue);
  ann->EndEvent();
  ann->BeginEvent(L"ArrayPass");
  ctx->ClearRenderTargetView(arrayDestRT, hc);
  ann->EndEvent();
  ann->BeginEvent(L"ConvertPass");
  ctx->ClearRenderTargetView(converted.rt, hc);
  ann->EndEvent();
  ann->BeginEvent(L"GenerateMips");
  ctx->GenerateMips(mipSRV);
  ann->EndEvent();
  ann->BeginEvent(L"Resolve");
  ctx->ResolveSubresource(converted.t, 0, msaa, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
  ann->EndEvent();
  ctx->ClearRenderTargetView(copy.rt, green);
  ann->BeginEvent(L"Discard");
  ctx1->DiscardResource(copy.t);
  ann->EndEvent();
  ann->BeginEvent(L"WindowPastBuffer");
  Scene(ranged, 0, 2);
  ann->EndEvent();
  if(!api->EndFrameCapture(dev, nullptr))
    return 5;
  printf("CAPTURE_OK count=%u\n", api->GetNumCaptures());
  ctx->ClearState();
  ctx->Flush();
  return 0;
}
