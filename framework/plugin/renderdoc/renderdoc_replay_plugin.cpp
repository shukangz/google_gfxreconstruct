/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gfxr/replay_event_plugin.h>
#include <util/logging.h>
#include <dlfcn.h>
#include <string>
#include <cstdlib>
#include "renderdoc_app.h"

struct RenderDocCapturePlugin
{
    GfxrReplayPluginV1 base;
    RENDERDOC_API_1_4_0* rdoc_api = nullptr;
    bool capture_started = false;
    std::string renderdoc_lib_path;
    uint64_t target_frame = 0;
    uint64_t first_frame = 0;
    bool first_frame_initialized = false;
    uint64_t captured_frame_index = 0;
};

static bool load_renderdoc_api(RenderDocCapturePlugin* plugin)
{
    if (plugin->rdoc_api != nullptr)
    {
        return true;
    }

    pRENDERDOC_GetAPI rdoc_get_api = nullptr;

    // Try loading custom library path first if provided
    if (!plugin->renderdoc_lib_path.empty())
    {
        GFXRECON_LOG_INFO("Attempting to load RenderDoc from custom path: %s", plugin->renderdoc_lib_path.c_str());
        void* mod = dlopen(plugin->renderdoc_lib_path.c_str(), RTLD_NOW);
        if (mod != nullptr)
        {
            rdoc_get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
        }
        else
        {
            GFXRECON_LOG_WARNING("Failed to dlopen custom RenderDoc path: %s, error: %s", 
                                 plugin->renderdoc_lib_path.c_str(), dlerror());
        }
    }

    if (rdoc_get_api == nullptr)
    {
        // Try RTLD_DEFAULT first since RenderDoc layer might be globally loaded
        rdoc_get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"));
    }

    if (rdoc_get_api == nullptr)
    {
        // Try opening the default libs
        void* mod = dlopen("libVkLayer_renderdoc.so", RTLD_NOW);
        if (mod == nullptr)
        {
            mod = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW);
        }
        if (mod == nullptr)
        {
            mod = dlopen("librenderdoc.so", RTLD_NOW);
        }
        if (mod != nullptr)
        {
            rdoc_get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
        }
    }

    if (rdoc_get_api != nullptr)
    {
        int ret = rdoc_get_api(eRENDERDOC_API_Version_1_4_0, reinterpret_cast<void**>(&plugin->rdoc_api));
        if (ret == 1)
        {
            GFXRECON_LOG_INFO("Successfully loaded RenderDoc API on-demand");
            plugin->rdoc_api->SetCaptureFilePathTemplate("/sdcard/Download/sherlock_renderdoc_capture");
            return true;
        }
        else
        {
            GFXRECON_LOG_ERROR("Failed to get RenderDoc API pointer on-demand");
            plugin->rdoc_api = nullptr;
        }
    }

    return false;
}

static void ensure_api_loaded_and_start_capture(RenderDocCapturePlugin* plugin, uint32_t event_type, uint64_t frame_index)
{
    if (plugin->capture_started)
    {
        return;
    }

    if (plugin->rdoc_api == nullptr)
    {
        load_renderdoc_api(plugin);
    }

    if (plugin->rdoc_api != nullptr)
    {
        GFXRECON_LOG_INFO("Starting RenderDoc capture on event %u (frame_index: %llu)...", event_type, frame_index);
        plugin->rdoc_api->StartFrameCapture(nullptr, nullptr);
        plugin->capture_started = true;
    }
}

static void destroy(GfxrReplayPluginV1* self)
{
    if (self == NULL)
    {
        GFXRECON_LOG_ERROR("Received NULL plugin instance");
        return;
    }

    RenderDocCapturePlugin* plugin = reinterpret_cast<RenderDocCapturePlugin*>(self);
    if (plugin->rdoc_api != nullptr && plugin->capture_started)
    {
        GFXRECON_LOG_INFO("Stopping RenderDoc capture in destroy...");
        int result = plugin->rdoc_api->EndFrameCapture(nullptr, nullptr);
        GFXRECON_LOG_INFO("RenderDoc capture stopped in destroy. Result: %d", result);
    }

    GFXRECON_LOG_INFO("Destroying RenderDoc capture plugin");
    delete plugin;
}

static GfxrReplayPluginResult on_event(GfxrReplayPluginV1* self, const GfxrReplayEventHeader* event)
{
    if (self == NULL || event == NULL)
    {
        return GFXR_REPLAY_PLUGIN_RESULT_ERROR;
    }

    RenderDocCapturePlugin* plugin = reinterpret_cast<RenderDocCapturePlugin*>(self);

    if (event->type == GFXR_REPLAY_EVENT_STATE_LOADING_COMPLETE)
    {
        plugin->first_frame = event->frame_index;
        plugin->first_frame_initialized = true;

        if (plugin->target_frame == 0 || plugin->target_frame == plugin->first_frame)
        {
            ensure_api_loaded_and_start_capture(plugin, event->type, event->frame_index);
            if (plugin->capture_started)
            {
                plugin->captured_frame_index = event->frame_index;
            }
        }
    }
    else if (event->type == GFXR_REPLAY_EVENT_FRAME_BEGIN)
    {
        if (plugin->target_frame != 0 && event->frame_index == plugin->target_frame)
        {
            ensure_api_loaded_and_start_capture(plugin, event->type, event->frame_index);
            if (plugin->capture_started)
            {
                plugin->captured_frame_index = event->frame_index;
            }
        }
    }
    else if (event->type == GFXR_REPLAY_EVENT_FRAME_END)
    {
        if (plugin->capture_started && event->frame_index == plugin->captured_frame_index && plugin->rdoc_api != nullptr)
        {
            GFXRECON_LOG_INFO("Stopping RenderDoc capture on FRAME_END (frame_index: %llu)...", event->frame_index);
            int result = plugin->rdoc_api->EndFrameCapture(nullptr, nullptr);
            GFXRECON_LOG_INFO("RenderDoc capture stopped. Result: %d", result);
            plugin->capture_started = false;
        }
    }

    return GFXR_REPLAY_PLUGIN_RESULT_OK;
}

GFXR_REPLAY_PLUGIN_EXPORT GfxrReplayPluginV1* gfxrCreateReplayPluginV1(const GfxrReplayPluginCreateInfo* create_info)
{
    if (create_info == NULL)
    {
        GFXRECON_LOG_ERROR("Create info is NULL");
        return NULL;
    }

    if (create_info->abi_version != GFXR_REPLAY_PLUGIN_ABI_VERSION)
    {
        GFXRECON_LOG_ERROR("Unsupported plugin ABI version %u", create_info->abi_version);
        return NULL;
    }

    if (create_info->struct_size != sizeof(GfxrReplayPluginCreateInfo))
    {
        GFXRECON_LOG_ERROR("Unexpected create info struct size %u", create_info->struct_size);
        return NULL;
    }

    gfxrecon::util::Log::Init();
    GFXRECON_LOG_INFO("Creating RenderDoc capture plugin");

    RenderDocCapturePlugin* plugin = new RenderDocCapturePlugin;
    plugin->base.abi_version   = GFXR_REPLAY_PLUGIN_ABI_VERSION;
    plugin->base.struct_size   = sizeof(GfxrReplayPluginV1);
    plugin->base.destroy       = destroy;
    plugin->base.on_event      = on_event;
    plugin->capture_started    = false;

    if (create_info->plugin_params != nullptr && strlen(create_info->plugin_params) > 0)
    {
        std::string params(create_info->plugin_params);
        size_t semi = params.find(';');
        if (semi != std::string::npos)
        {
            plugin->renderdoc_lib_path = params.substr(0, semi);
            std::string extra = params.substr(semi + 1);
            size_t eq = extra.find("frame=");
            if (eq != std::string::npos)
            {
                std::string val_str = extra.substr(eq + 6);
                plugin->target_frame = std::strtoull(val_str.c_str(), nullptr, 10);
            }
        }
        else
        {
            plugin->renderdoc_lib_path = params;
            plugin->target_frame = 0;
        }
        GFXRECON_LOG_INFO("RenderDoc plugin configured with custom library path: %s, target_frame: %llu", 
                          plugin->renderdoc_lib_path.c_str(), plugin->target_frame);
    }

    return &plugin->base;
}
