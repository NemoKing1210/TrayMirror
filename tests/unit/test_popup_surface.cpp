// The overflow flyout's material decision (plan task 5): acrylic must
// never be half-applied. The surface transmits the background only when
// transparency is enabled, high contrast is off and both the accent API
// and the composition stack came up; every other combination is the
// intentional opaque system-colour fallback.
#include "render/NativePopupSurface.h"

#include <windows.h>

#include <gtest/gtest.h>

namespace {

using traymirror::PopupEnvironment;
using traymirror::PopupMaterial;
using traymirror::ResolvePopupMaterial;

PopupEnvironment FullEnvironment() {
  return PopupEnvironment{.transparencyEnabled = true,
                          .highContrast = false,
                          .acrylicAvailable = true,
                          .compositionAvailable = true};
}

}  // namespace

TEST(PopupMaterial, AcrylicWhenEverythingIsAvailable) {
  EXPECT_EQ(ResolvePopupMaterial(FullEnvironment()), PopupMaterial::kAcrylic);
}

TEST(PopupMaterial, TransparencySettingDisabledFallsBackToOpaque) {
  PopupEnvironment env = FullEnvironment();
  env.transparencyEnabled = false;
  EXPECT_EQ(ResolvePopupMaterial(env), PopupMaterial::kOpaque);
}

TEST(PopupMaterial, HighContrastFallsBackToOpaque) {
  PopupEnvironment env = FullEnvironment();
  env.highContrast = true;
  EXPECT_EQ(ResolvePopupMaterial(env), PopupMaterial::kOpaque);
}

TEST(PopupMaterial, AccentApiFailureFallsBackToOpaque) {
  PopupEnvironment env = FullEnvironment();
  env.acrylicAvailable = false;
  EXPECT_EQ(ResolvePopupMaterial(env), PopupMaterial::kOpaque);
}

TEST(PopupMaterial, CompositionFailureFallsBackToOpaque) {
  PopupEnvironment env = FullEnvironment();
  env.compositionAvailable = false;
  EXPECT_EQ(ResolvePopupMaterial(env), PopupMaterial::kOpaque);
}

TEST(PopupMaterial, TintMatchesTheMeasuredWin11SurfaceColours) {
  // ABGR; both measured surfaces are neutral, so the byte order is not
  // observable here, but the alpha and level are.
  EXPECT_EQ(traymirror::PopupAcrylicTint(true), 0xCC202020U);
  EXPECT_EQ(traymirror::PopupAcrylicTint(false), 0xCCF3F3F3U);
}

TEST(PopupMaterial, TransparencyFlagMatchesThePersonalisationSetting) {
  DWORD value = 1;
  DWORD size = sizeof(value);
  const bool present =
      RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                   L"Personalize",
                   L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value,
                   &size) == ERROR_SUCCESS;
  const bool expected = !present || value != 0;
  EXPECT_EQ(traymirror::SystemTransparencyEnabled(), expected);
}
