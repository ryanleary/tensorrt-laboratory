/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "tensorrt/laboratory/graph_workspace.h"

#include "tensorrt/laboratory/core/memory/allocator.h"
#include "tensorrt/laboratory/core/utils.h"
#include "tensorrt/laboratory/cuda/device_info.h"

using trtlab::Memory::Allocator;
using trtlab::Memory::CudaDeviceMemory;
using trtlab::Memory::MemoryStack;

namespace {

std::size_t Align(std::size_t size, std::size_t alignment)
{
    std::size_t remainder = size % alignment;
    size = (remainder == 0) ? size : size + alignment - remainder;
    return size;
}

} // namespace

namespace trtlab {
namespace TensorRT {

GraphWorkspace::GraphWorkspace() : m_DeviceStackSize(0), m_ActivationsSize(0)
{
    DLOG(INFO) << "GraphWorkspace Constructor";
    CHECK_EQ(cudaStreamCreate(&m_Stream), cudaSuccess);
}

GraphWorkspace::~GraphWorkspace()
{
    DLOG(INFO) << "GraphWorkspace Deconstructor";
    CHECK_EQ(cudaStreamSynchronize(m_Stream), CUDA_SUCCESS);

    DLOG(INFO) << "Destroying GraphExecutors";
    for(auto& item : m_GraphExecutors)
    {
        CHECK_EQ(cudaGraphExecDestroy(item.second), CUDA_SUCCESS);
    }

    DLOG(INFO) << "Destroying Graphs";
    for(auto& item : m_Graphs)
    {
        CHECK_EQ(cudaGraphDestroy(item.second), CUDA_SUCCESS);
    }

    CHECK_EQ(cudaStreamDestroy(m_Stream), CUDA_SUCCESS);
}

void GraphWorkspace::RegisterModel(const std::string& name, std::shared_ptr<Model> model,
                                   uint32_t batch_size)
{
    if(m_BindingsStack || m_ActivationSpace)
    {
        LOG(FATAL) << "Registration of new models is not allowed after graph creation";
    }

    CHECK_LE(batch_size, model->GetMaxBatchSize());

    auto key = MakeKey(name, batch_size);
    auto search = m_ModelsAndBatchSize.find(key);
    if(search != m_ModelsAndBatchSize.end())
    {
        LOG(FATAL) << "Model collsion; Model with name=" << name << " and bs=" << batch_size
                   << " is already registered.";
    }

    // Size according to largest padding - device alignment
    size_t bindings =
        model->GetBindingMemorySize() + model->GetBindingsCount() * DeviceInfo::Alignment();
    size_t activations = Align(model->GetActivationsMemorySize(), 128 * 1024); // add a cacheline

    size_t device = Align(bindings, 128 * 1024);

    m_DeviceStackSize = std::max(m_DeviceStackSize, device);
    m_ActivationsSize = std::max(m_ActivationsSize, activations);

    VLOG(2) << "-- Registering Model: " << name << " --";
    VLOG(2) << "Input/Output Tensors require " << BytesToString(device);
    VLOG(2) << "Execution Activations require " << BytesToString(activations);
    auto weights = model->GetWeightsMemorySize();
    if(weights)
    {
        VLOG(2) << "Weights require " << BytesToString(weights);
    }

    model->SetName(name);
    m_Models[name] = model;
    m_ExecutionContexts[name] = model->CreateExecutionContext();
    m_ModelsAndBatchSize[key] = model;
}

void GraphWorkspace::BuildGraphs()
{
    if(m_Models.size() == 0)
    {
        LOG(INFO) << "No Graphs Registered";
        return;
    }

    DCHECK_GT(m_DeviceStackSize, 0);
    DCHECK_GT(m_ActivationsSize, 0);

    // Allocate memory based on registration statistics
    m_BindingsStack = std::make_unique<MemoryStack<CudaDeviceMemory>>(m_DeviceStackSize);
    m_ActivationSpace = std::make_unique<Allocator<CudaDeviceMemory>>(m_ActivationsSize);

    for(const auto& item : m_ModelsAndBatchSize)
    {
        // Unpack map entry { std::string, std::shared_ptr<Model> }
        const auto& key = item.first;
        const auto& model = *(item.second);
        const auto& name = key.first;
        const auto& batch_size = key.second;

        DLOG(INFO) << "Building Graph for: " << name << "; bs=" << batch_size;

        // Set the IExecutionContext to use the Workspace's activation memory
        auto& ctx = m_ExecutionContexts.at(name);
        ctx->setDeviceMemory(m_ActivationSpace->Data());

        // Push Model Bindings on Device Memory Stack
        // These pointers will get baked into the graph
        std::vector<void*> bindings;

        // Only push and save the stack once per model since we allocated for max_batch_size
        auto search = m_DeviceBindings.find(name);
        if(search == m_DeviceBindings.end())
        {
            auto max_batch_size = model.GetMaxBatchSize();
            bindings.resize(model.GetBindingsCount());
            for(int i = 0; i < model.GetBindingsCount(); i++)
            {
                const auto& info = model.GetBinding(i);
                auto size = max_batch_size * info.bytesPerBatchItem;
                bindings[i] = m_BindingsStack->Allocate(size);
            }
            m_DeviceBindings[name] = bindings;
        }
        else
        {
            bindings = m_DeviceBindings[name];
        }

        // Build Graph
        cudaGraph_t graph;
        // these fail
        // CHECK_EQ(cudaStreamBeginCapture(m_Stream, cudaStreamCaptureModeGlobal), CUDA_SUCCESS);
        // CHECK_EQ(cudaStreamBeginCapture(m_Stream, cudaStreamCaptureModeThreadLocal),
        // CUDA_SUCCESS);

#if (CUDA_VERSION >= 10010)
        CHECK_EQ(cudaStreamBeginCapture(m_Stream, cudaStreamCaptureModeRelaxed), CUDA_SUCCESS);
#else
        CHECK_EQ(cudaStreamBeginCapture(m_Stream), CUDA_SUCCESS);
#endif
        ctx->enqueue(batch_size, (void**)bindings.data(), m_Stream, nullptr);
        CHECK_EQ(cudaStreamEndCapture(m_Stream, &graph), CUDA_SUCCESS);

        // Store the Graph by Model Name
        m_Graphs[key] = graph;
        // m_DeviceBindings[name] = std::move(bindings);

        // Reset the Device Memory Stack
        m_BindingsStack->Reset();

        cudaGraphExec_t graphExec;
        CHECK_EQ(cudaGraphInstantiate(&graphExec, graph, NULL, NULL, 0), CUDA_SUCCESS);
        m_GraphExecutors[key] = graphExec;
    }
}

bool GraphWorkspace::IsModelRegistered(const std::string& name) const
{
    auto search = m_Models.find(name);
    if(search == m_Models.end())
    {
        return false;
    }
    return true;
}

bool GraphWorkspace::IsGraphAvailable(const std::string& name, uint32_t batch_size) const
{
    auto key = MakeKey(name, batch_size);
    auto search = m_GraphExecutors.find(key);
    if(search == m_GraphExecutors.end())
    {
        return false;
    }
    return true;
}

cudaGraphExec_t GraphWorkspace::GetGraph(const std::string& name, uint32_t batch_size)
{
    auto key = MakeKey(name, batch_size);
    auto search = m_GraphExecutors.find(key);
    if(search == m_GraphExecutors.end())
    {
        throw std::runtime_error("No graph executor for " + name);
    }
    return search->second;
}

std::vector<void*> GraphWorkspace::DeviceBindingsByName(const std::string& name)
{
    auto search = m_DeviceBindings.find(name);
    if(search == m_DeviceBindings.end())
    {
        throw std::runtime_error("No DeviceBindings for model: " + name);
    }
    return search->second;
}

void GraphWorkspace::Synchronize()
{
    CHECK_EQ(cudaStreamSynchronize(m_Stream), CUDA_SUCCESS);
}

} // namespace TensorRT
} // namespace trtlab