// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

struct EmuEnvState;
struct NetState;

bool init(NetState &state);

// Starts the ASBR connector hello when the running title is a retail PlayStation All-Stars Battle Royale id,
// whether connector mode is on or off (see net/psas_connector.h). NetState::deinit stops it.
void psas_start_hello(EmuEnvState &emuenv);
