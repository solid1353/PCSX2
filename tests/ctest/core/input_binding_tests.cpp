// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "Input/InputManager.h"
#include "LayeredSettingsInterface.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Pad/PadDualshock2.h"
#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

TEST(HotkeyBindings, ActiveBindingsComeOnlyFromSettings)
{
	MemorySettingsInterface settings;
	const HotkeyInfo hotkey{"Example", "Game List", "Example", nullptr, "Keyboard/Control & Keyboard/R"};
	EXPECT_TRUE(InputManager::GetHotkeyBindings(settings, hotkey).empty());
	EXPECT_FALSE(settings.ContainsValue("Hotkeys", "Example"));

	settings.SetStringValue("Hotkeys", "Example", "Keyboard/F7");
	EXPECT_EQ(InputManager::GetHotkeyBindings(settings, hotkey), (std::vector<std::string>{"Keyboard/F7"}));

	settings.SetStringValue("Hotkeys", "Example", "");
	EXPECT_TRUE(InputManager::GetHotkeyBindings(settings, hotkey).empty());
	settings.DeleteValue("Hotkeys", "Example");
	EXPECT_TRUE(InputManager::GetHotkeyBindings(settings, hotkey).empty());
}

namespace
{
	class KeyboardPadChordTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_old_config = EmuConfig;
			for (auto& port : EmuConfig.Pad.Ports)
				port.Type = Pad::ControllerType::NotConnected;
			EmuConfig.Pad.Ports[0].Type = Pad::ControllerType::DualShock2;
			EmuConfig.Pad.Ports[1].Type = Pad::ControllerType::DualShock2;
			ConfigureBindings();
			m_layered_settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &m_base_settings);
			Pad::LoadConfig(m_settings);
			InputManager::ReloadBindings(m_layered_settings, m_settings, m_settings, false, false);
			const auto* info = Pad::GetControllerInfo(Pad::ControllerType::DualShock2);
			m_up_bind = info->bindings[*info->GetBindIndex("Up")].bind_index;
		}

		void TearDown() override
		{
			MemorySettingsInterface empty_settings;
			LayeredSettingsInterface layered_settings;
			layered_settings.SetLayer(LayeredSettingsInterface::LAYER_BASE, &empty_settings);
			InputManager::ReloadBindings(layered_settings, empty_settings, empty_settings, false, false);
			Pad::Shutdown();
			EmuConfig = std::move(m_old_config);
		}

		virtual void ConfigureBindings()
		{
			m_settings.SetStringList("Pad1", "Up", {"Keyboard/W"});
			m_settings.SetStringList("Pad2", "Up", {"Keyboard/Shift & Keyboard/W"});
		}

		void Key(u32 code, float value)
		{
			InputBindingKey key = {};
			key.source_type = InputSourceType::Keyboard;
			key.data = code;
			ASSERT_TRUE(InputManager::InvokeEvents(key, value));
		}

		void Press(const char* binding, float value)
		{
			const std::optional<InputBindingKey> key = InputManager::ParseInputBindingKey(binding);
			ASSERT_TRUE(key.has_value());
			ASSERT_TRUE(InputManager::InvokeEvents(key.value(), value));
		}

		float PadUp(u8 pad) const { return Pad::GetPad(pad)->GetEffectiveInput(m_up_bind); }

		Pcsx2Config m_old_config;
		MemorySettingsInterface m_base_settings;
		MemorySettingsInterface m_settings;
		LayeredSettingsInterface m_layered_settings;
		u32 m_up_bind = 0;
	};

	// Pointer buttons stand in for controller buttons: both reach pad bindings
	// through the non-keyboard path. HoldTurbo does nothing without a VM.
	class ControllerHotkeyChordTest : public KeyboardPadChordTest
	{
	protected:
		void ConfigureBindings() override
		{
			m_settings.SetStringList("Pad1", "Up", {"Pointer-0/Button0"});
			m_settings.SetStringList("Hotkeys", "HoldTurbo", {"Pointer-0/Button1 & Pointer-0/Button0"});
		}
	};

	class SelectStartR1Test : public KeyboardPadChordTest
	{
	protected:
		void ConfigureBindings() override
		{
			KeyboardPadChordTest::ConfigureBindings();
			m_settings.SetStringList("Pad1", "Select", {"Keyboard/Backspace"});
			m_settings.SetStringList("Pad1", "Start", {"Keyboard/Return"});
			m_settings.SetStringList("Pad1", "R1", {"Keyboard/E"});
			m_settings.SetStringList("Pad2", "Select", {"Keyboard/Shift & Keyboard/Backspace"});
			m_settings.SetStringList("Pad2", "Start", {"Keyboard/Shift & Keyboard/Return"});
			m_settings.SetStringList("Pad2", "R1", {"Keyboard/Shift & Keyboard/E"});
		}
	};
} // namespace

TEST_F(SelectStartR1Test, EveryPressOrderPreservesAllThreePadInputs)
{
	const char* keys[] = {"Keyboard/Backspace", "Keyboard/Return", "Keyboard/E"};
	const u32 inputs[] = {PadDualshock2::PAD_SELECT, PadDualshock2::PAD_START, PadDualshock2::PAD_R1};
	unsigned order[] = {0, 1, 2};
	do
	{
		bool held[3] = {};
		for (const unsigned step : order)
		{
			Press(keys[step], 1.0f);
			held[step] = true;
			for (unsigned i = 0; i < 3; i++)
			{
				EXPECT_FLOAT_EQ(Pad::GetPad(0)->GetEffectiveInput(inputs[i]), held[i] ? 255.0f : 0.0f)
					<< keys[step] << " -> " << keys[i];
				EXPECT_FLOAT_EQ(Pad::GetPad(1)->GetEffectiveInput(inputs[i]), 0.0f);
			}
		}
		for (const unsigned step : order)
		{
			Press(keys[step], 0.0f);
			held[step] = false;
			for (unsigned i = 0; i < 3; i++)
				EXPECT_FLOAT_EQ(Pad::GetPad(0)->GetEffectiveInput(inputs[i]), held[i] ? 255.0f : 0.0f);
		}
	} while (std::next_permutation(std::begin(order), std::end(order)));
}

TEST_F(KeyboardPadChordTest, ShiftAloneDoesNotPressPadTwoAndCompletedChordSuppressesPadOne)
{
	Key(16, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);

	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 255.0f);
	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);

	Key(16, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);
	Key(87, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

TEST_F(KeyboardPadChordTest, ChordCancelsAnAlreadyHeldPlainKeyAndReleasesWhenEitherKeyLifts)
{
	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);

	Key(16, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 255.0f);

	Key(87, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);
	Key(87, 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 255.0f);
	Key(16, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	EXPECT_FLOAT_EQ(PadUp(1), 0.0f);
	Key(87, 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

TEST_F(ControllerHotkeyChordTest, HeldHotkeyChordKeepsItsPadInputReleased)
{
	Press("Pointer-0/Button1", 1.0f);
	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	Press("Pointer-0/Button0", 0.0f);
	Press("Pointer-0/Button1", 0.0f);

	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	Press("Pointer-0/Button0", 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

TEST_F(ControllerHotkeyChordTest, CompletedHotkeyChordReleasesAnAlreadyHeldPadInput)
{
	Press("Pointer-0/Button0", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 255.0f);
	Press("Pointer-0/Button1", 1.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
	Press("Pointer-0/Button0", 0.0f);
	EXPECT_FLOAT_EQ(PadUp(0), 0.0f);
}

// A stick deflection inside the analog deadzone centers the stick in every direction,
// including straight left, up and down, where the other axis is exactly zero.
TEST(DualShock2Deadzone, CentersAxisAlignedDeflectionsInsideTheDeadzone)
{
	constexpr float deflection = 10.0f / 255.0f;
	for (const u32 input : {PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_RIGHT,
			 PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN,
			 PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_RIGHT,
			 PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_DOWN})
	{
		PadDualshock2 pad(0, 0);
		pad.SetAxisScale(0.2f, 1.0f);
		pad.Set(input, deflection);
		EXPECT_FLOAT_EQ(pad.GetEffectiveInput(input), 0.0f) << "input " << input;
	}
}

TEST(DualShock2Deadzone, KeepsDeflectionsPastTheDeadzone)
{
	PadDualshock2 pad(0, 0);
	pad.SetAxisScale(0.2f, 1.0f);
	pad.Set(PadDualshock2::Inputs::PAD_L_LEFT, 0.5f);
	EXPECT_LT(pad.GetEffectiveInput(PadDualshock2::Inputs::PAD_L_LEFT), 0.0f);
}
