/* Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
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
#include "tensorrt/laboratory/core/resources.h"
#include "tensorrt/laboratory/core/thread_pool.h"

using trtlab::Resources;
using trtlab::ThreadPool;

#include "nvrpc/executor.h"
#include "nvrpc/server.h"
#include "nvrpc/service.h"

using nvrpc::AsyncRPC;
using nvrpc::AsyncService;
using nvrpc::Executor;
using nvrpc::Server;

#include "echo.grpc.pb.h"
#include "echo.pb.h"

#include <gtest/gtest.h>

using namespace nvrpc;

// clang-format off
struct TestResources : public Resources
{
    TestResources(int numThreadsInPool = 3) : m_ThreadPool(numThreadsInPool) {}
    ThreadPool& AcquireThreadPool() { return m_ThreadPool; }
  private:
    ThreadPool m_ThreadPool;
};
// clang-format on

class EchoContext final : public StreamingContext<simple::Input, simple::Output, TestResources>
{
    void RequestReceived(RequestType&& input, std::shared_ptr<ServerStream> stream) final override
    {
        simple::Output output;
        output.set_batch_id(input.batch_id());
        stream->WriteResponse(std::move(output));
    }
};

class ServerTest : public ::testing::Test
{
    void SetUp() override
    {
        m_BackgroundThreads = std::make_unique<ThreadPool>(1);
        BuildAndStartServer();
    }

    void TearDown() override
    {
        if(m_Server)
        {
            m_Server->Shutdown();
            m_Server.reset();
        }
        m_BackgroundThreads.reset();
    }

    void BuildAndStartServer()
    {
        m_Server = std::make_unique<Server>("0.0.0.0:13377");
        auto simpleInference = m_Server->RegisterAsyncService<simple::Inference>();
        auto rpcCompute = simpleInference->RegisterRPC<EchoContext>(
            &simple::Inference::AsyncService::RequestBidirectional);
        auto rpcResources = std::make_shared<TestResources>(3);
        auto executor = m_Server->RegisterExecutor(new Executor(1));
        executor->RegisterContexts(rpcCompute, rpcResources, 10);
    }

  protected:
    volatile bool m_Running;
    std::unique_ptr<Server> m_Server;
    std::unique_ptr<ThreadPool> m_BackgroundThreads;
};

TEST_F(ServerTest, AsyncStartAndShutdown)
{
    EXPECT_FALSE(m_Server->Running());
    m_Server->AsyncStart();
    EXPECT_TRUE(m_Server->Running());
    m_Server->Shutdown();
    EXPECT_FALSE(m_Server->Running());
}

TEST_F(ServerTest, RunAndShutdown)
{
    bool running = false;
    std::mutex mutex;
    std::condition_variable condition;

    EXPECT_FALSE(m_Server->Running());
    m_BackgroundThreads->enqueue([&, this] {
        m_Server->Run(std::chrono::milliseconds(1), [&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                running = true;
            }
            condition.notify_all();
        });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&running]{ return running; });
    }
    EXPECT_TRUE(m_Server->Running());
    m_Server->Shutdown();
    EXPECT_FALSE(m_Server->Running());
}