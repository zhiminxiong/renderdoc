/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2024-2025 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include "d3d12_dxil_debug.h"
#include "data/hlsl/hlsl_cbuffers.h"
#include "driver/dxgi/dxgi_common.h"
#include "d3d12_command_queue.h"
#include "d3d12_debug.h"
#include "d3d12_replay.h"
#include "d3d12_resources.h"
#include "d3d12_state.h"

using namespace DXIL;
using namespace DXILDebug;

namespace DXILDebug
{
static DXBC::ResourceRetType ConvertCompTypeToResourceRetType(const CompType compType)
{
  switch(compType)
  {
    case CompType::Float: return DXBC::ResourceRetType::RETURN_TYPE_FLOAT;
    case CompType::UNormSRGB:
    case CompType::UNorm: return DXBC::ResourceRetType::RETURN_TYPE_UNORM;
    case CompType::SNorm: return DXBC::ResourceRetType::RETURN_TYPE_SNORM;
    case CompType::UInt: return DXBC::ResourceRetType::RETURN_TYPE_UINT;
    case CompType::SInt: return DXBC::ResourceRetType::RETURN_TYPE_SINT;
    case CompType::Typeless:
    case CompType::UScaled:
    case CompType::SScaled:
    case CompType::Depth:
    default:
      RDCERR("Unexpected component type %s", ToStr(compType).c_str());
      return DXBC::ResourceRetType ::RETURN_TYPE_UNKNOWN;
  }
}

static DXBCBytecode::ResourceDimension ConvertSRVResourceDimensionToResourceDimension(
    D3D12_SRV_DIMENSION dim)
{
  switch(dim)
  {
    case D3D12_SRV_DIMENSION_UNKNOWN:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_UNKNOWN;
    case D3D12_SRV_DIMENSION_BUFFER:
      return DXBCBytecode::ResourceDimension ::RESOURCE_DIMENSION_BUFFER;
    case D3D12_SRV_DIMENSION_TEXTURE1D:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE1D;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE1DARRAY;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE2D;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE2DARRAY;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE2DMS;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE2DMSARRAY;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURE3D;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURECUBE;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_TEXTURECUBEARRAY;
    default:
      RDCERR("Unexpected SRV dimension %s", ToStr(dim).c_str());
      return DXBCBytecode::ResourceDimension::RESOURCE_DIMENSION_UNKNOWN;
  }
}

static DXDebug::SamplerMode ConvertSamplerFilterToSamplerMode(D3D12_FILTER filter)
{
  switch(filter)
  {
    case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_MAG_ANISOTROPIC_MIP_POINT:
    case D3D12_FILTER_COMPARISON_ANISOTROPIC:
      return DXBCBytecode::SamplerMode::SAMPLER_MODE_COMPARISON;
      break;
    default: break;
  }
  return DXBCBytecode::SamplerMode::SAMPLER_MODE_DEFAULT;
}

static bool IsShaderParameterVisible(DXBC::ShaderType shaderType,
                                     D3D12_SHADER_VISIBILITY shaderVisibility)
{
  if(shaderVisibility == D3D12_SHADER_VISIBILITY_ALL)
    return true;

  if(shaderType == DXBC::ShaderType::Vertex && shaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX)
    return true;

  if(shaderType == DXBC::ShaderType::Pixel && shaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL)
    return true;

  if(shaderType == DXBC::ShaderType::Amplification &&
     shaderVisibility == D3D12_SHADER_VISIBILITY_AMPLIFICATION)
    return true;

  if(shaderType == DXBC::ShaderType::Mesh && shaderVisibility == D3D12_SHADER_VISIBILITY_MESH)
    return true;

  return false;
}

static void FillViewFmtFromResourceFormat(DXGI_FORMAT format, GlobalState::ViewFmt &viewFmt)
{
  RDCASSERT(format != DXGI_FORMAT_UNKNOWN);
  ResourceFormat fmt = MakeResourceFormat(format);

  viewFmt.byteWidth = fmt.compByteWidth;
  viewFmt.numComps = fmt.compCount;
  viewFmt.compType = fmt.compType;

  if(format == DXGI_FORMAT_R11G11B10_FLOAT)
    viewFmt.byteWidth = 11;
  else if(format == DXGI_FORMAT_R10G10B10A2_UINT || format == DXGI_FORMAT_R10G10B10A2_UNORM)
    viewFmt.byteWidth = 10;

  if(viewFmt.byteWidth == 10 || viewFmt.byteWidth == 11)
    viewFmt.stride = 4;    // 10 10 10 2 or 11 11 10
  else
    viewFmt.stride = viewFmt.byteWidth * viewFmt.numComps;
}

// Only valid/used fo root descriptors
// Root descriptors only have RawBuffer and StructuredBuffer types
static uint32_t GetUAVBufferStrideFromShaderMetadata(const DXIL::EntryPointInterface *reflection,
                                                     const BindingSlot &slot)
{
  for(const DXIL::EntryPointInterface::ResourceBase &bind : reflection->uavs)
  {
    if(bind.MatchesBinding(slot.shaderRegister, slot.shaderRegister, slot.registerSpace))
    {
      if(bind.uavData.shape == ResourceKind::RawBuffer ||
         bind.uavData.shape == ResourceKind::StructuredBuffer)
      {
        return bind.uavData.elementStride;
      }
    }
  }
  return 0;
}

static uint32_t GetSRVBufferStrideFromShaderMetadata(const DXIL::EntryPointInterface *reflection,
                                                     const BindingSlot &slot)
{
  for(const DXIL::EntryPointInterface::ResourceBase &bind : reflection->srvs)
  {
    if(bind.MatchesBinding(slot.shaderRegister, slot.shaderRegister, slot.registerSpace))
    {
      if(bind.srvData.shape == ResourceKind::RawBuffer ||
         bind.srvData.shape == ResourceKind::StructuredBuffer)
      {
        return bind.srvData.elementStride;
      }
    }
  }
  return 0;
}

static void FlattenSingleVariable(const rdcstr &cbufferName, uint32_t byteOffset,
                                  const rdcstr &basename, const ShaderVariable &v,
                                  rdcarray<ShaderVariable> &outvars,
                                  rdcarray<SourceVariableMapping> &sourcevars)
{
  size_t outIdx = byteOffset / 16;
  size_t outComp = (byteOffset % 16) / 4;

  if(v.RowMajor())
    outvars.resize(RDCMAX(outIdx + v.rows, outvars.size()));
  else
    outvars.resize(RDCMAX(outIdx + v.columns, outvars.size()));

  if(outvars[outIdx].columns > 0)
  {
    // if we already have a variable in this slot, just copy the data for this variable and add
    // the source mapping. We should not overlap into the next register as that's not allowed.
    memcpy(&outvars[outIdx].value.u32v[outComp], &v.value.u32v[0], sizeof(uint32_t) * v.columns);
    uint32_t oldColumns = outvars[outIdx].columns;
    uint32_t newColumns = (uint32_t)(outComp + v.columns);
    uint32_t numColumns = RDCMAX(oldColumns, newColumns);
    numColumns = RDCMIN(4U, numColumns);
    outvars[outIdx].columns = (uint8_t)numColumns;

    SourceVariableMapping mapping;
    mapping.name = basename;
    mapping.type = v.type;
    mapping.rows = v.rows;
    mapping.columns = v.columns;
    mapping.offset = byteOffset;
    mapping.variables.resize(v.columns);

    for(int i = 0; i < v.columns; i++)
    {
      mapping.variables[i].type = DebugVariableType::Constant;
      mapping.variables[i].name = StringFormat::Fmt("%s[%u]", cbufferName.c_str(), outIdx);
      mapping.variables[i].component = uint16_t(outComp + i);
    }

    sourcevars.push_back(mapping);
  }
  else
  {
    const uint32_t numRegisters = v.RowMajor() ? v.rows : v.columns;
    for(uint32_t reg = 0; reg < numRegisters; reg++)
    {
      outvars[outIdx + reg].rows = 1;
      outvars[outIdx + reg].type = v.type;
      outvars[outIdx + reg].columns = v.columns;
      outvars[outIdx + reg].flags = v.flags;
    }

    if(v.RowMajor())
    {
      for(size_t ri = 0; ri < v.rows; ri++)
        memcpy(&outvars[outIdx + ri].value.u32v[0], &v.value.u32v[ri * v.columns],
               sizeof(uint32_t) * v.columns);
    }
    else
    {
      // if we have a matrix stored in column major order, we need to transpose it back so we can
      // unroll it into vectors.
      for(size_t ci = 0; ci < v.columns; ci++)
        for(size_t ri = 0; ri < v.rows; ri++)
          outvars[outIdx + ci].value.u32v[ri] = v.value.u32v[ri * v.columns + ci];
    }

    SourceVariableMapping mapping;
    mapping.name = basename;
    mapping.type = v.type;
    mapping.rows = v.rows;
    mapping.columns = v.columns;
    mapping.offset = byteOffset;
    mapping.variables.resize(v.rows * v.columns);

    RDCASSERT(outComp == 0 || v.rows == 1, outComp, v.rows);

    size_t i = 0;
    for(uint8_t r = 0; r < v.rows; r++)
    {
      for(uint8_t c = 0; c < v.columns; c++)
      {
        size_t regIndex = outIdx + (v.RowMajor() ? r : c);
        size_t compIndex = outComp + (v.RowMajor() ? c : r);

        mapping.variables[i].type = DebugVariableType::Constant;
        mapping.variables[i].name = StringFormat::Fmt("%s[%zu]", cbufferName.c_str(), regIndex);
        mapping.variables[i].component = uint16_t(compIndex);
        i++;
      }
    }

    sourcevars.push_back(mapping);
  }
}

static void FlattenVariables(const rdcstr &cbufferName, const rdcarray<ShaderConstant> &constants,
                             const rdcarray<ShaderVariable> &invars,
                             rdcarray<ShaderVariable> &outvars, const rdcstr &prefix,
                             uint32_t baseOffset, rdcarray<SourceVariableMapping> &sourceVars)
{
  RDCASSERTEQUAL(constants.size(), invars.size());

  for(size_t i = 0; i < constants.size(); i++)
  {
    const ShaderConstant &c = constants[i];
    const ShaderVariable &v = invars[i];

    uint32_t byteOffset = baseOffset + c.byteOffset;

    rdcstr basename = prefix + rdcstr(v.name);

    if(v.type == VarType::Struct)
    {
      // check if this is an array of structs or not
      if(c.type.elements == 1)
      {
        FlattenVariables(cbufferName, c.type.members, v.members, outvars, basename + ".",
                         byteOffset, sourceVars);
      }
      else
      {
        for(int m = 0; m < v.members.count(); m++)
        {
          FlattenVariables(cbufferName, c.type.members, v.members[m].members, outvars,
                           StringFormat::Fmt("%s[%zu].", basename.c_str(), m),
                           byteOffset + m * c.type.arrayByteStride, sourceVars);
        }
      }
    }
    else if(c.type.elements > 1 || (v.rows == 0 && v.columns == 0) || !v.members.empty())
    {
      for(int m = 0; m < v.members.count(); m++)
      {
        FlattenSingleVariable(cbufferName, byteOffset + m * c.type.arrayByteStride,
                              StringFormat::Fmt("%s[%zu]", basename.c_str(), m), v.members[m],
                              outvars, sourceVars);
      }
    }
    else
    {
      FlattenSingleVariable(cbufferName, byteOffset, basename, v, outvars, sourceVars);
    }
  }
}
static void AddCBufferToGlobalState(const DXIL::Program *program, GlobalState &global,
                                    rdcarray<SourceVariableMapping> &sourceVars,
                                    const ShaderReflection &refl, const BindingSlot &slot,
                                    bytebuf &cbufData)
{
  // Find the identifier
  size_t numCBs = refl.constantBlocks.size();
  for(size_t i = 0; i < numCBs; ++i)
  {
    const ConstantBlock &cb = refl.constantBlocks[i];
    if(slot.registerSpace == (uint32_t)cb.fixedBindSetOrSpace &&
       slot.shaderRegister >= (uint32_t)cb.fixedBindNumber &&
       slot.shaderRegister < (uint32_t)(cb.fixedBindNumber + cb.bindArraySize))
    {
      uint32_t arrayIndex = slot.shaderRegister - cb.fixedBindNumber;

      rdcarray<ShaderVariable> &targetVars =
          cb.bindArraySize > 1 ? global.constantBlocks[i].members[arrayIndex].members
                               : global.constantBlocks[i].members;
      RDCASSERTMSG("Reassigning previously filled cbuffer", targetVars.empty());

      ConstantBlockReference constantBlockRef = {i, arrayIndex};
      global.constantBlocksDatas[constantBlockRef] = cbufData;
      rdcstr resName = Debugger::GetResourceReferenceName(program, ResourceClass::CBuffer, slot);
      global.constantBlocks[i].name = resName;

      SourceVariableMapping cbSourceMapping;
      cbSourceMapping.name = refl.constantBlocks[i].name;
      cbSourceMapping.variables.push_back(
          DebugVariableReference(DebugVariableType::Constant, global.constantBlocks[i].name));
      sourceVars.push_back(cbSourceMapping);

      rdcstr identifierPrefix = global.constantBlocks[i].name;
      rdcstr variablePrefix = refl.constantBlocks[i].name;
      if(cb.bindArraySize > 1)
      {
        identifierPrefix =
            StringFormat::Fmt("%s[%u]", global.constantBlocks[i].name.c_str(), arrayIndex);
        variablePrefix = StringFormat::Fmt("%s[%u]", refl.constantBlocks[i].name.c_str(), arrayIndex);

        // The above sourceVar is for the logical identifier, and FlattenVariables adds the
        // individual elements of the constant buffer. For CB arrays, add an extra source
        // var for the CB array index
        SourceVariableMapping cbArrayMapping;
        global.constantBlocks[i].members[arrayIndex].name = StringFormat::Fmt("[%u]", arrayIndex);
        cbArrayMapping.name = variablePrefix;
        cbArrayMapping.variables.push_back(
            DebugVariableReference(DebugVariableType::Constant, identifierPrefix));
        sourceVars.push_back(cbArrayMapping);
      }
      const rdcarray<ShaderConstant> &constants =
          (cb.bindArraySize > 1) ? refl.constantBlocks[i].variables[0].type.members
                                 : refl.constantBlocks[i].variables;

      rdcarray<ShaderVariable> vars;
      StandardFillCBufferVariables(refl.resourceId, constants, vars, cbufData);
      FlattenVariables(identifierPrefix, constants, vars, targetVars, variablePrefix + ".", 0,
                       sourceVars);
      for(size_t c = 0; c < targetVars.size(); c++)
        targetVars[c].name = StringFormat::Fmt("[%u]", (uint32_t)c);

      return;
    }
  }
}

void FetchConstantBufferData(WrappedID3D12Device *device, const DXIL::Program *program,
                             const D3D12RenderState::RootSignature &rootsig,
                             const ShaderReflection &refl, GlobalState &global,
                             rdcarray<SourceVariableMapping> &sourceVars)
{
  WrappedID3D12RootSignature *pD3D12RootSig =
      device->GetResourceManager()->GetCurrentAs<WrappedID3D12RootSignature>(rootsig.rootsig);
  const DXBC::ShaderType shaderType = program->GetShaderType();

  size_t numParams = RDCMIN(pD3D12RootSig->sig.Parameters.size(), rootsig.sigelems.size());
  for(size_t i = 0; i < numParams; i++)
  {
    const D3D12RootSignatureParameter &rootSigParam = pD3D12RootSig->sig.Parameters[i];
    const D3D12RenderState::SignatureElement &element = rootsig.sigelems[i];
    if(IsShaderParameterVisible(shaderType, rootSigParam.ShaderVisibility))
    {
      if(rootSigParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
         element.type == eRootConst)
      {
        BindingSlot slot(rootSigParam.Constants.ShaderRegister, rootSigParam.Constants.RegisterSpace);
        UINT sizeBytes = sizeof(uint32_t) * RDCMIN(rootSigParam.Constants.Num32BitValues,
                                                   (UINT)element.constants.size());
        bytebuf cbufData((const byte *)element.constants.data(), sizeBytes);
        AddCBufferToGlobalState(program, global, sourceVars, refl, slot, cbufData);
      }
      else if(rootSigParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && element.type == eRootCBV)
      {
        BindingSlot slot(rootSigParam.Descriptor.ShaderRegister,
                         rootSigParam.Descriptor.RegisterSpace);
        ID3D12Resource *cbv = device->GetResourceManager()->GetCurrentAs<ID3D12Resource>(element.id);
        bytebuf cbufData;
        device->GetDebugManager()->GetBufferData(cbv, element.offset, 0, cbufData);
        AddCBufferToGlobalState(program, global, sourceVars, refl, slot, cbufData);
      }
      else if(rootSigParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
              element.type == eRootTable)
      {
        UINT prevTableOffset = 0;
        WrappedID3D12DescriptorHeap *heap =
            device->GetResourceManager()->GetCurrentAs<WrappedID3D12DescriptorHeap>(element.id);

        size_t numRanges = rootSigParam.ranges.size();
        for(size_t r = 0; r < numRanges; r++)
        {
          // For this traversal we only care about CBV descriptor ranges, but we still need to
          // calculate the table offsets in case a descriptor table has a combination of
          // different range types
          const D3D12_DESCRIPTOR_RANGE1 &range = rootSigParam.ranges[r];

          UINT offset = range.OffsetInDescriptorsFromTableStart;
          if(range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
            offset = prevTableOffset;

          D3D12Descriptor *desc = (D3D12Descriptor *)heap->GetCPUDescriptorHandleForHeapStart().ptr;
          desc += element.offset;
          desc += offset;

          UINT numDescriptors = range.NumDescriptors;
          if(numDescriptors == UINT_MAX)
          {
            // Find out how many descriptors are left after
            numDescriptors = heap->GetNumDescriptors() - offset - (UINT)element.offset;

            // TODO: Look up the bind point in the D3D12 state to try to get
            // a better guess at the number of descriptors
          }

          prevTableOffset = offset + numDescriptors;

          if(range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
            continue;

          BindingSlot slot(range.BaseShaderRegister, range.RegisterSpace);

          bytebuf cbufData;
          for(UINT n = 0; n < numDescriptors; ++n, ++slot.shaderRegister)
          {
            const D3D12_CONSTANT_BUFFER_VIEW_DESC &cbv = desc->GetCBV();
            ResourceId resId;
            uint64_t byteOffset = 0;
            WrappedID3D12Resource::GetResIDFromAddr(cbv.BufferLocation, resId, byteOffset);
            ID3D12Resource *pCbvResource =
                device->GetResourceManager()->GetCurrentAs<ID3D12Resource>(resId);
            cbufData.clear();

            if(cbv.SizeInBytes > 0)
              device->GetDebugManager()->GetBufferData(pCbvResource, byteOffset, cbv.SizeInBytes,
                                                       cbufData);
            AddCBufferToGlobalState(program, global, sourceVars, refl, slot, cbufData);

            desc++;
          }
        }
      }
    }
  }
}

InterpolationMode GetInterpolationModeForInputParam(const SigParameter &sig,
                                                    const rdcarray<SigParameter> &stageInputSig,
                                                    const DXIL::Program *program)
{
  if(sig.varType == VarType::SInt || sig.varType == VarType::UInt)
    return InterpolationMode::INTERPOLATION_CONSTANT;

  if((sig.varType == VarType::Float) || (sig.varType == VarType::Half))
  {
    // if we're packed with a different type on either side, we must be nointerpolation
    size_t numInputs = stageInputSig.size();
    for(size_t j = 0; j < numInputs; j++)
    {
      if(sig.regIndex == stageInputSig[j].regIndex && (stageInputSig[j].varType != sig.varType))
        return DXBC::InterpolationMode::INTERPOLATION_CONSTANT;
    }

    if(!program)
    {
      RDCERR("No DXIL program");
      return DXBC::InterpolationMode::INTERPOLATION_UNDEFINED;
    }
    // Search the DXIL shader meta data to get the interpolation mode
    const DXIL::EntryPointInterface *entryPoint = program->GetEntryPointInterface();
    if(!entryPoint)
    {
      RDCERR("No entry point interface found in DXIL program");
      return DXBC::InterpolationMode::INTERPOLATION_UNDEFINED;
    }
    for(size_t j = 0; j < entryPoint->inputs.size(); ++j)
    {
      const EntryPointInterface::Signature &dxilParam = entryPoint->inputs[j];
      int row = sig.regIndex;
      if((dxilParam.startRow <= row) && (row < (int)(dxilParam.startRow + dxilParam.rows)))
      {
        const int firstElem = sig.regChannelMask & 0x1   ? 0
                              : sig.regChannelMask & 0x2 ? 1
                              : sig.regChannelMask & 0x4 ? 2
                              : sig.regChannelMask & 0x8 ? 3
                                                         : -1;
        if(dxilParam.startCol == firstElem)
        {
          if(sig.semanticName == dxilParam.name)
          {
            return (InterpolationMode)dxilParam.interpolation;
          }
        }
      }
    }
    return DXBC::InterpolationMode::INTERPOLATION_UNDEFINED;
  }

  RDCERR("Unexpected input signature type: %s", ToStr(sig.varType).c_str());
  return InterpolationMode::INTERPOLATION_UNDEFINED;
}

void GetInterpolationModeForInputParams(const rdcarray<SigParameter> &inputSig,
                                        const DXIL::Program *program,
                                        rdcarray<DXBC::InterpolationMode> &interpModes)
{
  size_t numInputs = inputSig.size();
  interpModes.resize(numInputs);
  for(size_t i = 0; i < numInputs; i++)
  {
    const SigParameter &sig = inputSig[i];
    interpModes[i] = GetInterpolationModeForInputParam(sig, inputSig, program);
  }
}

D3D12APIWrapper::D3D12APIWrapper(WrappedID3D12Device *device, const DXIL::Program &dxilProgram,
                                 GlobalState &globalState, uint32_t eventId)
    : m_Device(device),
      m_EntryPointInterface(dxilProgram.GetEntryPointInterface()),
      m_GlobalState(globalState),
      m_ShaderType(dxilProgram.GetShaderType()),
      m_EventId(eventId)
{
}

D3D12APIWrapper::~D3D12APIWrapper()
{
  // if we replayed to before the action for fetching some UAVs
  // replay back to after the action to keep the state consistent.
  if(m_DidReplay)
  {
    D3D12MarkerRegion region(m_Device->GetQueue()->GetReal(), "ResetReplay");
    // replay the action to get back to 'normal' state for this event
    m_Device->ReplayLog(0, m_EventId, eReplay_OnlyDraw);
  }
}

void D3D12APIWrapper::FetchSRV(const D3D12Descriptor *resDescriptor, const BindingSlot &slot)
{
  if(resDescriptor)
  {
    D3D12ResourceManager *rm = m_Device->GetResourceManager();
    DXILDebug::GlobalState::SRVData &srvData = m_GlobalState.srvs[slot];
    ResourceId srvId = resDescriptor->GetResResourceId();
    ID3D12Resource *pResource = rm->GetCurrentAs<ID3D12Resource>(srvId);
    if(pResource)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = resDescriptor->GetSRV();
      if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_UNKNOWN)
        srvDesc = MakeSRVDesc(pResource->GetDesc());

      if(srvDesc.Format != DXGI_FORMAT_UNKNOWN)
      {
        DXILDebug::FillViewFmtFromResourceFormat(srvDesc.Format, srvData.resInfo.format);
      }
      else
      {
        D3D12_RESOURCE_DESC resDesc = pResource->GetDesc();
        if(resDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
          srvData.resInfo.format.stride = srvDesc.Buffer.StructureByteStride;
      }

      if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
      {
        srvData.resInfo.firstElement = (uint32_t)srvDesc.Buffer.FirstElement;
        srvData.resInfo.numElements = srvDesc.Buffer.NumElements;
        srvData.resInfo.isByteBuffer =
            ((srvDesc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0) ? true : false;
        // Get the buffer stride from the shader metadata (for StructuredBuffer, RawBuffer)
        uint32_t mdStride =
            DXILDebug::GetSRVBufferStrideFromShaderMetadata(m_EntryPointInterface, slot);
        if(mdStride != 0)
          srvData.resInfo.format.stride = mdStride;

        m_Device->GetDebugManager()->GetBufferData(pResource, 0, 0, srvData.data);
      }
      // Textures are sampled via a pixel shader, so there's no need to copy their data
    }
  }
}

void D3D12APIWrapper::FetchSRV(const BindingSlot &slot)
{
  // Direct access resource
  if(slot.heapType != DXDebug::HeapDescriptorType::NoHeap)
  {
    const HeapDescriptorType heapType = slot.heapType;
    const uint32_t descriptorIndex = slot.descriptorIndex;
    const D3D12Descriptor srvDesc =
        D3D12ShaderDebug::FindDescriptor(m_Device, heapType, descriptorIndex);
    return FetchSRV(&srvDesc, slot);
  }

  const D3D12RenderState &rs = m_Device->GetQueue()->GetCommandData()->m_RenderState;
  D3D12ResourceManager *rm = m_Device->GetResourceManager();

  // Get the root signature
  const D3D12RenderState::RootSignature *pRootSignature = NULL;
  if(m_ShaderType == DXBC::ShaderType::Compute)
  {
    if(rs.compute.rootsig != ResourceId())
    {
      pRootSignature = &rs.compute;
    }
  }
  else if(rs.graphics.rootsig != ResourceId())
  {
    pRootSignature = &rs.graphics;
  }

  if(pRootSignature)
  {
    WrappedID3D12RootSignature *pD3D12RootSig =
        rm->GetCurrentAs<WrappedID3D12RootSignature>(pRootSignature->rootsig);

    size_t numParams = RDCMIN(pD3D12RootSig->sig.Parameters.size(), pRootSignature->sigelems.size());
    for(size_t i = 0; i < numParams; ++i)
    {
      const D3D12RootSignatureParameter &param = pD3D12RootSig->sig.Parameters[i];
      const D3D12RenderState::SignatureElement &element = pRootSignature->sigelems[i];
      if(IsShaderParameterVisible(m_ShaderType, param.ShaderVisibility))
      {
        if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV && element.type == eRootSRV)
        {
          if(param.Descriptor.ShaderRegister == slot.shaderRegister &&
             param.Descriptor.RegisterSpace == slot.registerSpace)
          {
            // Found the requested SRV
            ID3D12Resource *pResource = rm->GetCurrentAs<ID3D12Resource>(element.id);
            if(pResource)
            {
              DXILDebug::GlobalState::SRVData &srvData = m_GlobalState.srvs[slot];
              D3D12_RESOURCE_DESC resDesc = pResource->GetDesc();

              // DXC allows root buffers to have a stride of up to 16 bytes in the shader, which
              // means encoding the byte offset into the first element here is wrong without
              // knowing what the actual accessed stride is. Instead we only fetch the data from
              // that offset onwards.

              // Root buffers are typeless, try with the resource desc format
              // The debugger code will handle DXGI_FORMAT_UNKNOWN
              if(resDesc.Format != DXGI_FORMAT_UNKNOWN)
                DXILDebug::FillViewFmtFromResourceFormat(resDesc.Format, srvData.resInfo.format);

              srvData.resInfo.isRootDescriptor = true;
              srvData.resInfo.firstElement = 0;
              // root arguments have no bounds checking, so use the most conservative number of elements
              srvData.resInfo.numElements = uint32_t(resDesc.Width - element.offset);

              if(resDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
              {
                // Get the buffer stride from the shader metadata (for StructuredBuffer, RawBuffer)
                uint32_t mdStride =
                    DXILDebug::GetSRVBufferStrideFromShaderMetadata(m_EntryPointInterface, slot);
                if(mdStride != 0)
                  srvData.resInfo.format.stride = mdStride;
                m_Device->GetDebugManager()->GetBufferData(pResource, element.offset, 0,
                                                           srvData.data);
              }
            }
            return;
          }
        }
        else if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                element.type == eRootTable)
        {
          UINT prevTableOffset = 0;
          WrappedID3D12DescriptorHeap *heap =
              rm->GetCurrentAs<WrappedID3D12DescriptorHeap>(element.id);

          size_t numRanges = param.ranges.size();
          for(size_t r = 0; r < numRanges; ++r)
          {
            const D3D12_DESCRIPTOR_RANGE1 &range = param.ranges[r];

            // For every range, check the number of descriptors so that we are accessing the
            // correct data for append descriptor tables, even if the range type doesn't match
            // what we need to fetch
            UINT offset = range.OffsetInDescriptorsFromTableStart;
            if(range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
              offset = prevTableOffset;

            D3D12Descriptor *desc = (D3D12Descriptor *)heap->GetCPUDescriptorHandleForHeapStart().ptr;
            desc += element.offset;
            desc += offset;

            UINT numDescriptors = range.NumDescriptors;
            if(numDescriptors == UINT_MAX)
            {
              // Find out how many descriptors are left after
              numDescriptors = heap->GetNumDescriptors() - offset - (UINT)element.offset;

              // TODO: Should we look up the bind point in the D3D12 state to try to get
              // a better guess at the number of descriptors?
            }

            prevTableOffset = offset + numDescriptors;

            // Check if the range is for SRVs and the slot we want is contained
            if(range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
               slot.shaderRegister >= range.BaseShaderRegister &&
               slot.shaderRegister < range.BaseShaderRegister + numDescriptors &&
               range.RegisterSpace == slot.registerSpace)
            {
              desc += slot.shaderRegister - range.BaseShaderRegister;
              return FetchSRV(desc, slot);
            }
          }
        }
      }
    }

    RDCERR("Couldn't find root signature parameter corresponding to SRV %u in space %u",
           slot.shaderRegister, slot.registerSpace);
    return;
  }

  RDCERR("No root signature bound, couldn't identify SRV %u in space %u", slot.shaderRegister,
         slot.registerSpace);
}

void D3D12APIWrapper::FetchUAV(const D3D12Descriptor *resDescriptor, const BindingSlot &slot)
{
  if(resDescriptor)
  {
    D3D12ResourceManager *rm = m_Device->GetResourceManager();
    DXILDebug::GlobalState::UAVData &uavData = m_GlobalState.uavs[slot];
    ResourceId uavId = resDescriptor->GetResResourceId();
    ID3D12Resource *pResource = rm->GetCurrentAs<ID3D12Resource>(uavId);

    if(pResource)
    {
      // TODO: Need to fetch counter resource if applicable
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = resDescriptor->GetUAV();

      if(uavDesc.ViewDimension == D3D12_UAV_DIMENSION_UNKNOWN)
        uavDesc = MakeUAVDesc(pResource->GetDesc());

      if(uavDesc.Format != DXGI_FORMAT_UNKNOWN)
      {
        DXILDebug::FillViewFmtFromResourceFormat(uavDesc.Format, uavData.resInfo.format);
      }

      if(uavDesc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
      {
        uavData.resInfo.firstElement = (uint32_t)uavDesc.Buffer.FirstElement;
        uavData.resInfo.numElements = uavDesc.Buffer.NumElements;
        uavData.resInfo.isByteBuffer =
            ((uavDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0) ? true : false;
        // Get the buffer stride from the shader metadata (for StructuredBuffer, RawBuffer)
        uint32_t mdStride =
            DXILDebug::GetUAVBufferStrideFromShaderMetadata(m_EntryPointInterface, slot);
        if(mdStride != 0)
          uavData.resInfo.format.stride = mdStride;

        m_Device->GetDebugManager()->GetBufferData(pResource, 0, 0, uavData.data);
      }
      else
      {
        uavData.tex = true;
        m_Device->GetReplay()->GetTextureData(uavId, Subresource(), GetTextureDataParams(),
                                              uavData.data);

        uavDesc.Format = D3D12ShaderDebug::GetUAVResourceFormat(uavDesc, pResource);
        DXILDebug::FillViewFmtFromResourceFormat(uavDesc.Format, uavData.resInfo.format);
        D3D12_RESOURCE_DESC resDesc = pResource->GetDesc();
        uavData.rowPitch = GetByteSize((int)resDesc.Width, 1, 1, uavDesc.Format, 0);
        uavData.depthPitch =
            GetByteSize((int)resDesc.Width, (int)(resDesc.Height), 1, uavDesc.Format, 0);
      }
    }
  }
}

void D3D12APIWrapper::FetchUAV(const BindingSlot &slot)
{
  // if the UAV might be dirty from side-effects from the action, replay back to right
  // before it.
  if(!m_DidReplay)
  {
    D3D12MarkerRegion region(m_Device->GetQueue()->GetReal(), "un-dirtying resources");
    m_Device->ReplayLog(0, m_EventId, eReplay_WithoutDraw);
    m_DidReplay = true;
  }

  // Direct access resource
  if(slot.heapType != DXDebug::HeapDescriptorType::NoHeap)
  {
    const HeapDescriptorType heapType = slot.heapType;
    const uint32_t descriptorIndex = slot.descriptorIndex;
    const D3D12Descriptor uavDesc =
        D3D12ShaderDebug::FindDescriptor(m_Device, heapType, descriptorIndex);
    return FetchUAV(&uavDesc, slot);
  }

  const D3D12RenderState &rs = m_Device->GetQueue()->GetCommandData()->m_RenderState;
  D3D12ResourceManager *rm = m_Device->GetResourceManager();

  // Get the root signature
  const D3D12RenderState::RootSignature *pRootSignature = NULL;
  if(m_ShaderType == DXBC::ShaderType::Compute)
  {
    if(rs.compute.rootsig != ResourceId())
    {
      pRootSignature = &rs.compute;
    }
  }
  else if(rs.graphics.rootsig != ResourceId())
  {
    pRootSignature = &rs.graphics;
  }

  if(pRootSignature)
  {
    WrappedID3D12RootSignature *pD3D12RootSig =
        rm->GetCurrentAs<WrappedID3D12RootSignature>(pRootSignature->rootsig);

    size_t numParams = RDCMIN(pD3D12RootSig->sig.Parameters.size(), pRootSignature->sigelems.size());
    for(size_t i = 0; i < numParams; ++i)
    {
      const D3D12RootSignatureParameter &param = pD3D12RootSig->sig.Parameters[i];
      const D3D12RenderState::SignatureElement &element = pRootSignature->sigelems[i];
      if(IsShaderParameterVisible(m_ShaderType, param.ShaderVisibility))
      {
        if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV && element.type == eRootUAV)
        {
          if(param.Descriptor.ShaderRegister == slot.shaderRegister &&
             param.Descriptor.RegisterSpace == slot.registerSpace)
          {
            // Found the requested UAV
            ID3D12Resource *pResource = rm->GetCurrentAs<ID3D12Resource>(element.id);

            if(pResource)
            {
              DXILDebug::GlobalState::UAVData &uavData = m_GlobalState.uavs[slot];
              D3D12_RESOURCE_DESC resDesc = pResource->GetDesc();
              // DXC allows root buffers to have a stride of up to 16 bytes in the shader, which
              // means encoding the byte offset into the first element here is wrong without
              // knowing what the actual accessed stride is. Instead we only fetch the data from
              // that offset onwards.

              // Root buffers are typeless, try with the resource desc format
              // The debugger code will handle DXGI_FORMAT_UNKNOWN
              if(resDesc.Format != DXGI_FORMAT_UNKNOWN)
              {
                DXILDebug::FillViewFmtFromResourceFormat(resDesc.Format, uavData.resInfo.format);
              }

              uavData.resInfo.isRootDescriptor = true;
              uavData.resInfo.firstElement = 0;
              // root arguments have no bounds checking, use the most conservative number of elements
              uavData.resInfo.numElements = uint32_t(resDesc.Width - element.offset);

              if(resDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
              {
                // Get the buffer stride from the shader metadata (for StructuredBuffer, RawBuffer)
                uint32_t mdStride =
                    DXILDebug::GetUAVBufferStrideFromShaderMetadata(m_EntryPointInterface, slot);
                if(mdStride != 0)
                  uavData.resInfo.format.stride = mdStride;
                m_Device->GetDebugManager()->GetBufferData(pResource, element.offset, 0,
                                                           uavData.data);
              }
            }

            return;
          }
        }
        else if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                element.type == eRootTable)
        {
          UINT prevTableOffset = 0;
          WrappedID3D12DescriptorHeap *heap =
              rm->GetCurrentAs<WrappedID3D12DescriptorHeap>(element.id);

          size_t numRanges = param.ranges.size();
          for(size_t r = 0; r < numRanges; ++r)
          {
            const D3D12_DESCRIPTOR_RANGE1 &range = param.ranges[r];

            // For every range, check the number of descriptors so that we are accessing the
            // correct data for append descriptor tables, even if the range type doesn't match
            // what we need to fetch
            UINT offset = range.OffsetInDescriptorsFromTableStart;
            if(range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
              offset = prevTableOffset;

            D3D12Descriptor *desc = (D3D12Descriptor *)heap->GetCPUDescriptorHandleForHeapStart().ptr;
            desc += element.offset;
            desc += offset;

            UINT numDescriptors = range.NumDescriptors;
            if(numDescriptors == UINT_MAX)
            {
              // Find out how many descriptors are left after
              numDescriptors = heap->GetNumDescriptors() - offset - (UINT)element.offset;

              // TODO: Should we look up the bind point in the D3D12 state to try to get
              // a better guess at the number of descriptors?
            }

            prevTableOffset = offset + numDescriptors;

            // Check if the range is for UAVs and the slot we want is contained
            if(range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
               slot.shaderRegister >= range.BaseShaderRegister &&
               slot.shaderRegister < range.BaseShaderRegister + numDescriptors &&
               range.RegisterSpace == slot.registerSpace)
            {
              desc += slot.shaderRegister - range.BaseShaderRegister;
              return FetchUAV(desc, slot);
            }
          }
        }
      }
    }

    RDCERR("Couldn't find root signature parameter corresponding to UAV %u in space %u",
           slot.shaderRegister, slot.registerSpace);
    return;
  }

  RDCERR("No root signature bound, couldn't identify UAV %u in space %u", slot.shaderRegister,
         slot.registerSpace);
}

bool D3D12APIWrapper::CalculateMathIntrinsic(DXIL::DXOp dxOp, const ShaderVariable &input,
                                             ShaderVariable &output)
{
  D3D12MarkerRegion region(m_Device->GetQueue()->GetReal(), "CalculateMathIntrinsic");

  int mathOp;
  switch(dxOp)
  {
    case DXOp::Cos: mathOp = DEBUG_SAMPLE_MATH_DXIL_COS; break;
    case DXOp::Sin: mathOp = DEBUG_SAMPLE_MATH_DXIL_SIN; break;
    case DXOp::Tan: mathOp = DEBUG_SAMPLE_MATH_DXIL_TAN; break;
    case DXOp::Acos: mathOp = DEBUG_SAMPLE_MATH_DXIL_ACOS; break;
    case DXOp::Asin: mathOp = DEBUG_SAMPLE_MATH_DXIL_ASIN; break;
    case DXOp::Atan: mathOp = DEBUG_SAMPLE_MATH_DXIL_ATAN; break;
    case DXOp::Hcos: mathOp = DEBUG_SAMPLE_MATH_DXIL_HCOS; break;
    case DXOp::Hsin: mathOp = DEBUG_SAMPLE_MATH_DXIL_HSIN; break;
    case DXOp::Htan: mathOp = DEBUG_SAMPLE_MATH_DXIL_HTAN; break;
    case DXOp::Exp: mathOp = DEBUG_SAMPLE_MATH_DXIL_EXP; break;
    case DXOp::Log: mathOp = DEBUG_SAMPLE_MATH_DXIL_LOG; break;
    case DXOp::Sqrt: mathOp = DEBUG_SAMPLE_MATH_DXIL_SQRT; break;
    case DXOp::Rsqrt: mathOp = DEBUG_SAMPLE_MATH_DXIL_RSQRT; break;
    default:
      // To support a new instruction, the shader created in
      // D3D12DebugManager::CreateShaderDebugResources will need updating
      RDCERR("Unsupported opcode for DXIL CalculateMathIntrinsic: %s %u", ToStr(dxOp).c_str(),
             (uint)dxOp);
      return false;
  }

  ShaderVariable ignored;
  return D3D12ShaderDebug::CalculateMathIntrinsic(true, m_Device, mathOp, input, output, ignored);
}

bool D3D12APIWrapper::CalculateSampleGather(
    DXIL::DXOp dxOp, SampleGatherResourceData resourceData, SampleGatherSamplerData samplerData,
    const ShaderVariable &uv, const ShaderVariable &ddxCalc, const ShaderVariable &ddyCalc,
    const int8_t texelOffsets[3], int multisampleIndex, float lodValue, float compareValue,
    const uint8_t swizzle[4], GatherChannel gatherChannel, DXBC::ShaderType shaderType,
    uint32_t instructionIdx, const char *opString, ShaderVariable &output)
{
  int sampleOp;
  switch(dxOp)
  {
    case DXOp::Sample: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE; break;
    case DXOp::SampleBias: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_BIAS; break;
    case DXOp::SampleLevel: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_LEVEL; break;
    case DXOp::SampleGrad: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_GRAD; break;
    case DXOp::SampleCmp: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_CMP; break;
    case DXOp::SampleCmpBias: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_CMP_BIAS; break;
    case DXOp::SampleCmpLevel: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_CMP_LEVEL; break;
    case DXOp::SampleCmpGrad: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_CMP_GRAD; break;
    case DXOp::SampleCmpLevelZero: sampleOp = DEBUG_SAMPLE_TEX_SAMPLE_CMP_LEVEL_ZERO; break;
    case DXOp::TextureGather: sampleOp = DEBUG_SAMPLE_TEX_GATHER4; break;
    case DXOp::TextureGatherCmp: sampleOp = DEBUG_SAMPLE_TEX_GATHER4_CMP; break;
    case DXOp::CalculateLOD: sampleOp = DEBUG_SAMPLE_TEX_LOD; break;
    // In the shader DEBUG_SAMPLE_TEX_LOAD and DEBUG_SAMPLE_TEX_LOAD_MS behave equivalently
    case DXOp::TextureLoad: sampleOp = DEBUG_SAMPLE_TEX_LOAD; break;
    default:
      // To support a new instruction, the shader created in
      // D3D12DebugManager::CreateShaderDebugResources will need updating
      RDCERR("Unsupported instruction for CalculateSampleGather: %s %u", ToStr(dxOp).c_str(), dxOp);
      return false;
  }

  return D3D12ShaderDebug::CalculateSampleGather(
      true, m_Device, sampleOp, resourceData, samplerData, uv, ddxCalc, ddyCalc, texelOffsets,
      multisampleIndex, lodValue, compareValue, swizzle, gatherChannel, shaderType, instructionIdx,
      opString, output);
}

ShaderVariable D3D12APIWrapper::GetResourceInfo(DXIL::ResourceClass resClass,
                                                const DXDebug::BindingSlot &slot, uint32_t mipLevel,
                                                const DXBC::ShaderType shaderType, int &dim)
{
  D3D12_DESCRIPTOR_RANGE_TYPE descType;
  switch(resClass)
  {
    case DXIL::ResourceClass::SRV: descType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; break;
    case DXIL::ResourceClass::UAV: descType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; break;
    case DXIL::ResourceClass::CBuffer: descType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; break;
    case DXIL::ResourceClass::Sampler: descType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; break;
    default:
      RDCERR("Unsupported resource class %s", ToStr(resClass).c_str());
      return ShaderVariable();
  }
  return D3D12ShaderDebug::GetResourceInfo(m_Device, descType, slot, mipLevel, shaderType, dim, true);
}

ShaderVariable D3D12APIWrapper::GetSampleInfo(DXIL::ResourceClass resClass,
                                              const DXDebug::BindingSlot &slot,
                                              const DXBC::ShaderType shaderType, const char *opString)
{
  D3D12_DESCRIPTOR_RANGE_TYPE descType;
  switch(resClass)
  {
    case DXIL::ResourceClass::SRV: descType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; break;
    case DXIL::ResourceClass::UAV: descType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; break;
    case DXIL::ResourceClass::CBuffer: descType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; break;
    case DXIL::ResourceClass::Sampler: descType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; break;
    default:
      RDCERR("Unsupported resource class %s", ToStr(resClass).c_str());
      return ShaderVariable();
  }
  return D3D12ShaderDebug::GetSampleInfo(m_Device, descType, slot, shaderType, opString);
}

ShaderVariable D3D12APIWrapper::GetRenderTargetSampleInfo(const DXBC::ShaderType shaderType,
                                                          const char *opString)
{
  return D3D12ShaderDebug::GetRenderTargetSampleInfo(m_Device, shaderType, opString);
}

ResourceReferenceInfo D3D12APIWrapper::GetResourceReferenceInfo(const DXDebug::BindingSlot &slot)
{
  const HeapDescriptorType heapType = slot.heapType;
  RDCASSERT(heapType != HeapDescriptorType::NoHeap);
  const uint32_t descriptorIndex = slot.descriptorIndex;
  D3D12Descriptor desc = D3D12ShaderDebug::FindDescriptor(m_Device, heapType, descriptorIndex);

  D3D12ResourceManager *rm = m_Device->GetResourceManager();

  ResourceReferenceInfo resRefInfo;
  resRefInfo.binding.heapType = heapType;
  resRefInfo.binding.descriptorIndex = descriptorIndex;

  switch(desc.GetType())
  {
    case D3D12DescriptorType::CBV:
    {
      resRefInfo.resClass = DXIL::ResourceClass::CBuffer;
      resRefInfo.descType = DescriptorType::ConstantBuffer;
      resRefInfo.varType = VarType::ConstantBlock;
    }
    case D3D12DescriptorType::SRV:
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = desc.GetSRV();
      ResourceId srvId = desc.GetResResourceId();
      ID3D12Resource *pResource = rm->GetCurrentAs<ID3D12Resource>(srvId);
      if(pResource)
      {
        resRefInfo.resClass = DXIL::ResourceClass::SRV;
        resRefInfo.varType = VarType::ReadOnlyResource;
        if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_UNKNOWN)
          srvDesc = MakeSRVDesc(pResource->GetDesc());

        GlobalState::ViewFmt viewFmt;
        if(srvDesc.Format != DXGI_FORMAT_UNKNOWN)
        {
          resRefInfo.srvData.dim =
              (DXDebug::ResourceDimension)ConvertSRVResourceDimensionToResourceDimension(
                  srvDesc.ViewDimension);

          DXILDebug::FillViewFmtFromResourceFormat(srvDesc.Format, viewFmt);
          resRefInfo.srvData.compType =
              (DXDebug::ResourceRetType)ConvertCompTypeToResourceRetType(viewFmt.compType);

          D3D12_RESOURCE_DESC resDesc = pResource->GetDesc();
          resRefInfo.srvData.sampleCount = resDesc.SampleDesc.Count;
        }
      }
      else
      {
        RDCERR("Unknown SRV resource at Descriptor Index %u", descriptorIndex);
        return ResourceReferenceInfo();
      }

      if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
        resRefInfo.descType = DescriptorType::AccelerationStructure;
      else if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
              (srvDesc.Buffer.StructureByteStride > 0 || srvDesc.Format == DXGI_FORMAT_UNKNOWN))
        resRefInfo.descType = DescriptorType::Buffer;
      else if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
        resRefInfo.descType = DescriptorType::TypedBuffer;
      else
        resRefInfo.descType = DescriptorType::Image;

      break;
    }
    case D3D12DescriptorType::UAV:
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = desc.GetUAV();
      resRefInfo.resClass = DXIL::ResourceClass::UAV;
      resRefInfo.varType = VarType::ReadWriteResource;
      if(uavDesc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
        resRefInfo.descType = uavDesc.Format != DXGI_FORMAT_UNKNOWN
                                  ? DescriptorType::ReadWriteTypedBuffer
                                  : DescriptorType::ReadWriteBuffer;
      else
        resRefInfo.descType = DescriptorType::ReadWriteImage;
      break;
    }
    case D3D12DescriptorType::Sampler:
    {
      resRefInfo.resClass = DXIL::ResourceClass::Sampler;
      resRefInfo.descType = DescriptorType::Sampler;
      resRefInfo.varType = VarType::Sampler;
      D3D12_SAMPLER_DESC2 samplerDesc = desc.GetSampler();
      // Don't think SAMPLER_MODE_MONO is supported in D3D12 (set for filter mode D3D10_FILTER_TEXT_1BIT)
      resRefInfo.samplerData.samplerMode =
          (DXDebug::SamplerMode)ConvertSamplerFilterToSamplerMode(samplerDesc.Filter);
      break;
    }
    default:
      RDCERR("Unhandled Descriptor Type %s", ToStr(desc.GetType()).c_str());
      return ResourceReferenceInfo();
  }
  return resRefInfo;
}

ShaderDirectAccess D3D12APIWrapper::GetShaderDirectAccess(DescriptorType type,
                                                          const DXDebug::BindingSlot &slot)
{
  const HeapDescriptorType heapType = slot.heapType;
  RDCASSERT(heapType != HeapDescriptorType::NoHeap);
  uint32_t descriptorIndex = slot.descriptorIndex;

  const D3D12RenderState &rs = m_Device->GetQueue()->GetCommandData()->m_RenderState;
  D3D12ResourceManager *rm = m_Device->GetResourceManager();

  ShaderDirectAccess access;
  uint32_t byteSize = DXILDebug::D3D12_DESCRIPTOR_BYTESIZE;
  uint32_t byteOffset = descriptorIndex * byteSize;

  // Fetch the correct heap sampler and resource descriptor heap
  rdcarray<ResourceId> descHeaps = rs.heaps;
  for(ResourceId heapId : descHeaps)
  {
    WrappedID3D12DescriptorHeap *pD3D12Heap = rm->GetCurrentAs<WrappedID3D12DescriptorHeap>(heapId);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = pD3D12Heap->GetDesc();
    if(heapDesc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
    {
      if(heapType == HeapDescriptorType::Sampler)
      {
        RDCASSERTEQUAL(CategoryForDescriptorType(type), DescriptorCategory::Sampler);
        return ShaderDirectAccess(type, rm->GetOriginalID(heapId), byteOffset, byteSize);
      }
    }
    else
    {
      RDCASSERT(heapDesc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      if(heapType == HeapDescriptorType::CBV_SRV_UAV)
      {
        RDCASSERTNOTEQUAL(CategoryForDescriptorType(type), DescriptorCategory::Sampler);
        return ShaderDirectAccess(type, rm->GetOriginalID(heapId), byteOffset, byteSize);
      }
    }
  }
  RDCERR("Failed to find descriptor %u %u", (uint32_t)heapType, descriptorIndex);
  return ShaderDirectAccess();
}
};
