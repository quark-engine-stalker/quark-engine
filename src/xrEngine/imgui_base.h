#pragma once

#include "IInputReceiver.h"
#include "../Include/xrRender/ImGuiRender.h"

#define IMGUI_DISABLE_OBSOLETE_KEYIO
struct ImGuiContext;
struct ImFont;
struct ImFontConfig;

namespace xr_imgui
{
    struct ide_backend;

    class ide :
        public pureRender,
        public pureFrame,
        public pureAppActivate,
        public pureAppDeactivate,
        public pureAppStart,
        public pureAppEnd,
        public pureScreenResolutionChanged,
        public IInputReceiver
    {
    public:
        ide();
        ~ide();

        bool is_shown() const { return m_shown; }
        void Show(bool bShow = true);
        bool is_input() const { return m_input; }
        void EnableInput(bool bInput = true);
        // quark_panel: opens the window with the mouse, gives the mouse back to it, closes it when it has the mouse
        void ShowMTPanel();
        bool is_mt_panel_shown() const { return m_shown && m_mt_panel; }

    public:
        void OnDeviceCreate();
        void OnDeviceDestroy();
        void OnDeviceResetBegin() const;
        void OnDeviceResetEnd() const;

    public:
        // Interface implementations
        void OnFrame() final;
        void OnRender() final;

        void OnAppActivate() final;
        void OnAppDeactivate() final;

        void OnAppStart() final;
        void OnAppEnd() final;

        virtual void OnScreenResolutionChanged();

        virtual void IR_Capture();
        virtual void IR_Release();

        void IR_OnMousePress(int key) final;
        void IR_OnMouseRelease(int key) final;
        void IR_OnMouseWheel(int direction) final;
        void IR_OnMouseMove(int x, int y) final;

        void IR_OnKeyboardPress(int key) final;
        void IR_OnKeyboardRelease(int key) final;

        // ImGui handles hold state on its own
        void IR_OnMouseHold(int key) final {};
        void IR_OnKeyboardHold(int key) final {};

        void InputChar(WPARAM param);
        void UpdateInputLang();

        // QUARK-PANEL's images (ImTextureID), decoded at the first use and made anew with the device
        enum panel_image { panel_logo, panel_title, panel_image_count };
        void* PanelImage(panel_image which, u32& width, u32& height);

        ImFont* GetFont(LPCSTR name);
        ImFontConfig LoadImGuiFontConfig(string_path path, LPCSTR name);
        void LoadImGuiFont(string_path path, LPCSTR name);

    private:
        void InitBackend();
        void ShutdownBackend();

    private:
        void ShowMain();

    private:
        CTimer m_timer;
        IImGuiRender* m_render;
        ImGuiContext* m_context;
        ide_backend* m_backend_data;
        bool m_shown;
        bool m_input;
        bool m_mt_panel; // shown is the mt_panel window, not the editor
        bool firstframe;
        UINT32 keyboard_code_page;
        xr_vector<IReader*> ImGuiFontsPtr;
        xr_map<shared_str, ImFont*> ImFonts;
        struct panel_texture
        {
            void* texture;
            u32 width, height;
            bool tried;
        } m_panel_images[panel_image_count] = {};
    };

    // Every frame, open or not: the window counts the parallel tasks only while it is open (mt_panel.cpp).
    void mt_panel_frame(ide& owner, bool open);
} // namespace xr_imgui