/*
 * Fooyin
 * Copyright © 2026, zakius <zakius.rolkash@gmail.com>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "fycore_export.h"

#include <QString>

#include <cstdint>

namespace Fooyin::Scripting {
//! How $display_name() shows names with a romanized form.
enum class NameDisplay : uint8_t
{
    Original = 0,
    Romanized,
    Both,
};

//! Returns @p original as @p display asks; a missing romanized form shows the original.
FYCORE_EXPORT QString displayName(const QString& original, const QString& romanized, NameDisplay display);

//! Sets how $display_name() shows names. It shows originals until this is called; safe from any thread.
FYCORE_EXPORT void setNameDisplay(NameDisplay display);
//! Returns the display set with setNameDisplay().
FYCORE_EXPORT NameDisplay nameDisplay();
} // namespace Fooyin::Scripting
