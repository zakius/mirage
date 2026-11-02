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

#include <utils/settings/settingspage.h>

#include <QSignalSpy>

#include <gtest/gtest.h>

namespace Fooyin::Testing {
TEST(SettingsPageTest, ReportsWhenItIsHiddenOrShown)
{
    SettingsPage page;
    QSignalSpy hiddenChanged{&page, &SettingsPage::hiddenChanged};
    EXPECT_FALSE(page.isHidden());

    page.setHidden(true);
    page.setHidden(true);
    EXPECT_TRUE(page.isHidden());
    EXPECT_EQ(1, hiddenChanged.count());

    page.setHidden(false);
    EXPECT_FALSE(page.isHidden());
    EXPECT_EQ(2, hiddenChanged.count());
}
} // namespace Fooyin::Testing
