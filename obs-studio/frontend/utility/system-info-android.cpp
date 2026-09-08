/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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

#include "system-info.hpp"

#include <util/platform.h>

#include <sys/system_properties.h>
#include <sys/utsname.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace std;

static string read_property(const char *key)
{
	char value[PROP_VALUE_MAX] = {};
	if (__system_property_get(key, value) <= 0) {
		return string();
	}

	return string(value);
}

static bool read_cpu_max_speed(uint32_t &speed_mhz)
{
	FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
	if (!fp) {
		return false;
	}

	unsigned int khz = 0;
	const bool parsed = (fscanf(fp, "%u", &khz) == 1) && (khz > 0);
	fclose(fp);

	if (!parsed) {
		return false;
	}

	speed_mhz = khz / 1000U;
	return true;
}

void system_info(GoLiveApi::Capabilities &capabilities)
{
	/* GPU 能力这里留空：Linux 版靠 libpci 扫 /sys/class/drm 的 card*，Android 上既没有
	 * libpci 也没有那条 sysfs 路径，NDK 侧也没有稳定的 GPU 型号查询接口。
	 * 等阶段 3 之后要报 GPU，就从 EGL 侧取（S1 已经证明能拿到 GLES 上下文）。 */

	auto &cpu_data = capabilities.cpu;
	cpu_data.physical_cores = os_get_physical_cores();
	cpu_data.logical_cores = os_get_logical_cores();

	string cpu_name = read_property("ro.hardware");
	if (cpu_name.empty()) {
		cpu_name = read_property("ro.board.platform");
	}
	if (cpu_name.empty()) {
		cpu_name = "Unknown";
	}
	cpu_data.name = cpu_name;

	uint32_t cpu_freq;
	if (read_cpu_max_speed(cpu_freq)) {
		cpu_data.speed = cpu_freq;
	}

	auto &memory_data = capabilities.memory;
	memory_data.total = os_get_sys_total_size();
	memory_data.free = os_get_sys_free_size();

	auto &system_data = capabilities.system;
	system_data.name = "Android";
	system_data.release = read_property("ro.build.version.release");
	system_data.revision = read_property("ro.build.version.incremental");

	if (const string sdk = read_property("ro.build.version.sdk"); !sdk.empty()) {
		system_data.build = stoi(sdk);
	}

	struct utsname utsinfo;
	if (uname(&utsinfo) == 0) {
		/* 与 Linux 版同一套判定：机器名里有 "64" 就是 64 位，有 "aarch" 就是 ARM。 */
		system_data.bits = strstr(utsinfo.machine, "64") ? 64 : 32;
		system_data.arm = strstr(utsinfo.machine, "aarch") ? true : false;

		system_data.version = utsinfo.sysname;
		system_data.version.append(" ");
		system_data.version.append(utsinfo.release);
		system_data.version.append(" ");
		system_data.version.append(utsinfo.version);
	} else {
		system_data.bits = 0;
		system_data.arm = false;
		system_data.version = "unknown";
	}

	/* 注意：Houdini（x86_64 设备上跑 arm64 包）这里会报 false ——
	 * os_get_emulation_status() 在 platform-nix.c:390 是个恒返回 false 的占位实现，
	 * 真值只有 Windows 版有。MuMu 上验 arm64 ABI 时别把这个字段当证据。 */
	system_data.armEmulation = os_get_emulation_status();
}
