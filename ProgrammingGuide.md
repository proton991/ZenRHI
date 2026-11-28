# Programming with ZenRHI

## Choose and open a device

Link `ZenRHI::ZenRHI` and include `ZenRHI/DynamicRHI.h` and `ZenRHI/RHICommandListExecutor.h`. Before starting worker threads, create an `RHIDeviceDesc` and install a diagnostic callback if the host has its own log. A callback can run on any reporting thread. Keep its code and logging state alive through shutdown, synchronize concurrent calls, and copy any message that must survive the call. Configure validation with `SetValidationMode`. Required mode returns a startup error if the layer or its required settings are unavailable.

Call `EnumerateRHIAdapters(desc)` before device creation. Check the result before accessing its value. Each entry includes driver and API versions, vendor and device IDs, and an explanation when the adapter fails the device profile. Pass a supported adapter's name to `SetPreferredDevice`. A preference is a substring match with fallback to scored selection.

Call `CreateRHIChecked(desc)`. On failure, surface `GetError().operation` and `nativeCode`. The application can continue and retry after its environment changes. On success, pass the backend pointer to one `RHICommandListExecutor` on the render owner thread. The executor takes ownership. Threaded mode sends native work to an ordered worker. Inline mode executes on the owner. Device creation, enumeration and shutdown require external serialization.

To reuse Vulkan compilation data across launches, supply a previous successful snapshot through `RHIDeviceDesc::SetPipelineCacheSeed`. After compilation and before shutdown, call `SnapshotPipelineCache(byteLimit, bytes)` on the executor. Replace the host's file atomically when the snapshot and write both succeed. Choose a size bound before reading the seed. An incompatible device, changed driver cache UUID or corrupt payload starts an empty cache.

## Create and record

Read `QueryGPUInfo()` and `GetQueueCapabilities()` before using optional functionality. Create buffers, textures, samplers, shaders and pipelines through the executor. Check each factory result. Adopt the returned owning reference using `RHIResourcePtr<T>::Adopt`. For shader modules, provide SPIR-V through the copying span overload or transfer ownership of the bytes.

Begin the frame on the executor. Obtain a context for the required queue, then transfer it to `RHICommandList::Create`. Bind the pipeline, set its reflected resources and record work. Describe producer and consumer memory access with transitions. Command lists retain recorded resources. Each recording has one owner at a time.

## Submit, present and retire

For a window, create a viewport on its owner and pass it to `SubmitFrame`. For offscreen work, pass a null viewport. Keep the returned ticket alive while reading its result. A successful ticket establishes native submission. Inspect the accepted queue serials and use checked completion waits before reading back GPU output. Grouped submissions express dependencies with a queue and an earlier submission point. The executor reports accepted prefixes if a later group fails.

Transfer an owning reference to `ReleaseWhenComplete` after issuing the final work. Call `FlushRHIThread` to retire eligible references. Stop producers before destroying the executor. Finish or discard recordings, wait for accepted GPU uses, release resources and destroy viewports on the window owner. A terminal device loss ends rendering and delivers the device report through the callback.

## Calling threads

The table gives the supported caller for each operation family. Overloads share the same rule. Call remaining executor operations on the render owner and remaining raw backend operations on the native owner. Threaded execution assigns native ownership to the RHI worker. Inline execution assigns it to the render owner.

| Entry points | Caller and ordering |
| --- | --- |
| `EnumerateRHIAdapters`, `CreateRHIChecked`, `CreateRHI`, executor constructor and destructor | Startup and shutdown owner, externally serialized across the process. Stop producers before teardown. |
| `RHIDeviceDesc::Set*`, `SetRHIMessageCallback`, `SetRHIStrictTeardownChecks` | Startup owner. Finish configuration before creating the device; the device keeps its own copy of the description. |
| Executor `BeginFrame`, `EndFrame`, `BeginGPUFrameTiming`, `EndGPUFrameTiming`, `GetCommandContext` | Render owner. Hand each context to a single recording owner. |
| Executor `SubmitFrame` (both overloads), `SubmitGroups`, `SubmitQueuedGroups`, `FlushAllGPUCommands`, `FlushRHIThread` | Render owner, ordered with frame boundaries and presentation. Finish recording before handoff. |
| Executor `CreateBuffer`, `CreateTexture`, `CreateTextureView`, `CreateSampler`, `CreateShader`, `CreatePipeline` (all overloads), `CreateAccelerationStructure`, `SnapshotPipelineCache` | Render owner in either mode. Factories and snapshots may wait for the worker. |
| `GetBufferAllocationSize`, `GetAccelerationStructureBuildSizes`, `GetTextureCopyCapabilities`, `GetSupportedDepthFormat` | Render owner through the executor. |
| `RegisterBindlessResource`, `UnregisterBindlessResource`, `IsBindlessResourceRegistered`, `CollectRetiredBindlessResources`, `ResetBindlessResources` | Render owner through the executor. Stop publishing a slot before unregistering. A heap reset requires a drained boundary. |
| `PollGPUProgress`, `QueryLastCompletedSerial`, `QueryCompletedSerials`, `WaitForCompletion`, `WaitDeviceIdle`, `ReleaseWhenComplete` | Render owner. Queries on an inline executor can call the driver. |
| `GetAPIType`, `GetName`, `QueryGPUInfo`, `GetQueueCapabilities`, `GetQueueCopyCapabilities`, `GetGPUMemoryStats`; executor cached submitted and completed serials | Any reader holding the initialized device alive. Serialize raw-backend progress queries with its owner. |
| `GetVulkanDeviceHandles` | Device owner between successful creation and teardown. Borrowed handles require the caller to observe Vulkan's host synchronization rules. |
| `CreateViewport`, viewport resize, surface callbacks, `DestroyViewport` | Window owner, coordinated with the render owner and pending frames. Keep the native window alive through destruction. |
| `RHICommandList::Create`, recording methods, rollback, detach and reset | One recording owner per list. Hand off ownership before submission or reuse. |
| `IRHICommandContext::RHI*`, raw `SubmitCommandLists`, `PrepareSubmissionDependencies` and native resource-factory operations | RHI worker in threaded mode. The render owner assumes this role in inline mode. |
| `RHIResourcePtr` copies and releases, `Destroy*` (a reference release), immutable resource descriptions, submission ticket `IsReady` and `Wait` | Any thread holding a live reference. Serialize mutation of the same handle or ticket. A waiter must allow its producer to progress. |
| Buffer `Map`, host writes and reads, `Unmap` | Exclusive host owner after the required GPU uses complete. |
| `RHIThread::Start`, `Stop`, `CloseCleanupAdmission`; executor `Destroy`, raw backend `Destroy` | Lifecycle owner. Serialize mode changes and retire the backend after ordinary producers stop. |
| `RHIThread::Dispatch*`, `TryDispatch`, `Invoke`, `InvokeChecked`, `Flush` | Any producer while the threaded worker accepts work. Native work dispatched in inline mode requires the render owner. Captured data must survive execution or cancellation. |
| `RHIThreadEvent::Signal`, `Reset`, `Wait`; submission-state and GPU-frame-timing result queries | Any thread holding a live object. The producer owns event reset and reuse. |
| Executor `AreSubmissionsBlocked`, `GetTerminalError`, `HasDeviceLoss`; `RHIThread::GetFailure`, `IsThreaded`, `IsCurrentThread` | Any thread holding the live executor or worker. These queries read published state. |
| Shader reflection, format and ray-tracing helpers, flags, error and result types, shader parameter builders and other standalone value types | Any thread with exclusive access to mutable inputs and outputs. Independent values can be used concurrently. |
| Diagnostic message callback | Any reporting thread. Keep it short and avoid waiting on the reporting worker. |

## Running the examples

Triangle creates a window and presents a graphics pipeline. ComputeReadback dispatches a compute shader and checks the mapped result. BindlessTextures indexes a global texture slot. AsyncCompute submits a producer and dependent consumer to two logical queues. RayQuery builds an empty TLAS and verifies a shader miss. Capability-dependent executables return 77 when their required feature is unavailable.

The [reference contracts](include/ZenRHI/README.md) describe queue acceptance, bindless slot reuse and terminal errors.
