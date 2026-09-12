#include <gtest/gtest.h>
#include <windows.h>
#include <RmlUi/Core/Input.h>
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlSystemInterfaceWin32.hpp"
#include <d3d11.h>
#include <wrl/client.h>

#include "ui/rml/RmlRenderInterfaceD3D11.hpp"

TEST(RmlInputWin32Test, ConvertKey_MapsStandardAndExtendedKeys) {
    EXPECT_EQ(RmlInputWin32::ConvertKey('A'), Rml::Input::KI_A);
    EXPECT_EQ(RmlInputWin32::ConvertKey('Z'), Rml::Input::KI_Z);
    EXPECT_EQ(RmlInputWin32::ConvertKey('0'), Rml::Input::KI_0);
    EXPECT_EQ(RmlInputWin32::ConvertKey('9'), Rml::Input::KI_9);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_ESCAPE), Rml::Input::KI_ESCAPE);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_RETURN), Rml::Input::KI_RETURN);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_TAB), Rml::Input::KI_TAB);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_BACK), Rml::Input::KI_BACK);

    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_F1), Rml::Input::KI_F1);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_F12), Rml::Input::KI_F12);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_F13), Rml::Input::KI_F13);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_F24), Rml::Input::KI_F24);

    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_SNAPSHOT), Rml::Input::KI_SNAPSHOT);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_APPS), Rml::Input::KI_APPS);
    EXPECT_EQ(RmlInputWin32::ConvertKey(VK_OEM_102), Rml::Input::KI_OEM_102);

    const auto shiftKey = RmlInputWin32::ConvertKey(VK_SHIFT);
    EXPECT_TRUE(shiftKey == Rml::Input::KI_LSHIFT || shiftKey == Rml::Input::KI_RSHIFT);

    const auto ctrlKey = RmlInputWin32::ConvertKey(VK_CONTROL);
    EXPECT_TRUE(ctrlKey == Rml::Input::KI_LCONTROL || ctrlKey == Rml::Input::KI_RCONTROL);

    const auto altKey = RmlInputWin32::ConvertKey(VK_MENU);
    EXPECT_TRUE(altKey == Rml::Input::KI_LMENU || altKey == Rml::Input::KI_RMENU);
}

// Every res:// image the UI references must resolve through the renderer's
// resource mapping. A missing mapping renders as an opaque white quad instead
// of the asset, which is how the chevron affordance silently disappeared.
TEST(RmlRenderInterfaceD3D11Test, ResolvesEveryReferencedImageAsset) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }

    RmlRenderInterfaceD3D11 renderer;
    ASSERT_TRUE(renderer.Initialize(device.Get(), context.Get(), 800, 600));
    for (const char* source : {"res://images/chevron.png", "res://images/Logo.png",
                               "res://images/Tiers/0.png", "res://images/Tiers/22.png",
                               "res://images/Divisions/0.png", "res://images/Playlists/0.png"}) {
        Rml::Vector2i dimensions{};
        const Rml::TextureHandle handle = renderer.LoadTexture(dimensions, source);
        EXPECT_NE(handle, Rml::TextureHandle(0)) << source;
        EXPECT_GT(dimensions.x, 0) << source;
        EXPECT_GT(dimensions.y, 0) << source;
        if (handle) renderer.ReleaseTexture(handle);
    }
    renderer.Shutdown();
}
