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

#include <core/scripting/displayname.h>

#include <atomic>

using namespace Qt::StringLiterals;

namespace {
//! Read by script workers.
std::atomic<Fooyin::Scripting::NameDisplay> currentDisplay{Fooyin::Scripting::NameDisplay::Original};
} // namespace

namespace Fooyin::Scripting {
QString displayName(const QString& original, const QString& romanized, NameDisplay display)
{
    if(original.isEmpty() || romanized.isEmpty() || romanized == original || display == NameDisplay::Original) {
        return original;
    }
    return display == NameDisplay::Romanized ? romanized : u"%1 (%2)"_s.arg(original, romanized);
}

void setNameDisplay(NameDisplay display)
{
    currentDisplay = display;
}

NameDisplay nameDisplay()
{
    return currentDisplay.load();
}
} // namespace Fooyin::Scripting
