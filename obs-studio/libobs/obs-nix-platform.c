/******************************************************************************
    Copyright (C) 2019 by Jason Francis <cycl0ps@tuta.io>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "obs-nix-platform.h"

#include <assert.h>

#if defined(__ANDROID__)
/* 桌面 Linux 上前端在启动时一定会调 obs_set_nix_platform()，所以这里的 X11_EGL
 * 只是个会被立刻覆盖的占位默认值。Android 没有 X11/Wayland，OBSApp.cpp 里那段
 * 平台探测在 __ANDROID__ 下整块不编译（它要用 QX11Application，Android 的 Qt
 * 套件里没有），于是这个默认值会一路留下来 —— 让 OBSBasic 的 isWayland 判断和
 * importers/studio.cpp 的 X11 分支都读到假答案。改成显式 INVALID，两处都会正确
 * 走非 X11 分支。obs_set_nix_platform() 里的 assert 不受影响，Android 没人调它。 */
static enum obs_nix_platform_type obs_nix_platform = OBS_NIX_PLATFORM_INVALID;
#else
static enum obs_nix_platform_type obs_nix_platform = OBS_NIX_PLATFORM_X11_EGL;
#endif

static void *obs_nix_platform_display = NULL;

void obs_set_nix_platform(enum obs_nix_platform_type platform)
{
	assert(platform != OBS_NIX_PLATFORM_INVALID);
	obs_nix_platform = platform;
}

enum obs_nix_platform_type obs_get_nix_platform(void)
{
	return obs_nix_platform;
}

void obs_set_nix_platform_display(void *display)
{
	obs_nix_platform_display = display;
}

void *obs_get_nix_platform_display(void)
{
	return obs_nix_platform_display;
}
