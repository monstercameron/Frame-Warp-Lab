#pragma once

#include <d3d12.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace frame_warp_lab {

class RenderGraphContext {
public:
    RenderGraphContext() {
        tracked_.reserve(24);
        pending_.reserve(24);
    }

    void reset() {
        tracked_.clear();
        pending_.clear();
    }

    void track(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial_state) {
        if (resource == nullptr) return;
        for (auto& entry : tracked_) {
            if (entry.resource == resource) {
                entry.state = initial_state;
                return;
            }
        }
        tracked_.push_back({resource, initial_state});
    }

    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES target_state) {
        if (resource == nullptr) return;
        for (auto& entry : tracked_) {
            if (entry.resource == resource) {
                if (entry.state != target_state) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    b.Transition.pResource = resource;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = entry.state;
                    b.Transition.StateAfter = target_state;
                    pending_.push_back(b);
                    entry.state = target_state;
                }
                return;
            }
        }
    }

    void uav(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.UAV.pResource = resource;
        pending_.push_back(b);
    }

    void flush_barriers(ID3D12GraphicsCommandList* command_list) {
        if (pending_.empty() || command_list == nullptr) return;
        command_list->ResourceBarrier(static_cast<UINT>(pending_.size()), pending_.data());
        pending_.clear();
    }

    template <typename SetupFn, typename ExecFn>
    void execute_pass(ID3D12GraphicsCommandList* command_list, SetupFn&& setup, ExecFn&& exec) {
        setup(*this);
        flush_barriers(command_list);
        exec();
    }

    std::size_t tracked_count() const { return tracked_.size(); }

private:
    struct TrackedResource {
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES state;
    };

    std::vector<TrackedResource> tracked_;
    std::vector<D3D12_RESOURCE_BARRIER> pending_;
};

}
