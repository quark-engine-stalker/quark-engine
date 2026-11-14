#include "stdafx.h"
#include "dxImGuiRender.h"

#include <imgui.h>

#include <backends/imgui_impl_dx11.h>

void dxImGuiRender::Copy(IImGuiRender& _in)
{
    *this = *fast_dynamic_cast<dxImGuiRender*>(&_in);
}

void dxImGuiRender::SetState(ImDrawData* data)
{
    D3D_VIEWPORT VP = { 0, 0, data->DisplaySize.x, data->DisplaySize.y, 0, 1.f };
    HW.pContext->RSSetViewports(1, &VP);

    // Setup shader and vertex buffers
    /*unsigned int stride = sizeof(ImDrawVert);
    unsigned int offset = 0;
    ctx->IASetInputLayout(bd->pInputLayout);
    ctx->IASetVertexBuffers(0, 1, &bd->pVB, &stride, &offset);
    ctx->IASetIndexBuffer(bd->pIB, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(bd->pVertexShader, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &bd->pVertexConstantBuffer);
    ctx->PSSetShader(bd->pPixelShader, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &bd->pFontSampler);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..
    ctx->DSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..
    ctx->CSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..

    // Setup blend state
    const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
    ctx->OMSetBlendState(bd->pBlendState, blend_factor, 0xffffffff);
    ctx->OMSetDepthStencilState(bd->pDepthStencilState, 0);
    ctx->RSSetState(bd->pRasterizerState);*/
}

void dxImGuiRender::Frame()
{
    ImGui_ImplDX11_NewFrame();
}

void dxImGuiRender::Render(ImDrawData* data)
{
    ImGui_ImplDX11_RenderDrawData(data);
}

void* dxImGuiRender::CreateTexture(const void* rgba, unsigned width, unsigned height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data = {rgba, width * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, &data, &texture)))
        HW.pDevice->CreateShaderResourceView(texture, nullptr, &view);
    _RELEASE(texture);
    if (view)
        m_textures.push_back(view);
    return view;
}

void dxImGuiRender::OnDeviceCreate(ImGuiContext* context)
{
    ImGui::SetAllocatorFunctions(
        [](size_t size, void* /*user_data*/)
        {
            return xr_malloc(size);
        },
        [](void* ptr, void* /*user_data*/)
        {
            xr_free(ptr);
        }
    );
    ImGui::SetCurrentContext(context);

    ImGui_ImplDX11_Init(HW.pDevice, HW.pContext);
}
void dxImGuiRender::OnDeviceDestroy()
{
    for (ID3D11ShaderResourceView*& view : m_textures)
        _RELEASE(view);
    m_textures.clear();
    ImGui_ImplDX11_Shutdown();
}

void dxImGuiRender::OnDeviceResetBegin()
{
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

void dxImGuiRender::OnDeviceResetEnd()
{
    ImGui_ImplDX11_CreateDeviceObjects();
}
