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

#include "driver/shaders/dxil/dxil_debug.h"
#include "d3d12_device.h"
#include "d3d12_shaderdebug.h"
#include "d3d12_state.h"

namespace DXILDebug
{
class Debugger;

class D3D12APIWrapper : public DebugAPIWrapper
{
public:
  D3D12APIWrapper(WrappedID3D12Device *device, const DXIL::Program *dxilProgram,
                  const ShaderReflection &refl, uint32_t eventId,
                  const rdcarray<SigParameter> &inputSig);
  ~D3D12APIWrapper();

  void FetchConstantBufferData(const D3D12RenderState::RootSignature &rootsig);

  const UAVData &GetUAVData(const BindingSlot &slot) override;
  const SRVData &GetSRVData(const BindingSlot &slot) override;

  bool CalculateMathIntrinsic(DXIL::DXOp dxOp, const ShaderVariable &input,
                              ShaderVariable &output) override;
  bool CalculateSampleGather(DXIL::DXOp dxOp, SampleGatherResourceData resourceData,
                             SampleGatherSamplerData samplerData, const ShaderVariable &uv,
                             const ShaderVariable &ddxCalc, const ShaderVariable &ddyCalc,
                             const int8_t texelOffsets[3], int multisampleIndex, float lodValue,
                             float compareValue, GatherChannel gatherChannel,
                             uint32_t instructionIdx, ShaderVariable &output) override;

  ShaderVariable GetResourceInfo(DXIL::ResourceClass resClass, const DXDebug::BindingSlot &slot,
                                 uint32_t mipLevel) const override;
  ShaderVariable GetSampleInfo(DXIL::ResourceClass resClass, const DXDebug::BindingSlot &slot,
                               const char *opString) const override;
  ShaderVariable GetRenderTargetSampleInfo(const char *opString) const override;
  ResourceReferenceInfo GetResourceReferenceInfo(const DXDebug::BindingSlot &slot) const override;
  ShaderDirectAccess GetShaderDirectAccess(DescriptorType type,
                                           const DXDebug::BindingSlot &slot) const override;

  bool IsSRVCached(const DXDebug::BindingSlot &slot) override;
  bool IsUAVCached(const DXDebug::BindingSlot &slot) override;

  void ResetReplay();

  void SetBuiltins(const BuiltinInputs &builtins) { m_Builtins = builtins; }
  void SetWorkgroupProperties(const rdcarray<DXILDebug::ThreadProperties> &workgroupProperties)
  {
    m_WorkgroupProperties = workgroupProperties;
  }
  void SetThreadsInputs(const rdcarray<ShaderVariable> &threadsInputs)
  {
    m_ThreadsInputs = threadsInputs;
  }
  void SetThreadsBuiltins(rdcarray<rdcflatmap<ShaderBuiltin, ShaderVariable>> &threadsBuiltins)
  {
    m_ThreadsBuiltins = threadsBuiltins;
  }
  void SetSubgroupSize(uint32_t subgroupSize) { m_SubgroupSize = subgroupSize; }

  const rdcarray<DXIL::EntryPointInterface::Signature> &GetDXILEntryPointInputs(void) const
  {
    return m_EntryPointInterface->inputs;
  }

  const rdcarray<DXILDebug::ThreadProperties> &GetWorkgroupProperties() const override
  {
    return m_WorkgroupProperties;
  }
  const rdcarray<ShaderVariable> &GetConstantBlocks() const override { return m_ConstantBlocks; }
  const std::map<ConstantBlockReference, bytebuf> &GetConstantBlocksDatas() const override
  {
    return m_ConstantBlocksDatas;
  }

  const BuiltinInputs &GetBuiltins() const override { return m_Builtins; }
  uint32_t GetSubgroupSize() const override { return m_SubgroupSize; }
  const rdcarray<rdcflatmap<ShaderBuiltin, ShaderVariable>> &GetThreadsBuiltins() const override
  {
    return m_ThreadsBuiltins;
  }
  const rdcarray<ShaderVariable> &GetThreadsInputs() const override { return m_ThreadsInputs; }
  const rdcarray<SourceVariableMapping> &GetSourceVars() const override { return m_SourceVars; }
  const ShaderVariable &GetInputPlaceholder() const override { return m_InputPlaceholder; }

private:
  void PrepareReplayForResources();
  void AddCBufferToGlobalState(const BindingSlot &slot, bytebuf &cbufData);
  void FlattenSingleVariable(const rdcstr &cbufferName, uint32_t byteOffset, const rdcstr &basename,
                             const ShaderVariable &v, rdcarray<ShaderVariable> &outvars);
  void FlattenVariables(const rdcstr &cbufferName, const rdcarray<ShaderConstant> &constants,
                        const rdcarray<ShaderVariable> &invars, rdcarray<ShaderVariable> &outvars,
                        const rdcstr &prefix, uint32_t baseOffset);

  SRVData &FetchSRV(const BindingSlot &slot);
  SRVData &FetchSRV(const D3D12Descriptor *resDescriptor, const BindingSlot &slot);

  UAVData &FetchUAV(const BindingSlot &slot);
  UAVData &FetchUAV(const D3D12Descriptor *resDescriptor, const BindingSlot &slot);

  BuiltinInputs m_Builtins;
  rdcarray<DXILDebug::ThreadProperties> m_WorkgroupProperties;
  rdcarray<ShaderVariable> m_ThreadsInputs;
  rdcarray<rdcflatmap<ShaderBuiltin, ShaderVariable>> m_ThreadsBuiltins;
  rdcarray<SourceVariableMapping> m_SourceVars;
  rdcarray<ShaderVariable> m_ConstantBlocks;
  std::map<ConstantBlockReference, bytebuf> m_ConstantBlocksDatas;
  ShaderVariable m_InputPlaceholder;
  uint32_t m_SubgroupSize = 1;

  Threading::RWLock m_UAVsLock;
  std::map<BindingSlot, UAVData> m_UAVs;
  Threading::RWLock m_SRVsLock;
  std::map<BindingSlot, SRVData> m_SRVs;

  const ShaderReflection &m_Reflection;
  WrappedID3D12Device *m_Device = NULL;
  const DXIL::Program *m_Program = NULL;
  const DXIL::EntryPointInterface *m_EntryPointInterface = NULL;
  const DXBC::ShaderType m_ShaderType;
  const uint32_t m_EventId;
  bool m_ResourcesDirty = true;
};

};
