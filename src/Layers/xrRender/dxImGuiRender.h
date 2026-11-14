#pragma once

#include "../Include/xrRender/ImGuiRender.h"

class dxImGuiRender : public IImGuiRender
{
    void SetState(ImDrawData* data);

public:
    void Copy(IImGuiRender& _in) override;

    void Frame() override;
    void Render(ImDrawData* data) override;
    void* CreateTexture(const void* rgba, unsigned width, unsigned height) override;

    void OnDeviceCreate(ImGuiContext* context) override;
    void OnDeviceDestroy() override;
    void OnDeviceResetBegin() override;
    void OnDeviceResetEnd() override;

private:
    xr_vector<ID3D11ShaderResourceView*> m_textures;
};