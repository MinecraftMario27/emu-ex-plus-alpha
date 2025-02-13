/*  This file is part of NES.emu.

	NES.emu is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	NES.emu is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with NES.emu.  If not, see <http://www.gnu.org/licenses/> */

#include <emuframework/EmuApp.hh>
#include "MainSystem.hh"
#include <fceu/sound.h>
#include <fceu/fceu.h>

namespace EmuEx
{

const char *EmuSystem::configFilename = "NesEmu.config";

std::span<const AspectRatioInfo> NesSystem::aspectRatioInfos()
{
	static constexpr AspectRatioInfo aspectRatioInfo[]
	{
		{"4:3 (Original)", {4, 3}},
		EMU_SYSTEM_DEFAULT_ASPECT_RATIO_INFO_INIT
	};
	return aspectRatioInfo;
}

void NesSystem::onOptionsLoaded()
{
	FCEUI_SetSoundQuality(optionSoundQuality);
	FCEUI_DisableSpriteLimitation(!optionSpriteLimit);
	setDefaultPalette(appContext(), defaultPalettePath);
	optionStartVideoLine.defaultVal = optionDefaultStartVideoLine;
	optionVisibleVideoLines.defaultVal = optionDefaultVisibleVideoLines;
}

void NesSystem::onSessionOptionsLoaded(EmuApp &app)
{
	nesInputPortDev[0] = (ESI)(int)optionInputPort1;
	nesInputPortDev[1] = (ESI)(int)optionInputPort2;
	updateVideoPixmap(app.video(), optionHorizontalVideoCrop, optionVisibleVideoLines);
}

bool NesSystem::resetSessionOptions(EmuApp &app)
{
	optionFourScore.reset();
	setupNESFourScore();
	optionVideoSystem.reset();
	optionInputPort1.reset();
	optionInputPort2.reset();
	nesInputPortDev[0] = (ESI)(int)optionInputPort1;
	nesInputPortDev[1] = (ESI)(int)optionInputPort2;
	setupNESInputPorts();
	optionCompatibleFrameskip.reset();
	optionStartVideoLine.reset();
	optionVisibleVideoLines.reset();
	optionHorizontalVideoCrop.reset();
	updateVideoPixmap(app.video(), optionHorizontalVideoCrop, optionVisibleVideoLines);
	overclock_enabled = 0;
	postrenderscanlines = 0;
	vblankscanlines = 0;
	return true;
}

bool NesSystem::readConfig(ConfigType type, MapIO &io, unsigned key, size_t readSize)
{
	if(type == ConfigType::MAIN)
	{
		switch(key)
		{
			case CFGKEY_FDS_BIOS_PATH:
				return readStringOptionValue(io, readSize, fdsBiosPath);
			case CFGKEY_SPRITE_LIMIT: return optionSpriteLimit.readFromIO(io, readSize);
			case CFGKEY_SOUND_QUALITY: return optionSoundQuality.readFromIO(io, readSize);
			case CFGKEY_DEFAULT_VIDEO_SYSTEM: return optionDefaultVideoSystem.readFromIO(io, readSize);
			case CFGKEY_DEFAULT_PALETTE_PATH:
				return readStringOptionValue(io, readSize, defaultPalettePath);
			case CFGKEY_DEFAULT_SOUND_LOW_PASS_FILTER:
				return readOptionValue<bool>(io, readSize, [](auto val){FCEUI_SetLowPass(val);});
			case CFGKEY_SWAP_DUTY_CYCLES: return readOptionValue(io, readSize, swapDuty);
			case CFGKEY_START_VIDEO_LINE: return optionDefaultStartVideoLine.readFromIO(io, readSize);
			case CFGKEY_VISIBLE_VIDEO_LINES: return optionDefaultVisibleVideoLines.readFromIO(io, readSize);
			case CFGKEY_CORRECT_LINE_ASPECT: return optionCorrectLineAspect.readFromIO(io, readSize);
			case CFGKEY_FF_DURING_FDS_ACCESS: return readOptionValue(io, readSize, fastForwardDuringFdsAccess);
			case CFGKEY_CHEATS_PATH: return readStringOptionValue(io, readSize, cheatsDir);
			case CFGKEY_PATCHES_PATH: return readStringOptionValue(io, readSize, patchesDir);
			case CFGKEY_PALETTE_PATH: return readStringOptionValue(io, readSize, palettesDir);
		}
	}
	else if(type == ConfigType::SESSION)
	{
		switch(key)
		{
			case CFGKEY_FOUR_SCORE: return optionFourScore.readFromIO(io, readSize);
			case CFGKEY_VIDEO_SYSTEM: return optionVideoSystem.readFromIO(io, readSize);
			case CFGKEY_INPUT_PORT_1: return optionInputPort1.readFromIO(io, readSize);
			case CFGKEY_INPUT_PORT_2: return optionInputPort2.readFromIO(io, readSize);
			case CFGKEY_COMPATIBLE_FRAMESKIP: return optionCompatibleFrameskip.readFromIO(io, readSize);
			case CFGKEY_START_VIDEO_LINE: return optionStartVideoLine.readFromIO(io, readSize);
			case CFGKEY_VISIBLE_VIDEO_LINES: return optionVisibleVideoLines.readFromIO(io, readSize);
			case CFGKEY_HORIZONTAL_VIDEO_CROP: return optionHorizontalVideoCrop.readFromIO(io, readSize);
			case CFGKEY_OVERCLOCKING: return readOptionValue<bool>(io, readSize, [&](auto on){overclock_enabled = on;});
			case CFGKEY_OVERCLOCK_EXTRA_LINES: return readOptionValue<int16_t>(io, readSize,
				[&](auto v){if(v >= 0 && v <= maxExtraLinesPerFrame) postrenderscanlines = v;});
			case CFGKEY_OVERCLOCK_VBLANK_MULTIPLIER: return readOptionValue<int8_t>(io, readSize,
				[&](auto v){if(v >= 0 && v <= maxVBlankMultiplier) vblankscanlines = v;});
		}
	}
	return false;
}

void NesSystem::writeConfig(ConfigType type, FileIO &io)
{
	if(type == ConfigType::MAIN)
	{
		optionSpriteLimit.writeWithKeyIfNotDefault(io);
		optionSoundQuality.writeWithKeyIfNotDefault(io);
		writeStringOptionValue(io, CFGKEY_FDS_BIOS_PATH, fdsBiosPath);
		optionDefaultVideoSystem.writeWithKeyIfNotDefault(io);
		writeStringOptionValue(io, CFGKEY_DEFAULT_PALETTE_PATH, defaultPalettePath);
		if(swapDuty)
			writeOptionValue(io, CFGKEY_SWAP_DUTY_CYCLES, swapDuty);
		if(FSettings.lowpass)
			writeOptionValue(io, CFGKEY_DEFAULT_SOUND_LOW_PASS_FILTER, (bool)FSettings.lowpass);
		optionDefaultStartVideoLine.writeWithKeyIfNotDefault(io);
		optionDefaultVisibleVideoLines.writeWithKeyIfNotDefault(io);
		optionCorrectLineAspect.writeWithKeyIfNotDefault(io);
		if(!fastForwardDuringFdsAccess)
			writeOptionValue(io, CFGKEY_FF_DURING_FDS_ACCESS, fastForwardDuringFdsAccess);
		writeStringOptionValue(io, CFGKEY_CHEATS_PATH, cheatsDir);
		writeStringOptionValue(io, CFGKEY_PATCHES_PATH, patchesDir);
		writeStringOptionValue(io, CFGKEY_PALETTE_PATH, palettesDir);
	}
	else if(type == ConfigType::SESSION)
	{
		optionFourScore.writeWithKeyIfNotDefault(io);
		optionVideoSystem.writeWithKeyIfNotDefault(io);
		optionInputPort1.writeWithKeyIfNotDefault(io);
		optionInputPort2.writeWithKeyIfNotDefault(io);
		optionCompatibleFrameskip.writeWithKeyIfNotDefault(io);
		optionStartVideoLine.writeWithKeyIfNotDefault(io);
		optionVisibleVideoLines.writeWithKeyIfNotDefault(io);
		optionHorizontalVideoCrop.writeWithKeyIfNotDefault(io);
		writeOptionValueIfNotDefault(io, CFGKEY_OVERCLOCKING, bool(overclock_enabled), 0);
		writeOptionValueIfNotDefault(io, CFGKEY_OVERCLOCK_EXTRA_LINES, int16_t(postrenderscanlines), 0);
		writeOptionValueIfNotDefault(io, CFGKEY_OVERCLOCK_VBLANK_MULTIPLIER, int8_t(vblankscanlines), 0);
	}
}

}
