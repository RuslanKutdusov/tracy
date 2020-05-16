#ifndef __TRACYD3D12_HPP__
#define __TRACYD3D12_HPP__

#if !defined TRACY_ENABLE

#define TracyBeginD3DZone(ctx, srcloc, cmdList, is_active) ((void)0)
#define TracyEndD3DZone(ctx, cmdList) ((void)0)
#define TracyCreateD3D12CommandQueueCtx(device, queue) nullptr
#define TracyDestroyD3D12CommandQueueCtx(ctx) ((void)0)
#define TracyCreateD3D12CommandAllocatorCtx(queueCtx, device, type) nullptr
#define TracyDestroyD3D12CommandAllocatorCtx(ctx) ((void)0)
#define TracyPreCmdListExecute(ctx, cmdList) ((void)0)
#define TracyCollectGpuQueries(ctx) ((void)0)


namespace tracy {
	class D3D12CommandQueueCtx;
	class D3D12CommandAllocatorCtx;
}

#else

#include <assert.h>
#include <stdlib.h>
#include <d3d12.h>
#include "Tracy.hpp"
#include "client/TracyProfiler.hpp"
#include "client/TracyCallstack.hpp"

namespace tracy {

	class D3D12CommandQueueCtx
	{
	public:
		D3D12CommandQueueCtx(ID3D12Device* device, ID3D12CommandQueue* queue) : m_context(GetGpuCtxCounter().fetch_add(1, std::memory_order_relaxed))
		{
			if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY)
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
				memset(&options3, 0, sizeof(options3));
				HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
				if (FAILED(hr) || !options3.CopyQueueTimestampQueriesSupported)
					return;
			}

			uint64_t freq;
			queue->GetTimestampFrequency(&freq);
			// number of nanoseconds required for a timestamp query to be incremented by 1
			const float period = 1e+09f / (float)freq;

			uint64_t d3dCpuTimestamp, d3dGpuTimestamp;
			queue->GetClockCalibration(&d3dGpuTimestamp, &d3dCpuTimestamp);
			int64_t tcpu = Profiler::GetTime();
			int64_t tgpu = d3dGpuTimestamp;

			auto item = Profiler::QueueSerial();
			MemWrite(&item->hdr.type, QueueType::GpuNewContext);
			MemWrite(&item->gpuNewContext.cpuTime, tcpu);
			MemWrite(&item->gpuNewContext.gpuTime, tgpu);
			memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
			MemWrite(&item->gpuNewContext.period, period);
			MemWrite(&item->gpuNewContext.context, m_context);
			MemWrite(&item->gpuNewContext.accuracyBits, uint8_t(0));
#ifdef TRACY_ON_DEMAND
			GetProfiler().DeferItem(*item);
#endif
			Profiler::QueueSerialFinish();

			m_inited = true;
		}

		bool IsInited() const
		{
			return m_inited;
		}

		uint8_t GetId() const
		{
			return m_context;
		}

		uint32_t NewCmdAllocatorCtx()
		{
			assert(m_cmdAllocatorCtxNum <= 64);
			return m_cmdAllocatorCtxNum++;
		}

	private:
		bool m_inited = false;
		uint8_t m_context;
		uint32_t m_cmdAllocatorCtxNum = 0;
	};

	class D3D12CommandAllocatorCtx
	{
		friend class D3DCtxScope;

		enum
		{
			QueryCount = 1024
		};

	public:
		struct Query
		{
			uint32_t id;
			uint32_t indexInHeap;
		};

		D3D12CommandAllocatorCtx(D3D12CommandQueueCtx* queueCtx, ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
		    : m_queueCtx(queueCtx), m_queryCount(QueryCount)
		{
			if (type == D3D12_COMMAND_LIST_TYPE_COPY)
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
				memset(&options3, 0, sizeof(options3));
				HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
				if (FAILED(hr) || !options3.CopyQueueTimestampQueriesSupported)
					return;
			}

			m_ctxIndex = m_queueCtx->NewCmdAllocatorCtx();

			D3D12_QUERY_HEAP_DESC heapDesc;
			heapDesc.Type = type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			heapDesc.NodeMask = 0;
			heapDesc.Count = m_queryCount;
			while (device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap)) != S_OK)
			{
				m_queryCount /= 2;
				heapDesc.Count = m_queryCount;
			}

			D3D12_HEAP_PROPERTIES bufferHeapProperty = {};
			bufferHeapProperty.Type = D3D12_HEAP_TYPE_READBACK;

			D3D12_RESOURCE_DESC bufferDesc;
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Alignment = 0;
			bufferDesc.Width = m_queryCount * sizeof(uint64_t);
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			bufferDesc.SampleDesc = {1, 0};
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
			device->CreateCommittedResource(&bufferHeapProperty, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                IID_PPV_ARGS(&m_resolveBuffer));

			m_inited = true;
		}

		~D3D12CommandAllocatorCtx()
		{
			if (m_queryHeap)
				m_queryHeap->Release();
			if (m_resolveBuffer)
				m_resolveBuffer->Release();
		}

		void PreCmdListExecute(ID3D12GraphicsCommandList* cmdList)
		{
			if (m_counter == 0)
				return;

			cmdList->ResolveQueryData(m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, m_counter, m_resolveBuffer, 0);
		}

		void Collect()
		{
			ZoneScopedC(Color::Red4);

			if (m_counter == 0)
				return;

#ifdef TRACY_ON_DEMAND
			if (!GetProfiler().IsConnected())
			{
				m_counter = 0;
				return;
			}
#endif

			D3D12_RANGE readRange = {0, m_counter * sizeof(int64_t)};
			void* mappedBuf = nullptr;
			m_resolveBuffer->Map(0, &readRange, &mappedBuf);
			int64_t* timestamps = static_cast<int64_t*>(mappedBuf);
			for (uint32_t idx = 0; idx < m_counter; idx++)
			{
				uint32_t queryId = (m_ctxIndex << 10) + idx;
				auto item = Profiler::QueueSerial();
				MemWrite(&item->hdr.type, QueueType::GpuTime);
				MemWrite(&item->gpuTime.gpuTime, timestamps[idx]);
				MemWrite(&item->gpuTime.queryId, uint16_t(queryId));
				MemWrite(&item->gpuTime.context, m_queueCtx->GetId());
				Profiler::QueueSerialFinish();
			}
			m_resolveBuffer->Unmap(0, nullptr);

			m_counter = 0;
		}

		tracy_force_inline ID3D12QueryHeap* GetQueryHeap() const
		{
			return m_queryHeap;
		}

		tracy_force_inline Query NextQuery()
		{
			Query query = {(m_ctxIndex << 10) + m_counter, m_counter};
			m_counter++;
			return query;
		}

		tracy_force_inline uint8_t GetId() const
		{
			return m_queueCtx->GetId();
		}

		tracy_force_inline bool IsInited() const
		{
			return m_inited;
		}

	private:
		D3D12CommandQueueCtx* m_queueCtx = nullptr;
		uint32_t m_ctxIndex = 0;
		ID3D12QueryHeap* m_queryHeap = nullptr;
		ID3D12Resource* m_resolveBuffer = nullptr;

		uint32_t m_queryCount = 0;
		uint32_t m_counter = 0;
		bool m_inited = false;
	};

	inline void BeginD3DZone(D3D12CommandAllocatorCtx* ctx, const SourceLocationData* srcloc, ID3D12GraphicsCommandList* cmdList, bool is_active)
	{
		if (!ctx->IsInited() || !is_active)
			return;

		const auto query = ctx->NextQuery();
		cmdList->EndQuery(ctx->GetQueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, query.indexInHeap);

		auto item = Profiler::QueueSerial();
#if defined TRACY_HAS_CALLSTACK && defined TRACY_CALLSTACK
		MemWrite(&item->hdr.type, QueueType::GpuZoneBeginCallstackSerial);
#else
		MemWrite(&item->hdr.type, QueueType::GpuZoneBeginSerial);
#endif
		MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
		MemWrite(&item->gpuZoneBegin.srcloc, (uint64_t)srcloc);
		MemWrite(&item->gpuZoneBegin.thread, GetThreadHandle());
		MemWrite(&item->gpuZoneBegin.queryId, uint16_t(query.id));
		MemWrite(&item->gpuZoneBegin.context, ctx->GetId());
		Profiler::QueueSerialFinish();
#if defined TRACY_HAS_CALLSTACK && defined TRACY_CALLSTACK
		GetProfiler().SendCallstack(TRACY_CALLSTACK);
#endif
	}

	inline void EndD3DZone(D3D12CommandAllocatorCtx* ctx, ID3D12GraphicsCommandList* cmdList)
	{
		/*if (!m_active)
		    return;*/

		if (!ctx->IsInited())
			return;

		const auto query = ctx->NextQuery();
		cmdList->EndQuery(ctx->GetQueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, query.indexInHeap);

		auto item = Profiler::QueueSerial();
		MemWrite(&item->hdr.type, QueueType::GpuZoneEndSerial);
		MemWrite(&item->gpuZoneEnd.cpuTime, Profiler::GetTime());
		MemWrite(&item->gpuZoneEnd.thread, GetThreadHandle());
		MemWrite(&item->gpuZoneEnd.queryId, uint16_t(query.id));
		MemWrite(&item->gpuZoneEnd.context, ctx->GetId());
		Profiler::QueueSerialFinish();
	}


	static inline D3D12CommandQueueCtx* CreateD3D12CommandQueueCtx(ID3D12Device* device, ID3D12CommandQueue* queue)
	{
		auto ctx = (D3D12CommandQueueCtx*)tracy_malloc(sizeof(D3D12CommandQueueCtx));
		new (ctx) D3D12CommandQueueCtx(device, queue);
		return ctx;
	}

	static inline void DestroyD3D12CommandQueueCtx(D3D12CommandQueueCtx* ctx)
	{
		ctx->~D3D12CommandQueueCtx();
		tracy_free(ctx);
	}

	static inline D3D12CommandAllocatorCtx* CreateD3D12CommandAllocatorCtx(D3D12CommandQueueCtx* queueCtx, ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
	{
		auto ctx = (D3D12CommandAllocatorCtx*)tracy_malloc(sizeof(D3D12CommandAllocatorCtx));
		new (ctx) D3D12CommandAllocatorCtx(queueCtx, device, type);
		return ctx;
	}

	static inline void DestroyD3D12CommandAllocatorCtx(D3D12CommandAllocatorCtx* ctx)
	{
		ctx->~D3D12CommandAllocatorCtx();
		tracy_free(ctx);
	}

}  // namespace tracy

#define TracyBeginD3DZone(ctx, srcloc, cmdList, is_active) tracy::BeginD3DZone(ctx, srcloc, cmdList, is_active)
#define TracyEndD3DZone(ctx, cmdList) tracy::EndD3DZone(ctx, cmdList)
#define TracyCreateD3D12CommandQueueCtx(device, queue) tracy::CreateD3D12CommandQueueCtx(device, queue)
#define TracyDestroyD3D12CommandQueueCtx(ctx) tracy::DestroyD3D12CommandQueueCtx(ctx)
#define TracyCreateD3D12CommandAllocatorCtx(queueCtx, device, type) tracy::CreateD3D12CommandAllocatorCtx(queueCtx, device, type)
#define TracyDestroyD3D12CommandAllocatorCtx(ctx) tracy::DestroyD3D12CommandAllocatorCtx(ctx)
#define TracyPreCmdListExecute(ctx, cmdList) ctx->PreCmdListExecute(cmdList)
#define TracyCollectGpuQueries(ctx) ctx->Collect()

#endif

#endif
