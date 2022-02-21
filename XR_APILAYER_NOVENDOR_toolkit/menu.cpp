// MIT License
//
// Copyright(c) 2021-2022 Matthieu Bucchianeri
// Copyright(c) 2021-2022 Jean-Luc Dupiot - Reality XP
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "layer.h"
#include "factories.h"
#include "interfaces.h"
#include "log.h"

namespace {

    using namespace toolkit;
    using namespace toolkit::config;
    using namespace toolkit::graphics;
    using namespace toolkit::input;
    using namespace toolkit::menu;
    using namespace toolkit::log;
    using namespace toolkit::utilities;

    using namespace xr::math;

    constexpr auto KeyRepeatDelay = 200ms;

    // Utility macros/functions for color manipulation.
#define COLOR_TO_TEXT_COLOR(color)                                                                                     \
    ((((uint8_t)((color.r) * 255)) << 0) | (((uint8_t)((color.g) * 255)) << 8) | (((uint8_t)((color.b) * 255)) << 16))

    constexpr XrColor4f sRGBToLinear(uint8_t r, uint8_t g, uint8_t b) {
        auto sRGBToLinearComponent = [](float x) {
            if (x <= 0.0f)
                return 0.0f;
            else if (x >= 1.0f)
                return 1.0f;
            else if (x < 0.04045f)
                return x / 12.92f;
            else
                return std::pow((x + 0.055f) / 1.055f, 2.4f);
        };

        return XrColor4f({sRGBToLinearComponent(r / 255.f),
                          sRGBToLinearComponent(g / 255.f),
                          sRGBToLinearComponent(b / 255.f),
                          1.f});
    }

    // Text colors
    const XrColor4f ColorWhite = sRGBToLinear(255, 255, 255);
    const XrColor4f ColorOverlay = sRGBToLinear(247, 198, 20);
    const XrColor4f ColorHint = sRGBToLinear(163, 163, 163);
    const XrColor4f ColorWarning = sRGBToLinear(255, 0, 0);
    const XrColor4f ColorHighlight = sRGBToLinear(88, 67, 98);
    const XrColor4f ColorSelected = sRGBToLinear(119, 114, 188);

    // Shape colors.
    const XrColor4f ColorBackground = sRGBToLinear(40, 44, 50);
    const XrColor4f ColorHeader = sRGBToLinear(60, 63, 73);
    const XrColor4f ColorHeaderSeparator = sRGBToLinear(120, 126, 145);
    constexpr float HeaderLineWeight = 1.f;

    // Spacing and indentation.
    constexpr float BorderHorizontalSpacing = 20.f;
    constexpr float BorderVerticalSpacing = 10.f;
    constexpr uint32_t OptionSpacing = 30;
    constexpr uint32_t ValueSpacing = 20;
    constexpr uint32_t SelectionHorizontalSpacing = 8;
    constexpr uint32_t SelectionVerticalSpacing = 5;
    const uint32_t OptionIndent = 0;
    const uint32_t SubGroupIndent = 20;

    enum class MenuState { Splash, NotVisible, Visible };
    enum class MenuEntryType { Tabs, Slider, Choice, Separator, RestoreDefaultsButton, ExitButton };

    struct MenuEntry {
        uint32_t indent;
        std::string title;
        MenuEntryType type;
#define BUTTON_OR_SEPARATOR "", 0, 0, [](int value) { return ""; }
        std::string configName;
        int minValue;
        int maxValue;
        std::function<std::string(int)> valueToString;

        // Steps for changing the value.
        int acceleration{1};

        // Some settings might require exiting VR immediately to test the change. Bypass the commit delay when true.
        bool noCommitDelay{false};

        // Some entries are not attached to config values. Use a direct pointer in this case.
        int* pValue{nullptr};

        bool visible{false};
    };

    class MenuGroup {
      public:
        MenuGroup(std::vector<MenuGroup>& menuGroups,
                  std::vector<MenuEntry>& menuEntries,
                  std::function<bool()> isVisible,
                  bool isTab = false)
            : m_menuGroups(&menuGroups), m_menuEntries(&menuEntries), m_isVisible(isVisible),
              m_start(menuEntries.size()), m_end(menuEntries.size()), m_isTab(isTab) {
        }

        MenuGroup& operator=(const MenuGroup&) = default;

        void finalize() {
            m_end = m_menuEntries->size();

            if (!m_isTab) {
                m_menuGroups->push_back(*this);
            } else {
                // Tabs must always be evaluated first.
                m_menuGroups->insert(m_menuGroups->begin(), *this);
            }
        }

        void updateVisibility() const {
            for (size_t i = m_start; i < m_end; i++) {
                (*m_menuEntries)[i].visible = (m_isTab || (*m_menuEntries)[i].visible) && m_isVisible();
            }
        };

      private:
        std::vector<MenuGroup>* m_menuGroups;
        std::vector<MenuEntry>* m_menuEntries;
        std::function<bool()> m_isVisible;
        size_t m_start;
        size_t m_end;

        bool m_isTab;
    };

    enum class MenuTab { Performance = 0, Appearance, Inputs, Menu };

    // The logic of our menus.
    class MenuHandler : public IMenuHandler {
      public:
        MenuHandler(std::shared_ptr<toolkit::config::IConfigManager> configManager,
                    std::shared_ptr<IDevice> device,
                    uint32_t displayWidth,
                    uint32_t displayHeight,
                    std::vector<int>& keyModifiers,
                    bool isHandTrackingSupported,
                    bool isPredictionDampeningSupported,
                    bool isMotionReprojectionRateSupported,
                    uint8_t variableRateShaderMaxDownsamplePow2)
            : m_configManager(configManager), m_device(device), m_displayWidth(displayWidth),
              m_displayHeight(displayHeight), m_keyModifiers(keyModifiers),
              m_isHandTrackingSupported(isHandTrackingSupported) {
            m_lastInput = std::chrono::steady_clock::now();

            // We display the hint for menu hotkeys for the first few runs.
            int firstRun = m_configManager->getValue("first_run");
            if (firstRun <= 10) {
                m_numSplashLeft = 10 - firstRun;
                m_state = MenuState::Splash;
                m_configManager->setValue("first_run", firstRun + 1);
            }

            m_configManager->setDefault("menu_eye", 0); // Both
            switch (m_configManager->getValue("menu_eye")) {
            case 0:
            default:
                m_displayLeftEye = m_displayRightEye = true;
                break;
            case 1:
                m_displayLeftEye = true;
                m_displayRightEye = false;
                break;
            case 2:
                m_displayLeftEye = false;
                m_displayRightEye = true;
                break;
            }

            if (std::count(m_keyModifiers.cbegin(), m_keyModifiers.cend(), VK_CONTROL)) {
                m_keyModifiersLabel += L"CTRL+";
            }
            if (std::count(m_keyModifiers.cbegin(), m_keyModifiers.cend(), VK_MENU)) {
                m_keyModifiersLabel += L"ALT+";
            }
            m_configManager->setDefault("key_left", m_keyLeft);
            m_configManager->setDefault("key_right", m_keyRight);
            m_configManager->setDefault("key_menu", m_keyMenu);
            m_configManager->setDefault("key_up", m_keyUp);
            m_keyLeft = m_configManager->getValue("key_left");
            m_keyLeftLabel = keyToString(m_keyLeft);
            m_keyRight = m_configManager->getValue("key_right");
            m_keyRightLabel = keyToString(m_keyRight);
            m_keyMenu = m_configManager->getValue("key_menu");
            m_keyMenuLabel = keyToString(m_keyMenu);
            m_keyUp = m_configManager->getValue("key_up");
            if (m_keyUp) {
                m_keyUpLabel = keyToString(m_keyUp);
            } else {
                m_keyUpLabel = L"SHIFT+" + m_keyMenuLabel;
            }

            // Prepare the tabs.
            static const std::string_view tabs[] = {"Performance", "Appearance", "Inputs", "Menu"};
            m_menuEntries.push_back({0, "", MenuEntryType::Tabs, "", 0, ARRAYSIZE(tabs) - 1, [&](int value) {
                                         return std::string(tabs[value]);
                                     }});
            m_menuEntries.back().pValue = reinterpret_cast<int*>(&m_currentTab);
            m_menuEntries.back().visible = true; /* Always visible. */

            m_menuEntries.push_back({0, "", MenuEntryType::Separator, BUTTON_OR_SEPARATOR});
            m_menuEntries.back().visible = true; /* Always visible. */

            setupPerformanceTab(isMotionReprojectionRateSupported, variableRateShaderMaxDownsamplePow2);
            setupAppearanceTab();
            setupInputsTab(isPredictionDampeningSupported);
            setupMenuTab();

            m_menuEntries.push_back({0, "Exit menu", MenuEntryType::ExitButton, BUTTON_OR_SEPARATOR});
            m_menuEntries.back().visible = true; /* Always visible. */
        }

        void handleInput() override {
            const auto now = std::chrono::steady_clock::now();

            // Check whether this is a long press and the event needs to be repeated.
            m_isAccelerating = GetAsyncKeyState(VK_SHIFT) < 0;

            const bool isRepeat = (now - m_lastInput) > (m_isAccelerating ? KeyRepeatDelay / 10 : KeyRepeatDelay);
            const bool moveLeft = UpdateKeyState(m_moveLeftKeyState, m_keyModifiers, m_keyLeft, isRepeat);
            const bool moveRight = UpdateKeyState(m_moveRightKeyState, m_keyModifiers, m_keyRight, isRepeat);
            const bool menuControl = UpdateKeyState(m_menuControlKeyState, m_keyModifiers, m_keyMenu, false);
            const bool moveUp = m_keyUp ? UpdateKeyState(m_moveUpKeyState, m_keyModifiers, m_keyUp, false)
                                        : menuControl && m_isAccelerating;

            if (menuControl || moveUp) {
                if (menuControl && m_state != MenuState::Visible) {
                    m_state = MenuState::Visible;

                    m_needRestart = checkNeedRestartCondition();
                    m_menuEntriesTitleWidth = 0.0f;
                    m_menuEntriesWidth = m_menuEntriesHeight = m_menuHeaderHeight = 0.0f;

                } else if (m_state == MenuState::Visible) {
                    do {
                        static_assert(std::is_unsigned_v<decltype(m_selectedItem)>);

                        m_selectedItem += moveUp ? -1 : 1;
                        if (m_selectedItem >= m_menuEntries.size()) {
                            m_selectedItem = moveUp ? m_menuEntries.size() - 1 : 0;
                        }

                    } while (m_menuEntries[m_selectedItem].type == MenuEntryType::Separator ||
                             !m_menuEntries[m_selectedItem].visible);
                }

                m_resetArmed = false;
                m_lastInput = now;
            }

            if (m_state == MenuState::Visible && (moveLeft || moveRight)) {
                auto& menuEntry = m_menuEntries[m_selectedItem];

                switch (menuEntry.type) {
                case MenuEntryType::Separator:
                    break;

                case MenuEntryType::RestoreDefaultsButton:
                    if (m_resetArmed) {
                        m_configManager->resetToDefaults();

                        m_needRestart = checkNeedRestartCondition();
                        m_menuEntriesTitleWidth = 0.0f;
                        m_menuEntriesWidth = m_menuEntriesHeight = m_menuHeaderHeight = 0.0f;
                        m_resetArmed = false;
                    } else {
                        m_resetArmed = true;
                    }
                    break;

                case MenuEntryType::ExitButton:
                    m_selectedItem = 0;
                    m_state = MenuState::NotVisible;
                    break;

                default:
                    const auto previousValue = peekEntryValue(menuEntry);
                    setEntryValue(menuEntry,
                                  previousValue +
                                      (moveLeft ? -1 : 1) * (!m_isAccelerating ? 1 : menuEntry.acceleration));

                    // When changing anamorphic setting, toggle the config value sign.
                    if (menuEntry.pValue == &m_useAnamorphic) {
                        auto value = m_configManager->peekValue(SettingAnamorphic);

                        // Fail safe: ensure there is a valid condition to toggle the sign of the value.
                        // However in this case, since useAnamorphic is binary, we could also toggle the sign of the
                        // value directly.

                        if ((m_useAnamorphic != 0) ^ (value > 0))
                            m_configManager->setValue(SettingAnamorphic, -value, true);
                    }

                    // When changing some settings, display the warning that the session must be restarted.
                    const bool wasRestartNeeded = std::exchange(m_needRestart, checkNeedRestartCondition());

                    // When changing the font size or displaying the restart banner, force re-alignment/re-size.
                    if ((menuEntry.configName == SettingMenuFontSize &&
                         previousValue != m_configManager->peekValue(SettingMenuFontSize)) ||
                        wasRestartNeeded != m_needRestart) {
                        m_menuEntriesTitleWidth = 0.0f;
                        m_menuEntriesWidth = m_menuEntriesHeight = m_menuHeaderHeight = 0.0f;
                    }

                    break;
                }

                m_lastInput = now;
            }

            if (menuControl && moveLeft && moveRight) {
                m_configManager->hardReset();
                m_state = MenuState::Splash;
                m_selectedItem = 0;
            }

            for (const auto& group : m_menuGroups) {
                group.updateVisibility();
            }
        }

        void render(Eye eye, std::shared_ptr<ITexture> renderTarget) const override {
            if ((eye == Eye::Left && !m_displayLeftEye) || (eye == Eye::Right && !m_displayRightEye)) {
                return;
            }

            const bool measureWidth = eye == Eye::Left || !m_displayLeftEye;

            const float leftEyeOffset = 0.0f;
            const float rightEyeOffset = (float)m_configManager->getValue(SettingMenuEyeOffset);
            const float eyeOffset = eye == Eye::Left ? leftEyeOffset : rightEyeOffset;

            const float leftAnchor = (renderTarget->getInfo().width - m_menuEntriesWidth) / 2;
            const float leftAlign = leftAnchor + eyeOffset;
            const float centerAlign = leftAnchor + m_menuEntriesWidth / 2 + eyeOffset;
            const float rightAlign = leftAnchor + m_menuEntriesWidth + eyeOffset;
            const float overlayAlign = (2 * renderTarget->getInfo().width / 3.0f) + eyeOffset;
            const float topAlign = renderTarget->getInfo().height / 3.0f;

            const float fontSizes[(int)MenuFontSize::MaxValue] = {
                renderTarget->getInfo().height * 0.0075f,
                renderTarget->getInfo().height * 0.015f,
                renderTarget->getInfo().height * 0.02f,
            };
            const float fontSize = fontSizes[m_configManager->getValue(SettingMenuFontSize)];

            const double timeouts[(int)MenuTimeout::MaxValue] = {3.0, 12.0, 60.0};
            const double timeout =
                m_state == MenuState::Splash ? 10.0 : timeouts[m_configManager->getValue(SettingMenuTimeout)];

            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration<double>(now - m_lastInput).count();

            // Apply menu fade.
            const auto alpha = (unsigned int)(std::clamp(timeout - duration, 0.0, 1.0) * 255.0);
            const auto textColorOverlay = COLOR_TO_TEXT_COLOR(ColorOverlay) | (alpha << 24);

            // Leave upon timeout.
            if (duration >= timeout) {
                m_state = MenuState::NotVisible;
            }

            if (m_state == MenuState::Splash) {
                // The helper "splash screen".
                m_device->drawString(fmt::format(L"Press {}{} to bring up the menu ({}s)",
                                                 m_keyModifiersLabel,
                                                 m_keyMenuLabel,
                                                 (int)(std::ceil(timeout - duration))),
                                     TextStyle::Normal,
                                     fontSize,
                                     leftAlign,
                                     topAlign,
                                     textColorOverlay);

                m_device->drawString(fmt::format("(this message will be displayed {} more time{})",
                                                 m_numSplashLeft,
                                                 m_numSplashLeft == 1 ? "" : "s"),
                                     TextStyle::Normal,
                                     fontSize * 0.75f,
                                     leftAlign,
                                     topAlign + 1.05f * fontSize,
                                     textColorOverlay);
            } else if (m_state == MenuState::Visible) {
                // The actual menu.

                // Apply menu fade.
                const auto textColorNormal = COLOR_TO_TEXT_COLOR(ColorWhite) | (alpha << 24);
                const auto textColorHighlight = COLOR_TO_TEXT_COLOR(ColorHighlight) | (alpha << 24);
                const auto textColorSelected = COLOR_TO_TEXT_COLOR(ColorSelected) | (alpha << 24);
                const auto textColorHint = COLOR_TO_TEXT_COLOR(ColorHint) | (alpha << 24);
                const auto textColorWarning = COLOR_TO_TEXT_COLOR(ColorWarning) | (alpha << 24);

                const bool measure = m_menuEntriesWidth == 0.0f;
                if (!measure) {
                    m_device->clearColor(topAlign - BorderVerticalSpacing,
                                         leftAlign - BorderHorizontalSpacing,
                                         topAlign + m_menuHeaderHeight,
                                         leftAlign + m_menuEntriesWidth + BorderHorizontalSpacing,
                                         ColorHeader);
                    m_device->clearColor(topAlign + m_menuHeaderHeight,
                                         leftAlign - BorderHorizontalSpacing,
                                         topAlign + m_menuHeaderHeight + HeaderLineWeight,
                                         leftAlign + m_menuEntriesWidth + BorderHorizontalSpacing,
                                         ColorHeaderSeparator);
                    m_device->clearColor(topAlign + m_menuHeaderHeight + HeaderLineWeight,
                                         leftAlign - BorderHorizontalSpacing,
                                         topAlign + m_menuEntriesHeight + BorderVerticalSpacing,
                                         leftAlign + m_menuEntriesWidth + BorderHorizontalSpacing,
                                         ColorBackground);
                }

                float top = topAlign;

                m_device->drawString(
                    toolkit::LayerPrettyNameFull, TextStyle::Bold, fontSize * 0.75f, leftAlign, top, textColorHint);

                top += 1.2f * fontSize;

                float menuEntriesTitleWidth = m_menuEntriesTitleWidth;

                // Display each menu entry.
                for (unsigned int i = 0; i < m_menuEntries.size(); i++) {
                    const auto& menuEntry = m_menuEntries[i];
                    const auto& title = (menuEntry.type == MenuEntryType::RestoreDefaultsButton && m_resetArmed)
                                            ? "Confirm?"
                                            : menuEntry.title;

                    float left = leftAlign + menuEntry.indent;

                    // Always account for entries, even invisible ones, when calculating the alignment.
                    float entryWidth = 0.0f;
                    if (menuEntriesTitleWidth == 0.0f) {
                        entryWidth = menuEntry.indent + m_device->measureString(title, TextStyle::Bold, fontSize) +
                                     OptionSpacing;
                        m_menuEntriesTitleWidth = std::max(m_menuEntriesTitleWidth, entryWidth);
                    }

                    if (!menuEntry.visible) {
                        continue;
                    }

                    const auto entryColor = i == m_selectedItem ? textColorSelected : textColorNormal;

                    if (menuEntry.type != MenuEntryType::Tabs) {
                        m_device->drawString(title, TextStyle::Bold, fontSize, left, top, entryColor);
                        if (menuEntriesTitleWidth == 0.0f) {
                            left += entryWidth;
                        } else {
                            left += menuEntriesTitleWidth;
                        }
                    }

                    const int value = peekEntryValue(menuEntry);

                    // Display the current value.
                    switch (menuEntry.type) {
                    case MenuEntryType::Slider:
                        left += m_device->drawString(menuEntry.valueToString(value),
                                                     TextStyle::Normal,
                                                     fontSize,
                                                     left,
                                                     top,
                                                     entryColor,
                                                     measure);
                        break;

                    case MenuEntryType::Tabs:
                    case MenuEntryType::Choice:
                        for (int j = menuEntry.minValue; j <= menuEntry.maxValue; j++) {
                            const std::string label = menuEntry.valueToString(j);

                            const auto style =
                                menuEntry.type == MenuEntryType::Tabs ? TextStyle::Bold : TextStyle::Normal;
                            const auto valueColor =
                                i == m_selectedItem && value != j ? textColorSelected : textColorNormal;
                            const auto backgroundColor = i == m_selectedItem ? ColorSelected : ColorHighlight;

                            const auto width = m_device->measureString(label, style, fontSize);

                            if (j == value) {
                                m_device->clearColor(top + 4,
                                                     left - SelectionHorizontalSpacing,
                                                     top + 4 + fontSize + SelectionVerticalSpacing,
                                                     left + width + SelectionHorizontalSpacing + 2,
                                                     backgroundColor);
                            }

                            m_device->drawString(label, style, fontSize, left, top, valueColor);
                            left += width + ValueSpacing;
                        }
                        break;

                    case MenuEntryType::Separator:
                        // Counteract the auto-down and add our own spacing.
                        top -= 1.5f * fontSize;
                        top += fontSize / 3;
                        break;

                    case MenuEntryType::RestoreDefaultsButton:
                        break;

                    case MenuEntryType::ExitButton:
                        if (duration > 1.0) {
                            left += m_device->drawString(fmt::format("({}s)", (int)(std::ceil(timeout - duration))),
                                                         TextStyle::Normal,
                                                         fontSize,
                                                         left,
                                                         top,
                                                         entryColor,
                                                         measure);
                        }
                        break;
                    }

                    top += 1.5f * fontSize;

                    if (measureWidth) {
                        m_menuEntriesWidth = std::max(m_menuEntriesWidth, left - leftAlign - eyeOffset);
                    }

                    if (menuEntry.type == MenuEntryType::Tabs) {
                        m_menuHeaderHeight = top - topAlign + 3;
                    }
                }

                {
                    float left = leftAlign;
                    if (m_configManager->isSafeMode()) {
                        top += fontSize;

                        left +=
                            m_device->drawString(L"\x26A0  Running in safe mode with defaults settings  \x26A0",
                                                 TextStyle::Bold,
                                                 fontSize,
                                                 leftAlign,
                                                 top,
                                                 textColorWarning,
                                                 measure);

                        top += 1.05f * fontSize;
                    } else if (m_needRestart) {
                        top += fontSize;

                        left += m_device->drawString(L"\x26A0  Restart the VR session to apply changes  \x26A0",
                                                     TextStyle::Bold,
                                                     fontSize,
                                                     leftAlign,
                                                     top,
                                                     textColorWarning,
                                                     measure);

                        top += 1.05f * fontSize;
                    }

                    if (measureWidth) {
                        m_menuEntriesWidth = std::max(m_menuEntriesWidth, left - leftAlign - eyeOffset);
                    }
                }

                {
                    float left = leftAlign;

                    // Create a little spacing.
                    top += 10;

                    left += m_device->drawString(
                        fmt::format(L"{0}  ( {1} : \xE2B6 )   ( {2} : \xE1FE)   ( {3} : \xE1FC)   ( {4} : \xE2B7 )",
                                    m_keyModifiersLabel,
                                    m_keyLeftLabel,
                                    m_keyUpLabel,
                                    m_keyMenuLabel,
                                    m_keyRightLabel),
                        TextStyle::Normal,
                        fontSize * 0.75f,
                        leftAlign,
                        top,
                        textColorHint,
                        measure);
                    top += 0.8f * fontSize;

                    m_device->drawString(L"Change values faster with SHIFT",
                                         TextStyle::Normal,
                                         fontSize * 0.75f,
                                         leftAlign,
                                         top,
                                         textColorHint);
                    top += 0.8f * fontSize;

                    if (measureWidth) {
                        m_menuEntriesWidth = std::max(m_menuEntriesWidth, left - leftAlign - eyeOffset);
                    }
                }
                m_menuEntriesHeight = (top + fontSize * 0.2f) - topAlign;
            }

            auto overlayType = m_configManager->getEnumValue<OverlayType>(SettingOverlayType);
            if (m_state != MenuState::Splash && overlayType != OverlayType::None) {
                const auto textColorOverlayNoFade = COLOR_TO_TEXT_COLOR(ColorOverlay) | (0xff << 24);

                float top =
                    m_state != MenuState::Visible ? topAlign : topAlign - BorderVerticalSpacing - 1.1f * fontSize;

#define OVERLAY_COMMON TextStyle::Normal, fontSize, overlayAlign - 200, top, textColorOverlayNoFade, true, FW1_LEFT

                // FPS display.
                m_device->drawString(fmt::format("FPS: {}", m_stats.fps), OVERLAY_COMMON);
                top += 1.05f * fontSize;

                // Advanced display.
                if (m_state != MenuState::Visible && overlayType == OverlayType::Advanced) {
#define TIMING_STAT(label, name)                                                                                       \
    m_device->drawString(fmt::format(label ": {}", m_stats.name), OVERLAY_COMMON);                                     \
    top += 1.05f * fontSize;

                    TIMING_STAT("app CPU", appCpuTimeUs);
                    TIMING_STAT("app GPU", appGpuTimeUs);
                    TIMING_STAT("lay CPU", endFrameCpuTimeUs);
                    TIMING_STAT("pre GPU", preProcessorGpuTimeUs);
                    TIMING_STAT("scl GPU", upscalerGpuTimeUs);
                    TIMING_STAT("pst GPU", postProcessorGpuTimeUs);
                    TIMING_STAT("ovl CPU", overlayCpuTimeUs);
                    TIMING_STAT("ovl GPU", overlayGpuTimeUs);
                    if (m_isHandTrackingSupported) {
                        TIMING_STAT("hnd CPU", handTrackingCpuTimeUs);
                    }

                    m_device->drawString(fmt::format("{}{} / {}{}",
                                                     m_stats.hasColorBuffer[0] ? "C" : "_",
                                                     m_stats.hasDepthBuffer[0] ? "D" : "_",
                                                     m_stats.hasColorBuffer[1] ? "C" : "_",
                                                     m_stats.hasDepthBuffer[1] ? "D" : "_"),
                                         OVERLAY_COMMON);
                    top += 1.05f * fontSize;

                    m_device->drawString(fmt::format("biased: {}", m_stats.numBiasedSamplers), OVERLAY_COMMON);
                    top += 1.05f * fontSize;
                    m_device->drawString(fmt::format("VRS RTV: {}", m_stats.numRenderTargetsWithVRS), OVERLAY_COMMON);
                    top += 1.05f * fontSize;

#undef TIMING_STAT

                    top += 1.05f * fontSize;

#define GESTURE_STATE(label, name)                                                                                     \
    m_device->drawString(                                                                                              \
        fmt::format(label ": {:.2f}/{:.2f}", m_gesturesState.name##Value[0], m_gesturesState.name##Value[1]),          \
        OVERLAY_COMMON);                                                                                               \
    top += 1.05f * fontSize;

                    if (isHandTrackingEnabled()) {
                        GESTURE_STATE("pinch", pinch);
                        GESTURE_STATE("thb.pr", thumbPress);
                        GESTURE_STATE("indx.b", indexBend);
                        GESTURE_STATE("f.gun", fingerGun);
                        GESTURE_STATE("squze", squeeze);
                        GESTURE_STATE("wrist", wristTap);
                        GESTURE_STATE("palm", palmTap);
                        GESTURE_STATE("tiptap", indexTipTap);
                        GESTURE_STATE("cust1", custom1);

                        m_device->drawString(fmt::format("loss: {}/{}",
                                                         m_gesturesState.numTrackingLosses[0] % 256,
                                                         m_gesturesState.numTrackingLosses[1] % 256),
                                             OVERLAY_COMMON);
                        top += 1.05f * fontSize;
                    }

#undef GESTURE_STATE
                }
#undef OVERLAY_COMMON
            }
        }

        void updateStatistics(const MenuStatistics& stats) override {
            m_stats = stats;
        }

        void updateGesturesState(const GesturesState& state) override {
            m_gesturesState = state;
        }

      private:
        void setupPerformanceTab(bool isMotionReprojectionRateSupported, uint8_t variableRateShaderMaxDownsamplePow2) {
            MenuGroup performanceTab(
                m_menuGroups,
                m_menuEntries,
                [&] { return m_currentTab == MenuTab::Performance; } /* visible condition */,
                true /* isTab */);

            // Performance Overlay Settings.
            m_menuEntries.push_back({OptionIndent,
                                     "Overlay",
                                     MenuEntryType::Choice,
                                     SettingOverlayType,
                                     0,
                                     (int)OverlayType::MaxValue - (m_configManager->isExperimentalMode() ? 1 : 2),
                                     [](int value) {
                                         const std::string_view labels[] = {"Off", "FPS", "Detailed"};
                                         return std::string(labels[value]);
                                     }});
            m_configManager->setEnumDefault(SettingOverlayType, OverlayType::None);

            // Upscaling Settings.
            m_originalScalingType = getCurrentScalingType();
            m_originalScalingValue = getCurrentScaling();
            m_originalAnamorphicValue = getCurrentAnamorphic();
            m_useAnamorphic = m_originalAnamorphicValue > 0 ? 1 : 0;

            m_menuEntries.push_back({OptionIndent,
                                     "Upscaling",
                                     MenuEntryType::Choice,
                                     SettingScalingType,
                                     0,
                                     (int)ScalingType::MaxValue - 1,
                                     [](int value) {
                                         const std::string_view labels[] = {"Off", "NIS", "FSR"};
                                         return std::string(labels[value]);
                                     }});
            m_menuEntries.back().noCommitDelay = true;

            // Scaling sub-group.
            MenuGroup upscalingGroup(m_menuGroups, m_menuEntries, [&] {
                return getCurrentScalingType() != ScalingType::None;
            } /* visible condition */);
            m_menuEntries.push_back({SubGroupIndent, "Anamorphic", MenuEntryType::Choice, "", 0, 1, [](int value) {
                                         const std::string_view labels[] = {"Off", "On"};
                                         return std::string(labels[value]);
                                     }});
            m_menuEntries.back().noCommitDelay = true;
            m_menuEntries.back().pValue = &m_useAnamorphic;

            // Proportional sub-group.
            MenuGroup proportionalGroup(m_menuGroups, m_menuEntries, [&] {
                return getCurrentScalingType() != ScalingType::None && !m_useAnamorphic;
            } /* visible condition */);
            m_menuEntries.push_back(
                {SubGroupIndent, "Size", MenuEntryType::Slider, SettingScaling, 25, 400, [&](int value) {
                     // We don't even use value, the utility function below will query it.
                     return fmt::format("{}% ({}x{})",
                                        value,
                                        GetScaledInputSize(m_displayWidth, value, 2),
                                        GetScaledInputSize(m_displayHeight, value, 2));
                 }});
            m_menuEntries.back().noCommitDelay = true;
            proportionalGroup.finalize();

            // Anamorphic sub-group.
            MenuGroup anamorphicGroup(m_menuGroups, m_menuEntries, [&] {
                return getCurrentScalingType() != ScalingType::None && m_useAnamorphic;
            } /* visible condition */);
            m_menuEntries.push_back(
                {SubGroupIndent, "Width", MenuEntryType::Slider, SettingScaling, 25, 400, [&](int value) {
                     return fmt::format("{}% ({} pixels)", value, GetScaledInputSize(m_displayWidth, value, 2));
                 }});
            m_menuEntries.back().noCommitDelay = true;

            m_menuEntries.push_back(
                {SubGroupIndent, "Height", MenuEntryType::Slider, SettingAnamorphic, 25, 400, [&](int value) {
                     return fmt::format("{}% ({} pixels)", value, GetScaledInputSize(m_displayHeight, value, 2));
                 }});
            m_menuEntries.back().noCommitDelay = true;
            anamorphicGroup.finalize();

            m_menuEntries.push_back(
                {SubGroupIndent, "Sharpness", MenuEntryType::Slider, SettingSharpness, 0, 100, [](int value) {
                     return fmt::format("{}%", value);
                 }});
            m_menuEntries.push_back({SubGroupIndent,
                                     "Mip-map bias",
                                     MenuEntryType::Slider,
                                     SettingMipMapBias,
                                     0,
                                     (int)MipMapBias::MaxValue - 1,
                                     [](int value) {
                                         const std::string_view labels[] = {"Off", "Conservative", "All"};
                                         return std::string(labels[value]);
                                     }});
            upscalingGroup.finalize();

            // Motion Reprojection Settings.
            if (isMotionReprojectionRateSupported) {
                m_menuEntries.push_back({OptionIndent,
                                         "Lock motion reprojection",
                                         MenuEntryType::Slider,
                                         SettingMotionReprojectionRate,
                                         (int)MotionReprojectionRate::Off,
                                         (int)MotionReprojectionRate::MaxValue - 1,
                                         [&](int value) {
                                             std::string_view labels[] = {"Unlocked", "1/half", "1/third", "1/quarter"};
                                             return std::string(labels[value - 1]);
                                         }});
            }

            // Fixed Foveated Rendering (VRS) Settings.
            if (variableRateShaderMaxDownsamplePow2) {
                m_menuEntries.push_back({OptionIndent,
                                         "Fixed foveated rendering",
                                         MenuEntryType::Choice,
                                         SettingVRS,
                                         0,
                                         (int)VariableShadingRateType::MaxValue - 1,
                                         [&](int value) {
                                             const std::string_view labels[] = {"Off", "Preset", "Custom"};
                                             return std::string(labels[value]);
                                         }});

                // Preset sub-group.
                MenuGroup variableRateShaderPresetGroup(m_menuGroups, m_menuEntries, [&] {
                    return m_configManager->peekEnumValue<VariableShadingRateType>(SettingVRS) ==
                           VariableShadingRateType::Preset;
                } /* visible condition */);
                m_menuEntries.push_back({SubGroupIndent,
                                         "Mode",
                                         MenuEntryType::Slider,
                                         SettingVRSQuality,
                                         0,
                                         (int)VariableShadingRateQuality::MaxValue - 1,
                                         [&](int value) {
                                             const std::string_view labels[] = {"Performance", "Quality"};
                                             return std::string(labels[value]);
                                         }});
                m_menuEntries.push_back({SubGroupIndent,
                                         "Pattern",
                                         MenuEntryType::Slider,
                                         SettingVRSPattern,
                                         0,
                                         (int)VariableShadingRatePattern::MaxValue - 1,
                                         [&](int value) {
                                             const std::string_view labels[] = {"Wide", "Balanced", "Narrow"};
                                             return std::string(labels[value]);
                                         }});
                variableRateShaderPresetGroup.finalize();

                // Custom sub-group.
                MenuGroup variableRateShaderCustomGroup(m_menuGroups, m_menuEntries, [&] {
                    return m_configManager->peekEnumValue<VariableShadingRateType>(SettingVRS) ==
                           VariableShadingRateType::Custom;
                } /* visible condition */);
                {
                    static auto samplePow2ToString = [](int value) -> std::string {
                        switch (value) {
                        case 0:
                            return "1x";
                        default:
                            return fmt::format("1/{}x", 1 << value);
                        case 5:
                            return "Cull";
                        }
                    };
                    static auto radiusToString = [](int value) { return fmt::format("{}%", value); };
                    m_menuEntries.push_back({SubGroupIndent,
                                             "Inner resolution",
                                             MenuEntryType::Slider,
                                             SettingVRSInner,
                                             0,
                                             variableRateShaderMaxDownsamplePow2,
                                             samplePow2ToString});
                    m_menuEntries.push_back({SubGroupIndent,
                                             "Inner ring size",
                                             MenuEntryType::Slider,
                                             SettingVRSInnerRadius,
                                             0,
                                             100,
                                             radiusToString});
                    m_menuEntries.push_back({SubGroupIndent,
                                             "Middle resolution",
                                             MenuEntryType::Slider,
                                             SettingVRSMiddle,
                                             1, // Exclude 1x to discourage people from using poor settings!
                                             variableRateShaderMaxDownsamplePow2,
                                             samplePow2ToString});
                    m_menuEntries.push_back({SubGroupIndent,
                                             "Outer ring size",
                                             MenuEntryType::Slider,
                                             SettingVRSOuterRadius,
                                             0,
                                             100,
                                             radiusToString});
                    m_menuEntries.push_back({SubGroupIndent,
                                             "Outer resolution",
                                             MenuEntryType::Slider,
                                             SettingVRSOuter,
                                             1, // Exclude 1x to discourage people from using poor settings!
                                             variableRateShaderMaxDownsamplePow2,
                                             samplePow2ToString});
                }
                variableRateShaderCustomGroup.finalize();
            }

            // Must be kept last.
            performanceTab.finalize();
        }

        void setupAppearanceTab() {
            MenuGroup appearanceTab(
                m_menuGroups,
                m_menuEntries,
                [&] { return m_currentTab == MenuTab::Appearance; } /* visible condition */,
                true /* isTab */);
            m_menuEntries.push_back(
                {OptionIndent, "Brightness", MenuEntryType::Slider, SettingBrightness, 0, 1000, [](int value) {
                     return fmt::format("{:.1f}", value / 10.f);
                 }});
            m_menuEntries.back().acceleration = 5;
            m_menuEntries.push_back(
                {OptionIndent, "Contrast", MenuEntryType::Slider, SettingContrast, 0, 1000, [](int value) {
                     return fmt::format("{:.1f}", value / 10.f);
                 }});
            m_menuEntries.back().acceleration = 5;
            m_menuEntries.push_back(
                {OptionIndent, "Saturation", MenuEntryType::Slider, SettingSaturation, 0, 1000, [](int value) {
                     return fmt::format("{:.1f}", value / 10.f);
                 }});
            m_menuEntries.back().acceleration = 5;
            m_menuEntries.push_back(
                {OptionIndent, "World scale", MenuEntryType::Slider, SettingICD, 1, 10000, [&](int value) {
                     return fmt::format("{:.1f}% ({:.1f}mm)", value / 10.f, m_stats.icd * 1000);
                 }});
            m_menuEntries.back().acceleration = 5;
            m_menuEntries.push_back(
                {OptionIndent, "Field of view", MenuEntryType::Slider, SettingFOV, 50, 150, [&](int value) {
                     return fmt::format("{}% ({:.1f} deg)", value, m_stats.totalFov * 180.0f / M_PI);
                 }});

            // Must be kept last.
            appearanceTab.finalize();
        }

        void setupInputsTab(bool isPredictionDampeningSupported) {
            MenuGroup inputsTab(
                m_menuGroups,
                m_menuEntries,
                [&] { return m_currentTab == MenuTab::Inputs; } /* visible condition */,
                true /* isTab */);

            if (isPredictionDampeningSupported) {
                m_menuEntries.push_back({OptionIndent,
                                         "Shaking attenuation",
                                         MenuEntryType::Slider,
                                         SettingPredictionDampen,
                                         0,
                                         100,
                                         [&](int value) {
                                             if (value == 100) {
                                                 return fmt::format("{}%", value - 100);
                                             } else {
                                                 return fmt::format("{}% (of {:.1f}ms)",
                                                                    value - 100,
                                                                    m_stats.predictionTimeUs / 1000000.0f);
                                             }
                                         }});
            }

            if (m_isHandTrackingSupported) {
                m_menuEntries.push_back({OptionIndent,
                                         "Controller emulation",
                                         MenuEntryType::Choice,
                                         SettingHandTrackingEnabled,
                                         0,
                                         (int)HandTrackingEnabled::MaxValue - 1,
                                         [](int value) {
                                             const std::string_view labels[] = {"Off", "Both", "Left", "Right"};
                                             return std::string(labels[value]);
                                         }});
                m_originalHandTrackingEnabled = isHandTrackingEnabled();

                MenuGroup handTrackingGroup(
                    m_menuGroups, m_menuEntries, [&] { return isHandTrackingEnabled(); } /* visible condition */);
                m_menuEntries.push_back(
                    {SubGroupIndent,
                     "Hand skeleton",
                     MenuEntryType::Slider,
                     SettingHandVisibilityAndSkinTone,
                     0,
                     4,
                     [](int value) {
                         const std::string_view labels[] = {"Hidden", "Bright", "Medium", "Dark", "Darker"};
                         return std::string(labels[value]);
                     }});
                m_menuEntries.push_back({SubGroupIndent,
                                         "Controller timeout",
                                         MenuEntryType::Slider,
                                         SettingHandTimeout,
                                         0,
                                         60,
                                         [](int value) {
                                             if (value == 0) {
                                                 return std::string("Always on");
                                             } else {
                                                 return fmt::format("{}s", value);
                                             }
                                         }});
                handTrackingGroup.finalize();
            }

            // Must be kept last.
            inputsTab.finalize();
        }

        void setupMenuTab() {
            MenuGroup menuTab(
                m_menuGroups,
                m_menuEntries,
                [&] { return m_currentTab == MenuTab::Menu; } /* visible condition */,
                true /* isTab */);

            m_menuEntries.push_back({OptionIndent,
                                     "Font size",
                                     MenuEntryType::Slider,
                                     SettingMenuFontSize,
                                     0,
                                     (int)MenuFontSize::MaxValue - 1,
                                     [](int value) {
                                         const std::string_view labels[] = {"Small", "Medium", "Large"};
                                         return std::string(labels[value]);
                                     }});
            m_configManager->setEnumDefault(SettingMenuFontSize, MenuFontSize::Medium);
            m_menuEntries.push_back({OptionIndent,
                                     "Menu timeout",
                                     MenuEntryType::Slider,
                                     SettingMenuTimeout,
                                     0,
                                     (int)MenuTimeout::MaxValue - 1,
                                     [](int value) {
                                         const std::string_view labels[] = {"Short", "Medium", "Long"};
                                         return std::string(labels[value]);
                                     }});
            m_configManager->setEnumDefault(SettingMenuTimeout, MenuTimeout::Medium);
            m_menuEntries.push_back({OptionIndent,
                                     "Menu eye offset",
                                     MenuEntryType::Slider,
                                     SettingMenuEyeOffset,
                                     -3000,
                                     3000,
                                     [](int value) { return fmt::format("{} pixels", value); }});
            m_menuEntries.back().acceleration = 10;

            m_menuEntries.push_back(
                {OptionIndent, "Restore defaults", MenuEntryType::RestoreDefaultsButton, BUTTON_OR_SEPARATOR});

            // Must be kept last.
            menuTab.finalize();
        }

        int peekEntryValue(const MenuEntry& menuEntry) const {
            return menuEntry.pValue ? *menuEntry.pValue : m_configManager->peekValue(menuEntry.configName);
        }

        void setEntryValue(MenuEntry& menuEntry, int newValue) {
            newValue = std::clamp(newValue, menuEntry.minValue, menuEntry.maxValue);
            if (!menuEntry.pValue) {
                m_configManager->setValue(menuEntry.configName, newValue, menuEntry.noCommitDelay);
            } else {
                *menuEntry.pValue = newValue;
            }
        }

        std::wstring keyToString(int key) const {
            wchar_t buf[16];
            GetKeyNameTextW(MAKELPARAM(0, MapVirtualKeyA(key, MAPVK_VK_TO_VSC)), buf, ARRAYSIZE(buf));
            return buf;
        }

        bool isHandTrackingEnabled() const {
            return m_isHandTrackingSupported && m_configManager->peekEnumValue<HandTrackingEnabled>(
                                                    SettingHandTrackingEnabled) != HandTrackingEnabled::Off;
        }

        int getCurrentScaling() const {
            return m_configManager->peekValue(SettingScaling);
        }

        int getCurrentAnamorphic() const {
            return m_configManager->peekValue(SettingAnamorphic);
        }

        ScalingType getCurrentScalingType() const {
            return m_configManager->peekEnumValue<ScalingType>(SettingScalingType);
        }

        bool checkNeedRestartCondition() const {
            if (m_originalHandTrackingEnabled != isHandTrackingEnabled() ||
                m_originalScalingType != getCurrentScalingType()) {
                return true;
            }

            if (m_originalScalingType != ScalingType::None) {
                return m_originalScalingValue != getCurrentScaling() ||
                       m_originalAnamorphicValue != getCurrentAnamorphic();
            }

            return false;
        }

        const std::shared_ptr<IConfigManager> m_configManager;
        const std::shared_ptr<IDevice> m_device;
        const uint32_t m_displayWidth;
        const uint32_t m_displayHeight;
        const bool m_isHandTrackingSupported;
        MenuStatistics m_stats{};
        GesturesState m_gesturesState{};

        bool m_displayLeftEye{true};
        bool m_displayRightEye{true};

        std::vector<int> m_keyModifiers;
        std::wstring m_keyModifiersLabel;
        int m_keyLeft{VK_F1};
        std::wstring m_keyLeftLabel;
        int m_keyRight{VK_F3};
        std::wstring m_keyRightLabel;
        int m_keyMenu{VK_F2};
        std::wstring m_keyMenuLabel;
        int m_keyUp{0};
        std::wstring m_keyUpLabel;

        int m_numSplashLeft;
        std::vector<MenuEntry> m_menuEntries;
        std::vector<MenuGroup> m_menuGroups;
        size_t m_selectedItem{0};
        MenuTab m_currentTab{MenuTab::Performance};
        std::chrono::steady_clock::time_point m_lastInput;
        bool m_moveLeftKeyState{false};
        bool m_moveRightKeyState{false};
        bool m_menuControlKeyState{false};
        bool m_moveUpKeyState{false};
        bool m_resetArmed{false};

        // animation control
        bool m_isAccelerating{false};

        ScalingType m_originalScalingType{ScalingType::None};
        int m_originalScalingValue{0};
        int m_originalAnamorphicValue{0};
        int m_useAnamorphic{0};

        bool m_originalHandTrackingEnabled{false};
        bool m_needRestart{false};

        mutable MenuState m_state{MenuState::NotVisible};
        mutable float m_menuEntriesTitleWidth{0.0f};
        mutable float m_menuEntriesWidth{0.0f};
        mutable float m_menuEntriesHeight{0.0f};
        mutable float m_menuHeaderHeight{0.0f};
    };

} // namespace

namespace toolkit::menu {
    std::shared_ptr<IMenuHandler> CreateMenuHandler(std::shared_ptr<toolkit::config::IConfigManager> configManager,
                                                    std::shared_ptr<toolkit::graphics::IDevice> device,
                                                    uint32_t displayWidth,
                                                    uint32_t displayHeight,
                                                    std::vector<int>& keyModifiers,
                                                    bool isHandTrackingSupported,
                                                    bool isPredictionDampeningSupported,
                                                    bool isMotionReprojectionRateSupported,
                                                    uint8_t variableRateShaderMaxDownsamplePow2) {
        return std::make_shared<MenuHandler>(configManager,
                                             device,
                                             displayWidth,
                                             displayHeight,
                                             keyModifiers,
                                             isHandTrackingSupported,
                                             isPredictionDampeningSupported,
                                             isMotionReprojectionRateSupported,
                                             variableRateShaderMaxDownsamplePow2);
    }

} // namespace toolkit::menu
