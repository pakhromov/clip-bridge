#pragma once
#include "protocols/ext-data-control.h"
#include "protocols/wlr-data-control.h"
#include <algorithm>
#include <string>
#include <vector>

struct DeviceEvents {
    virtual void newOffer(void*) = 0;
    virtual void selection(void*, bool primary) = 0;
    virtual void finished() = 0;
    virtual ~DeviceEvents() = default;
};
struct SourceEvents {
    virtual void send(const char* mime, int fd) = 0;
    virtual void cancelled() = 0;
    virtual ~SourceEvents() = default;
};

struct Protocol {
    bool ext = false;
    uint32_t version = 0;
    void* manager = nullptr;
    void* device = nullptr;
    void bind(wl_registry* registry, uint32_t name, uint32_t advertised, bool useExt) {
        ext = useExt; version = std::min(advertised, ext ? 1u : 2u);
        manager = wl_registry_bind(registry, name,
            ext ? &ext_data_control_manager_v1_interface : &zwlr_data_control_manager_v1_interface, version);
    }
    bool primarySupported() const { return ext || version >= 2; }
    void getDevice(wl_seat* seat, DeviceEvents* events) {
        if (ext) {
            auto d = ext_data_control_manager_v1_get_data_device(static_cast<ext_data_control_manager_v1*>(manager), seat);
            static const ext_data_control_device_v1_listener listener = {
                .data_offer = [](void* p, auto*, auto* offer) { static_cast<DeviceEvents*>(p)->newOffer(offer); },
                .selection = [](void* p, auto*, auto* offer) { static_cast<DeviceEvents*>(p)->selection(offer, false); },
                .finished = [](void* p, auto*) { static_cast<DeviceEvents*>(p)->finished(); },
                .primary_selection = [](void* p, auto*, auto* offer) { static_cast<DeviceEvents*>(p)->selection(offer, true); },
            };
            ext_data_control_device_v1_add_listener(d, &listener, events); device = d;
        } else {
            auto d = zwlr_data_control_manager_v1_get_data_device(static_cast<zwlr_data_control_manager_v1*>(manager), seat);
            static const zwlr_data_control_device_v1_listener listener = {
                .data_offer = [](void* p, auto*, auto* offer) { static_cast<DeviceEvents*>(p)->newOffer(offer); },
                .selection = [](void* p, auto*, auto* offer) { static_cast<DeviceEvents*>(p)->selection(offer, false); },
                .finished = [](void* p, auto*) { static_cast<DeviceEvents*>(p)->finished(); },
                .primary_selection = [](void* p, auto*, auto* offer) { static_cast<DeviceEvents*>(p)->selection(offer, true); },
            };
            zwlr_data_control_device_v1_add_listener(d, &listener, events); device = d;
        }
    }
    void listenOffer(void* raw, std::vector<std::string>* mimes) const {
        if (ext) {
            static const ext_data_control_offer_v1_listener listener = {
                .offer = [](void* p, auto*, const char* mime) {
                    auto& list = *static_cast<std::vector<std::string>*>(p);
                    if (std::ranges::find(list, mime) == list.end()) list.emplace_back(mime);
                },
            };
            ext_data_control_offer_v1_add_listener(static_cast<ext_data_control_offer_v1*>(raw), &listener, mimes);
        } else {
            static const zwlr_data_control_offer_v1_listener listener = {
                .offer = [](void* p, auto*, const char* mime) {
                    auto& list = *static_cast<std::vector<std::string>*>(p);
                    if (std::ranges::find(list, mime) == list.end()) list.emplace_back(mime);
                },
            };
            zwlr_data_control_offer_v1_add_listener(static_cast<zwlr_data_control_offer_v1*>(raw), &listener, mimes);
        }
    }
    void destroyOffer(void* raw) const {
        if (ext) ext_data_control_offer_v1_destroy(static_cast<ext_data_control_offer_v1*>(raw));
        else zwlr_data_control_offer_v1_destroy(static_cast<zwlr_data_control_offer_v1*>(raw));
    }
    void receive(void* raw, const char* mime, int fd) const {
        if (ext) ext_data_control_offer_v1_receive(static_cast<ext_data_control_offer_v1*>(raw), mime, fd);
        else zwlr_data_control_offer_v1_receive(static_cast<zwlr_data_control_offer_v1*>(raw), mime, fd);
    }
    void* createSource(SourceEvents* events) const {
        if (ext) {
            auto s = ext_data_control_manager_v1_create_data_source(static_cast<ext_data_control_manager_v1*>(manager));
            static const ext_data_control_source_v1_listener listener = {
                .send = [](void* p, auto*, const char* mime, int fd) { static_cast<SourceEvents*>(p)->send(mime, fd); },
                .cancelled = [](void* p, auto*) { static_cast<SourceEvents*>(p)->cancelled(); },
            };
            ext_data_control_source_v1_add_listener(s, &listener, events); return s;
        }
        auto s = zwlr_data_control_manager_v1_create_data_source(static_cast<zwlr_data_control_manager_v1*>(manager));
        static const zwlr_data_control_source_v1_listener listener = {
            .send = [](void* p, auto*, const char* mime, int fd) { static_cast<SourceEvents*>(p)->send(mime, fd); },
            .cancelled = [](void* p, auto*) { static_cast<SourceEvents*>(p)->cancelled(); },
        };
        zwlr_data_control_source_v1_add_listener(s, &listener, events); return s;
    }
    void offer(void* source, const char* mime) const {
        if (ext) ext_data_control_source_v1_offer(static_cast<ext_data_control_source_v1*>(source), mime);
        else zwlr_data_control_source_v1_offer(static_cast<zwlr_data_control_source_v1*>(source), mime);
    }
    void set(void* source, bool primary) const {
        if (ext) {
            auto d = static_cast<ext_data_control_device_v1*>(device);
            auto s = static_cast<ext_data_control_source_v1*>(source);
            if (primary) ext_data_control_device_v1_set_primary_selection(d, s);
            else ext_data_control_device_v1_set_selection(d, s);
        } else {
            auto d = static_cast<zwlr_data_control_device_v1*>(device);
            auto s = static_cast<zwlr_data_control_source_v1*>(source);
            if (primary) zwlr_data_control_device_v1_set_primary_selection(d, s);
            else zwlr_data_control_device_v1_set_selection(d, s);
        }
    }
    void destroySource(void* source) const {
        if (ext) ext_data_control_source_v1_destroy(static_cast<ext_data_control_source_v1*>(source));
        else zwlr_data_control_source_v1_destroy(static_cast<zwlr_data_control_source_v1*>(source));
    }
    void reset() {
        if (device) {
            if (ext) ext_data_control_device_v1_destroy(static_cast<ext_data_control_device_v1*>(device));
            else zwlr_data_control_device_v1_destroy(static_cast<zwlr_data_control_device_v1*>(device));
            device = nullptr;
        }
        if (manager) {
            if (ext) ext_data_control_manager_v1_destroy(static_cast<ext_data_control_manager_v1*>(manager));
            else zwlr_data_control_manager_v1_destroy(static_cast<zwlr_data_control_manager_v1*>(manager));
            manager = nullptr;
        }
    }
};
