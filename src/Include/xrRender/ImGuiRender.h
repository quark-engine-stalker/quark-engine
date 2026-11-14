#pragma once

struct ImGuiContext;
struct ImDrawData;

class IImGuiRender
{
public:
    virtual ~IImGuiRender() = 0;
    virtual void Copy(IImGuiRender& _in) = 0;

    virtual void Frame() = 0;
    virtual void Render(ImDrawData* data) = 0;
    // An ImGui texture (ImTextureID) of 32-bit RGBA rows; nullptr on failure; released in OnDeviceDestroy
    virtual void* CreateTexture(const void* rgba, unsigned width, unsigned height) = 0;

    virtual void OnDeviceCreate(ImGuiContext* context) = 0;
    virtual void OnDeviceDestroy() = 0;
    virtual void OnDeviceResetBegin() = 0;
    virtual void OnDeviceResetEnd() = 0;
};

inline IImGuiRender::~IImGuiRender() = default;