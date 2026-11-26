#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include "runtime_state.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT swap_chain_buffer_count = 2;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), operation);
    }
}

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {
        if (!value_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateEvent");
    }
    ~Handle() { CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

class RuntimeStatePublisher {
public:
    RuntimeStatePublisher()
        : mapping_(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(frame_warp_lab::RuntimeState),
                                     frame_warp_lab::runtime_mapping_name(GetCurrentProcessId()).c_str())) {
        state_ = static_cast<frame_warp_lab::RuntimeState*>(MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(frame_warp_lab::RuntimeState)));
        if (!state_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Map runtime state");
        state_->version = frame_warp_lab::runtime_state_version;
        state_->process_id = GetCurrentProcessId();
        InterlockedExchange(&state_->present_count, 0);
        InterlockedExchange(&state_->resize_count, 0);
        InterlockedExchange(&state_->running, 1);
        record_event(1, 0);
    }
    ~RuntimeStatePublisher() {
        record_event(3, 0);
        InterlockedExchange(&state_->running, 0);
        UnmapViewOfFile(state_);
    }
    void record_present() { InterlockedIncrement(&state_->present_count); }
    void update_scene(float scene_time_seconds, bool paused) {
        InterlockedExchange(&state_->scene_time_ms, static_cast<LONG>(scene_time_seconds * 1000.0f));
        InterlockedExchange(&state_->scene_paused, paused ? 1 : 0);
    }
    void record_resize() {
        const LONG resize_count = InterlockedIncrement(&state_->resize_count);
        record_event(2, resize_count);
    }
private:
    void record_event(LONG event_type, LONG value) {
        const LONG event_sequence = state_->event_count + 1;
        auto& event = state_->events[(static_cast<std::size_t>(event_sequence) - 1) % frame_warp_lab::runtime_event_capacity];
        InterlockedExchange(&event.sequence, 0);
        FILETIME timestamp{};
        GetSystemTimeAsFileTime(&timestamp);
        event.timestamp_low = timestamp.dwLowDateTime;
        event.timestamp_high = timestamp.dwHighDateTime;
        event.event_type = event_type;
        event.value = value;
        InterlockedExchange(&event.sequence, event_sequence);
        InterlockedExchange(&state_->event_count, event_sequence);
    }
    Handle mapping_;
    frame_warp_lab::RuntimeState* state_ = nullptr;
};

class Renderer {
public:
    explicit Renderer(HWND window) : fence_completion_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        UINT flags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            flags = DXGI_CREATE_FACTORY_DEBUG;
        }
#endif
        check(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)), "Create DXGI factory");
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "Create D3D12 device");
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device_->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue_)), "Create command queue");

        RECT client_rectangle{};
        if (!GetClientRect(window, &client_rectangle)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetClientRect");
        DXGI_SWAP_CHAIN_DESC1 swap_chain_description{};
        swap_chain_description.Width = static_cast<UINT>(client_rectangle.right - client_rectangle.left);
        swap_chain_description.Height = static_cast<UINT>(client_rectangle.bottom - client_rectangle.top);
        swap_chain_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_description.SampleDesc.Count = 1;
        swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_description.BufferCount = swap_chain_buffer_count;
        swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swap_chain;
        check(factory_->CreateSwapChainForHwnd(queue_.Get(), window, &swap_chain_description, nullptr, nullptr, &swap_chain), "Create swap chain");
        check(swap_chain.As(&swap_chain_), "Query swap chain");
        check(factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Configure window association");

        D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_description{};
        descriptor_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descriptor_heap_description.NumDescriptors = swap_chain_buffer_count;
        check(device_->CreateDescriptorHeap(&descriptor_heap_description, IID_PPV_ARGS(&render_target_heap_)), "Create RTV heap");
        render_target_descriptor_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (auto& allocator : command_allocators_) {
            check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create command allocator");
        }
        check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[0].Get(), nullptr, IID_PPV_ARGS(&command_list_)), "Create command list");
        check(command_list_->Close(), "Close initial command list");
        check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Create fence");
        viewport_width_ = static_cast<UINT>(client_rectangle.right - client_rectangle.left);
        viewport_height_ = static_cast<UINT>(client_rectangle.bottom - client_rectangle.top);
        create_render_targets();
        create_scene_pipeline();
    }

    void draw(float scene_time_seconds) {
        const UINT buffer_index = swap_chain_->GetCurrentBackBufferIndex();
        wait_for_fence(buffer_fence_values_[buffer_index]);
        check(command_allocators_[buffer_index]->Reset(), "Reset command allocator");
        check(command_list_->Reset(command_allocators_[buffer_index].Get(), nullptr), "Reset command list");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = render_targets_[buffer_index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command_list_->ResourceBarrier(1, &barrier);
        auto handle = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(buffer_index) * render_target_descriptor_stride_;
        constexpr float color[] = {0.025f, 0.07f, 0.12f, 1.0f};
        command_list_->ClearRenderTargetView(handle, color, 0, nullptr);
        D3D12_VIEWPORT viewport{0, 0, static_cast<float>(viewport_width_), static_cast<float>(viewport_height_), 0, 1};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(viewport_width_), static_cast<LONG>(viewport_height_)};
        command_list_->RSSetViewports(1, &viewport);
        command_list_->RSSetScissorRects(1, &scissor);
        command_list_->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        command_list_->SetGraphicsRootSignature(scene_root_signature_.Get());
        command_list_->SetPipelineState(scene_pipeline_.Get());
        const float scene_parameters[] = {scene_time_seconds, static_cast<float>(viewport_width_), static_cast<float>(viewport_height_)};
        command_list_->SetGraphicsRoot32BitConstants(0, 3, scene_parameters, 0);
        command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list_->DrawInstanced(3, 1, 0, 0);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list_->ResourceBarrier(1, &barrier);
        check(command_list_->Close(), "Close command list");
        ID3D12CommandList* command_lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1, command_lists);
        check(swap_chain_->Present(1, 0), "Present");
        const auto value = next_fence_value_++;
        check(queue_->Signal(fence_.Get(), value), "Signal frame fence");
        buffer_fence_values_[buffer_index] = value;
    }

    void resize(UINT width, UINT height) {
        if (width == 0 || height == 0) return;
        flush();
        for (auto& target : render_targets_) target.Reset();
        check(swap_chain_->ResizeBuffers(swap_chain_buffer_count, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "Resize swap chain");
        viewport_width_ = width;
        viewport_height_ = height;
        buffer_fence_values_.fill(0);
        create_render_targets();
    }

    void flush() {
        const auto value = next_fence_value_++;
        check(queue_->Signal(fence_.Get(), value), "Signal flush fence");
        wait_for_fence(value);
    }

private:
    void create_scene_pipeline() {
        wchar_t executable_path[32768]{};
        const DWORD path_length = GetModuleFileNameW(nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
        if (path_length == 0 || path_length >= std::size(executable_path)) throw std::runtime_error("Could not locate scene shader");
        const auto shader_path = std::filesystem::path(executable_path).parent_path() / L"shaders" / L"motion_lab.hlsl";
        ComPtr<ID3DBlob> vertex_shader;
        ComPtr<ID3DBlob> pixel_shader;
        ComPtr<ID3DBlob> errors;
        auto compile_shader = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& shader) {
            errors.Reset();
            const HRESULT result = D3DCompileFromFile(shader_path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                       entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
            if (FAILED(result)) {
                const std::string detail = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "Shader file unavailable";
                throw std::runtime_error("Scene shader compilation failed: " + detail);
            }
        };
        compile_shader("vertex_main", "vs_5_0", vertex_shader);
        compile_shader("pixel_main", "ps_5_0", pixel_shader);
        D3D12_ROOT_PARAMETER parameters{};
        parameters.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters.Constants.ShaderRegister = 0;
        parameters.Constants.Num32BitValues = 3;
        parameters.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = 1;
        root_description.pParameters = &parameters;
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized_root;
        check(D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized_root, &errors), "Serialize scene root signature");
        check(device_->CreateRootSignature(0, serialized_root->GetBufferPointer(), serialized_root->GetBufferSize(), IID_PPV_ARGS(&scene_root_signature_)), "Create scene root signature");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = scene_root_signature_.Get();
        pipeline.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
        pipeline.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.StencilEnable = FALSE;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        check(device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&scene_pipeline_)), "Create scene graphics pipeline");
    }

    void wait_for_fence(std::uint64_t value) {
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) throw std::runtime_error("D3D12 device was removed");
        if (value == 0 || completed >= value) return;
        check(fence_->SetEventOnCompletion(value, fence_completion_event_.get()), "Set fence event");
        if (WaitForSingleObject(fence_completion_event_.get(), INFINITE) != WAIT_OBJECT_0)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for GPU");
        if (fence_->GetCompletedValue() == UINT64_MAX) throw std::runtime_error("D3D12 device was removed");
    }
    void create_render_targets() {
        auto handle = render_target_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < swap_chain_buffer_count; ++i) {
            check(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&render_targets_[i])), "Get swap-chain buffer");
            device_->CreateRenderTargetView(render_targets_[i].Get(), nullptr, handle);
            handle.ptr += render_target_descriptor_stride_;
        }
    }

    Handle fence_completion_event_;
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;
    ComPtr<ID3D12DescriptorHeap> render_target_heap_;
    std::array<ComPtr<ID3D12Resource>, swap_chain_buffer_count> render_targets_;
    std::array<ComPtr<ID3D12CommandAllocator>, swap_chain_buffer_count> command_allocators_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> scene_root_signature_;
    ComPtr<ID3D12PipelineState> scene_pipeline_;
    std::array<std::uint64_t, swap_chain_buffer_count> buffer_fence_values_{};
    std::uint64_t next_fence_value_ = 1;
    UINT render_target_descriptor_stride_ = 0;
    UINT viewport_width_ = 0;
    UINT viewport_height_ = 0;
};

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM message_parameter, LPARAM message_data) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, message_parameter, message_data);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        const wchar_t* class_name = frame_warp_lab::window_class_name;
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Register window class");
        HWND window = CreateWindowExW(0, class_name, L"Frame Warp Lab - Motion Lab", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, instance, nullptr);
        if (!window) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create window");
        Renderer renderer(window);
        RuntimeStatePublisher runtime_state;
        ShowWindow(window, show);
        RECT previous_client_rectangle{};
        GetClientRect(window, &previous_client_rectangle);
        MSG message{};
        bool running = true;
        bool scene_paused = false;
        double scene_time_seconds = 0;
        auto previous_tick = std::chrono::steady_clock::now();
        while (running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { running = false; break; }
                if (message.message == frame_warp_lab::scene_control_message) {
                    if (message.wParam == frame_warp_lab::scene_pause) scene_paused = true;
                    if (message.wParam == frame_warp_lab::scene_resume) scene_paused = false;
                    if (message.wParam == frame_warp_lab::scene_reset) scene_time_seconds = 0;
                    previous_tick = std::chrono::steady_clock::now();
                    runtime_state.update_scene(static_cast<float>(scene_time_seconds), scene_paused);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;
            if (IsIconic(window)) { WaitMessage(); previous_tick = std::chrono::steady_clock::now(); continue; }
            const auto current_tick = std::chrono::steady_clock::now();
            if (!scene_paused) scene_time_seconds += std::chrono::duration<double>(current_tick - previous_tick).count();
            previous_tick = current_tick;
            RECT current_client_rectangle{};
            if (!GetClientRect(window, &current_client_rectangle)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Get client size");
            if (current_client_rectangle.right != previous_client_rectangle.right || current_client_rectangle.bottom != previous_client_rectangle.bottom) {
                renderer.resize(static_cast<UINT>(current_client_rectangle.right), static_cast<UINT>(current_client_rectangle.bottom));
                runtime_state.record_resize();
                previous_client_rectangle = current_client_rectangle;
            }
            renderer.draw(static_cast<float>(scene_time_seconds));
            runtime_state.update_scene(static_cast<float>(scene_time_seconds), scene_paused);
            runtime_state.record_present();
        }
        renderer.flush();
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Frame Warp Lab error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
