// Session-only D3D11 replay edits. Original captured resources and bindings remain authoritative.
#include "d3d11_replay.h"
#include "driver/dx/official/d3dcompiler.h"
#include "d3d11_context.h"
#include "d3d11_device.h"
#include "d3d11_resources.h"
#include "d3d11_shader_cache.h"

namespace
{
ID3D11Texture2D *OverrideTexture(ResourceId id)
{
  auto it = WrappedID3D11Texture2D1::m_TextureList.find(id);
  return it == WrappedID3D11Texture2D1::m_TextureList.end()
             ? NULL
             : UNWRAP(WrappedID3D11Texture2D1, it->second.m_Texture);
}

bool OverrideDraw(D3D11Chunk chunk)
{
  switch(chunk)
  {
    case D3D11Chunk::Draw:
    case D3D11Chunk::DrawIndexed:
    case D3D11Chunk::DrawInstanced:
    case D3D11Chunk::DrawIndexedInstanced:
    case D3D11Chunk::DrawAuto:
    case D3D11Chunk::DrawInstancedIndirect:
    case D3D11Chunk::DrawIndexedInstancedIndirect: return true;
    default: return false;
  }
}

void BindOverrideCB(WrappedID3D11DeviceContext *context, ShaderStage stage, UINT slot,
                    ID3D11Buffer *buffer, UINT offset, UINT count)
{
  ID3D11DeviceContext *ctx = context->GetReal();
  ID3D11DeviceContext1 *ctx1 = context->GetReal1();
#define BIND_CB(s, name)                                                  \
  case ShaderStage::s:                                                    \
    if(ctx1)                                                              \
      ctx1->name##SetConstantBuffers1(slot, 1, &buffer, &offset, &count); \
    else                                                                  \
      ctx->name##SetConstantBuffers(slot, 1, &buffer);                    \
    break;
  switch(stage)
  {
    BIND_CB(Vertex, VS)
    BIND_CB(Hull, HS)
    BIND_CB(Domain, DS) BIND_CB(Geometry, GS) BIND_CB(Pixel, PS) BIND_CB(Compute, CS) default
        : break;
  }
#undef BIND_CB
}

// Copies and edits must not inherit a captured conditional-rendering predicate.
struct OverridePredicate
{
  ID3D11DeviceContext *ctx;
  ID3D11Predicate *predicate = NULL;
  BOOL value = FALSE;
  OverridePredicate(ID3D11DeviceContext *c) : ctx(c)
  {
    ctx->GetPredication(&predicate, &value);
    ctx->SetPredication(NULL, FALSE);
  }
  ~OverridePredicate()
  {
    ctx->SetPredication(predicate, value);
    SAFE_RELEASE(predicate);
  }
};
}

rdcstr D3D11Replay::ValidateReplayConstants(const rdcarray<ReplayConstantOverride> &overrides)
{
  if(m_Proxy)
    return "Replay overrides require a local D3D11 replay device.";
  for(const ReplayConstantOverride &o : overrides)
  {
    if(o.firstEvent == 0 || o.firstEvent > o.lastEvent || o.shader == ResourceId() ||
       uint32_t(o.stage) > uint32_t(ShaderStage::Compute) ||
       o.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || o.data.empty() ||
       o.byteOffset > 65536 || o.data.size() > 65536 - o.byteOffset)
      return "Invalid constant override: check event range, shader, stage, slot and byte range.";
    if(WrappedShader::m_ShaderList.find(o.shader) == WrappedShader::m_ShaderList.end())
      return "Constant override shader is not present in this capture.";
  }
  return {};
}

rdcstr D3D11Replay::SetReplayOverrides(const rdcarray<ReplayConstantOverride> &constants,
                                     const rdcarray<ReplayPassOverride> &passes)
{
  rdcstr error = ValidateReplayConstants(constants);
  if(!error.empty()) return error;
  // Pass resources are prepared before committing by SetReplayPassOverrides. Constants
  // have already been validated and their installation cannot fail after that commit.
  error = SetReplayPassOverrides(passes);
  if(!error.empty()) return error;
  return SetReplayConstantOverrides(constants);
}

rdcstr D3D11Replay::SetReplayConstantOverrides(const rdcarray<ReplayConstantOverride> &overrides)
{
  rdcstr error = ValidateReplayConstants(overrides);
  if(!error.empty()) return error;
  m_OverrideLegacy = Process::GetEnvVariable("RIDERDUCK_BENCHMARK_LEGACY_CAMERA") == "1";
  m_OverrideComparisons = m_OverrideCopyBytes = m_OverrideCopies = 0;
  m_OverrideAllocations = m_OverrideIndexBuilds = 0;
  bool sameLayout = overrides.size() == m_ConstantOverrides.size();
  for(size_t i = 0; sameLayout && i < overrides.size(); i++)
  {
    const ReplayConstantOverride &a = overrides[i], &b = m_ConstantOverrides[i];
    sameLayout = a.stage == b.stage && a.shader == b.shader && a.slot == b.slot &&
                 a.firstEvent == b.firstEvent && a.lastEvent == b.lastEvent &&
                 a.byteOffset == b.byteOffset && a.data.size() == b.data.size();
  }
  if(!sameLayout)
  {
    m_OverrideIndex.clear();
    m_AllOverrideIndices.clear();
    for(uint32_t i = 0; i < overrides.size(); i++)
    {
      const ReplayConstantOverride &o = overrides[i];
      m_OverrideIndex[std::make_pair(o.stage, o.shader)][o.slot].push_back(i);
      m_AllOverrideIndices.push_back(i);
    }
    m_OverrideIndexBuilds++;
  }
  m_ConstantOverrides = overrides;
  m_ReplayOverrideError.clear();
  return {};
}

rdcstr D3D11Replay::GetReplayOverrideStats()
{
  return StringFormat::Fmt(
      "{\"rule_comparisons\":%llu,\"copy_bytes\":%llu,\"copies\":%llu,"
      "\"allocations\":%llu,\"index_rebuilds\":%llu,\"legacy\":%s}",
      (unsigned long long)m_OverrideComparisons, (unsigned long long)m_OverrideCopyBytes,
      (unsigned long long)m_OverrideCopies, (unsigned long long)m_OverrideAllocations,
      (unsigned long long)m_OverrideIndexBuilds, m_OverrideLegacy ? "true" : "false");
}

void D3D11Replay::ReleaseOverrideColours(rdcarray<OverrideColour> &colours)
{
  for(OverrideColour &c : colours)
  {
    SAFE_RELEASE(c.srv);
    SAFE_RELEASE(c.rtv);
    SAFE_RELEASE(c.snapshot);
    SAFE_RELEASE(c.target);
  }
  colours.clear();
}

rdcstr D3D11Replay::CreateOverrideBlit()
{
  if(m_OverrideVS && m_OverridePS && m_OverrideSampler && m_OverrideRaster)
    return {};
  SAFE_RELEASE(m_OverrideVS);
  SAFE_RELEASE(m_OverridePS);
  SAFE_RELEASE(m_OverrideSampler);
  SAFE_RELEASE(m_OverrideRaster);
  const char *source = R"(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
  V v; v.uv = float2((id << 1) & 2, id & 2);
  v.pos = float4(v.uv * float2(2,-2) + float2(-1,1), 0, 1); return v;
}
Texture2D<float4> inputColour : register(t0);
SamplerState linearClamp : register(s0);
float4 PS(V v) : SV_Target0 { return inputColour.SampleLevel(linearClamp, v.uv, 0); }
)";
  ID3DBlob *blob = NULL;
  rdcstr error = m_pDevice->GetShaderCache()->GetShaderBlob(source, "VS", 0, {}, "vs_5_0", &blob);
  if(!blob)
    return "Cannot compile colour bypass vertex shader: " + error;
  HRESULT hr = m_pDevice->GetReal()->CreateVertexShader(blob->GetBufferPointer(),
                                                        blob->GetBufferSize(), NULL, &m_OverrideVS);
  SAFE_RELEASE(blob);
  if(FAILED(hr))
    return "Cannot create colour bypass vertex shader.";
  error = m_pDevice->GetShaderCache()->GetShaderBlob(source, "PS", 0, {}, "ps_5_0", &blob);
  if(!blob)
    return "Cannot compile colour bypass pixel shader: " + error;
  hr = m_pDevice->GetReal()->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                               NULL, &m_OverridePS);
  SAFE_RELEASE(blob);
  if(FAILED(hr))
    return "Cannot create colour bypass pixel shader.";
  D3D11_SAMPLER_DESC samp = {};
  samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp.MaxLOD = D3D11_FLOAT32_MAX;
  hr = m_pDevice->GetReal()->CreateSamplerState(&samp, &m_OverrideSampler);
  if(FAILED(hr))
    return "Cannot create colour bypass sampler.";
  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  hr = m_pDevice->GetReal()->CreateRasterizerState(&raster, &m_OverrideRaster);
  return FAILED(hr) ? "Cannot create colour bypass rasterizer." : rdcstr();
}

rdcstr D3D11Replay::SetReplayPassOverrides(const rdcarray<ReplayPassOverride> &overrides)
{
  if(m_Proxy)
    return "Replay overrides require a local D3D11 replay device.";
  rdcarray<OverrideColour> colours;
  rdcstr error;
  for(size_t i = 0; i < overrides.size() && error.empty(); i++)
  {
    const ReplayPassOverride &o = overrides[i];
    if(!o.firstEvent || o.firstEvent > o.lastEvent || (o.bypass && o.colours.empty()))
    {
      error = "Invalid pass range or empty colour bypass mapping.";
      break;
    }
    for(size_t j = 0; j < i; j++)
      if((o.bypass || overrides[j].bypass) && o.firstEvent <= overrides[j].lastEvent &&
         overrides[j].firstEvent <= o.lastEvent)
        error = "Colour bypass ranges cannot overlap another pass override.";
    if(!error.empty() || !o.bypass)
      continue;
    for(const ReplayColourMapping &mapping : o.colours)
    {
      ID3D11Texture2D *src = OverrideTexture(mapping.source);
      ID3D11Texture2D *dst = OverrideTexture(mapping.destination);
      if(!src || !dst)
      {
        error = "Colour bypass requires existing 2D textures.";
        break;
      }
      D3D11_TEXTURE2D_DESC sd, dd;
      src->GetDesc(&sd);
      dst->GetDesc(&dd);
      if(dd.Usage == D3D11_USAGE_IMMUTABLE)
      {
        error = "Colour bypass cannot write to an immutable output texture.";
        break;
      }
      if(sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1 ||
         mapping.sourceMip >= sd.MipLevels || mapping.sourceSlice >= sd.ArraySize ||
         mapping.destinationMip >= dd.MipLevels || mapping.destinationSlice >= dd.ArraySize)
      {
        error = "Colour bypass requires single-sample textures and valid mip/slice indices.";
        break;
      }
      DXGI_FORMAT sf = mapping.sourceFormat ? DXGI_FORMAT(mapping.sourceFormat) : sd.Format;
      DXGI_FORMAT df = mapping.destinationFormat ? DXGI_FORMAT(mapping.destinationFormat) : dd.Format;
      ResourceFormat sr = MakeResourceFormat(sf), dr = MakeResourceFormat(df);
      if(GetTypelessFormat(sf) != GetTypelessFormat(sd.Format) ||
         GetTypelessFormat(df) != GetTypelessFormat(dd.Format) ||
         (sr.compType != CompType::Float && sr.compType != CompType::UNorm) ||
         (dr.compType != CompType::Float && dr.compType != CompType::UNorm) || IsDepthFormat(sf) ||
         IsDepthFormat(df) || IsBlockFormat(sf) || IsBlockFormat(df) || IsSRGBFormat(sf) ||
         IsSRGBFormat(df) || (sd.BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
         (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL))
      {
        error =
            "Choose compatible linear Float/UNorm colour views (no typeless, depth, compressed or "
            "sRGB views).";
        break;
      }
      UINT support = 0;
      m_pDevice->GetReal()->CheckFormatSupport(sf, &support);
      if(!(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
      {
        error = "Input colour format does not support filtering.";
        break;
      }
      m_pDevice->GetReal()->CheckFormatSupport(df, &support);
      if(!(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
      {
        error = "Output colour format does not support rendering.";
        break;
      }
      OverrideColour c;
      c.rule = uint32_t(i);
      c.mapping = mapping;
      c.mapping.sourceFormat = uint32_t(sf);
      c.mapping.destinationFormat = uint32_t(df);
      sd.Width = RDCMAX(1U, sd.Width >> mapping.sourceMip);
      sd.Height = RDCMAX(1U, sd.Height >> mapping.sourceMip);
      sd.MipLevels = sd.ArraySize = 1;
      sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      sd.Usage = D3D11_USAGE_DEFAULT;
      sd.CPUAccessFlags = sd.MiscFlags = 0;
      dd.Width = RDCMAX(1U, dd.Width >> mapping.destinationMip);
      dd.Height = RDCMAX(1U, dd.Height >> mapping.destinationMip);
      dd.MipLevels = dd.ArraySize = 1;
      dd.Format = df;
      dd.BindFlags = D3D11_BIND_RENDER_TARGET;
      dd.Usage = D3D11_USAGE_DEFAULT;
      dd.CPUAccessFlags = dd.MiscFlags = 0;
      c.directCopy = sd.Width == dd.Width && sd.Height == dd.Height && sf == df;
      HRESULT hr = m_pDevice->GetReal()->CreateTexture2D(&sd, NULL, &c.snapshot);
      D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = sf;
      srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = 1;
      if(SUCCEEDED(hr))
        hr = m_pDevice->GetReal()->CreateShaderResourceView(c.snapshot, &srv, &c.srv);
      if(SUCCEEDED(hr) && !c.directCopy)
        hr = m_pDevice->GetReal()->CreateTexture2D(&dd, NULL, &c.target);
      if(SUCCEEDED(hr) && !c.directCopy)
        hr = m_pDevice->GetReal()->CreateRenderTargetView(c.target, NULL, &c.rtv);
      colours.push_back(c);
      if(FAILED(hr))
      {
        error = "Unable to allocate colour bypass resources: " + ToStr(hr);
        break;
      }
    }
  }
  if(error.empty() && !colours.empty())
    error = CreateOverrideBlit();
  if(!error.empty())
  {
    ReleaseOverrideColours(colours);
    return error;
  }
  ReleaseOverrideColours(m_OverrideColours);
  m_OverrideColours.swap(colours);
  m_PassOverrides = overrides;
  m_ReplayOverrideError.clear();
  return {};
}

bool D3D11Replay::IsReplayEventSkipped(uint32_t eventId) const
{
  for(const ReplayPassOverride &o : m_PassOverrides)
    if(eventId >= o.firstEvent && eventId <= o.lastEvent)
      return true;
  return false;
}

void D3D11Replay::ResetReplayOverrides()
{
  RestoreOverrideConstants();
  m_ReplayOverrideError.clear();
  for(OverrideColour &c : m_OverrideColours)
    c.ready = c.done = false;
}

void D3D11Replay::BeginReplayOverrideEvent(uint32_t eventId, D3D11Chunk chunk)
{
  ID3D11DeviceContext *ctx = m_pImmediateContext->GetReal();
  for(OverrideColour &c : m_OverrideColours)
  {
    const ReplayPassOverride &rule = m_PassOverrides[c.rule];
    if(c.ready || c.done || eventId < rule.firstEvent || eventId > rule.lastEvent)
      continue;
    OverridePredicate pred(ctx);
    ID3D11Texture2D *src = OverrideTexture(c.mapping.source);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);
    ctx->CopySubresourceRegion(c.snapshot, 0, 0, 0, 0, src,
                               c.mapping.sourceMip + c.mapping.sourceSlice * desc.MipLevels, NULL);
    c.ready = true;
  }
  bool compute = chunk == D3D11Chunk::Dispatch || chunk == D3D11Chunk::DispatchIndirect;
  if((!compute && !OverrideDraw(chunk)) || IsReplayEventSkipped(eventId) ||
     m_ConstantOverrides.empty())
    return;
  OverridePredicate pred(ctx);
  D3D11RenderState *state = m_pImmediateContext->GetCurrentPipelineState();
  D3D11RenderState::Shader *stages[] = {&state->VS, &state->HS, &state->DS,
                                        &state->GS, &state->PS, &state->CS};
  for(uint32_t s = 0; s < 6; s++)
  {
    if(compute != (s == 5))
      continue;
    const ResourceId shader = GetIDForDeviceChild(stages[s]->Object);
    auto stageRules = m_OverrideIndex.find(std::make_pair(ShaderStage(s), shader));
    if(!m_OverrideLegacy && stageRules == m_OverrideIndex.end())
      continue;
    for(UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; slot++)
    {
      const rdcarray<uint32_t> *indices = &m_AllOverrideIndices;
      if(!m_OverrideLegacy)
      {
        auto slotRules = stageRules->second.find(slot);
        if(slotRules == stageRules->second.end())
          continue;
        indices = &slotRules->second;
      }
      OverrideConstants *constants = NULL;
      UINT copyStart = 0, copySize = 0;
      for(uint32_t index : *indices)
      {
        const ReplayConstantOverride &o = m_ConstantOverrides[index];
        m_OverrideComparisons++;
        if(uint32_t(o.stage) != s || o.slot != slot || eventId < o.firstEvent ||
           eventId > o.lastEvent || shader != o.shader)
          continue;
        ID3D11Buffer *wrapped = stages[s]->ConstantBuffers[slot];
        D3D11_BUFFER_DESC desc = {};
        if(wrapped)
          wrapped->GetDesc(&desc);
        uint64_t offset = uint64_t(stages[s]->CBOffsets[slot]) * 16 + o.byteOffset;
        if(!wrapped || offset + o.data.size() > desc.ByteWidth ||
           uint64_t(o.byteOffset) + o.data.size() > uint64_t(stages[s]->CBCounts[slot]) * 16)
        {
          m_ReplayOverrideError = StringFormat::Fmt(
              "Constant override outside bound range at event %u, stage %u, slot %u.", eventId, s,
              slot);
          continue;
        }
        if(!constants)
        {
          const uint64_t firstByte = uint64_t(stages[s]->CBOffsets[slot]) * 16;
          const uint64_t visibleBytes =
              RDCMIN(uint64_t(desc.ByteWidth) - firstByte, uint64_t(stages[s]->CBCounts[slot]) * 16);
          // A compact buffer exposes the same intersection and the same out-of-bounds zeros.
          // Each event recopies its source: uploads/command lists may have changed it.
          const bool compact = !m_OverrideLegacy && visibleBytes > 0 && visibleBytes <= 65536 &&
                               (visibleBytes % 16) == 0 &&
                               (m_pImmediateContext->GetReal1() || firstByte == 0);
          copyStart = compact ? UINT(firstByte) : 0;
          copySize = compact ? UINT(visibleBytes) : desc.ByteWidth;
          for(OverrideConstants &existing : m_OverrideConstants)
            if(existing.stage == o.stage && existing.slot == slot)
              constants = &existing;
          if(!constants)
          {
            OverrideConstants entry;
            entry.stage = o.stage;
            entry.slot = slot;
            m_OverrideConstants.push_back(entry);
            constants = &m_OverrideConstants.back();
          }
          D3D11_BUFFER_DESC old = {};
          if(constants->patched)
            constants->patched->GetDesc(&old);
          if(old.ByteWidth != copySize)
          {
            SAFE_RELEASE(constants->scratch);
            SAFE_RELEASE(constants->patched);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = desc.MiscFlags = 0;
            desc.StructureByteStride = 0;
            desc.ByteWidth = copySize;
            desc.BindFlags = 0;
            HRESULT hr = m_pDevice->GetReal()->CreateBuffer(&desc, NULL, &constants->scratch);
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if(SUCCEEDED(hr))
              hr = m_pDevice->GetReal()->CreateBuffer(&desc, NULL, &constants->patched);
            if(FAILED(hr))
            {
              SAFE_RELEASE(constants->scratch);
              SAFE_RELEASE(constants->patched);
              m_ReplayOverrideError = "Cannot allocate temporary constant buffers: " + ToStr(hr);
              constants = NULL;
              break;
            }
            m_OverrideAllocations += 2;
          }
          constants->original = UNWRAP(WrappedID3D11Buffer, wrapped);
          constants->offset = stages[s]->CBOffsets[slot];
          constants->count = stages[s]->CBCounts[slot];
          if(compact)
          {
            D3D11_BOX source = {copyStart, 0, 0, copyStart + copySize, 1, 1};
            ctx->CopySubresourceRegion(constants->scratch, 0, 0, 0, 0, constants->original, 0,
                                       &source);
          }
          else
            ctx->CopyResource(constants->scratch, constants->original);
          m_OverrideCopies++;
          m_OverrideCopyBytes += copySize;
        }
        D3D11_BOX box = {
            UINT(offset - copyStart), 0, 0, UINT(offset - copyStart + o.data.size()), 1, 1};
        ctx->UpdateSubresource(constants->scratch, 0, &box, o.data.data(), 0, 0);
      }
      if(constants)
      {
        ctx->CopyResource(constants->patched, constants->scratch);
        m_OverrideCopies++;
        m_OverrideCopyBytes += copySize;
        BindOverrideCB(m_pImmediateContext, constants->stage, slot, constants->patched,
                       constants->offset - copyStart / 16, constants->count);
      }
    }
  }
}

void D3D11Replay::RestoreOverrideConstants()
{
  for(OverrideConstants &c : m_OverrideConstants)
  {
    if(c.original)
      BindOverrideCB(m_pImmediateContext, c.stage, c.slot, c.original, c.offset, c.count);
    c.original = NULL;
  }
}

void D3D11Replay::EndReplayOverrideEvent(uint32_t eventId)
{
  RestoreOverrideConstants();
  ID3D11DeviceContext *ctx = m_pImmediateContext->GetReal();
  for(OverrideColour &c : m_OverrideColours)
  {
    if(!c.ready || c.done || eventId < m_PassOverrides[c.rule].lastEvent)
      continue;
    D3D11RenderStateTracker tracker(m_pImmediateContext);
    ctx->ClearState();
    ID3D11Texture2D *dst = OverrideTexture(c.mapping.destination);
    D3D11_TEXTURE2D_DESC dd;
    dst->GetDesc(&dd);
    ID3D11Texture2D *result = c.snapshot;
    if(!c.directCopy)
    {
      D3D11_TEXTURE2D_DESC td;
      c.target->GetDesc(&td);
      D3D11_VIEWPORT vp = {0, 0, float(td.Width), float(td.Height), 0, 1};
      ctx->RSSetViewports(1, &vp);
      ctx->RSSetState(m_OverrideRaster);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(m_OverrideVS, NULL, 0);
      ctx->PSSetShader(m_OverridePS, NULL, 0);
      ctx->PSSetShaderResources(0, 1, &c.srv);
      ctx->PSSetSamplers(0, 1, &m_OverrideSampler);
      ctx->OMSetRenderTargets(1, &c.rtv, NULL);
      ctx->Draw(3, 0);
      ctx->ClearState();
      result = c.target;
    }
    ctx->CopySubresourceRegion(dst,
                               c.mapping.destinationMip + c.mapping.destinationSlice * dd.MipLevels,
                               0, 0, 0, result, 0, NULL);
    c.done = true;
  }
}

void D3D11Replay::ReleaseReplayOverrides()
{
  RestoreOverrideConstants();
  for(OverrideConstants &c : m_OverrideConstants)
  {
    SAFE_RELEASE(c.scratch);
    SAFE_RELEASE(c.patched);
  }
  m_OverrideConstants.clear();
  ReleaseOverrideColours(m_OverrideColours);
  SAFE_RELEASE(m_OverrideVS);
  SAFE_RELEASE(m_OverridePS);
  SAFE_RELEASE(m_OverrideSampler);
  SAFE_RELEASE(m_OverrideRaster);
}
