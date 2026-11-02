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

#include "core/database/database.h"

#include <utils/fypaths.h>

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
void ensureApplication()
{
    QStandardPaths::setTestModeEnabled(true);
    if(!QCoreApplication::instance()) {
        static int argc{1};
        static char name[]  = "mirage-database-test";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);
        QCoreApplication::setApplicationName(QString::fromLatin1(name));
    }
}
} // namespace

#ifdef Q_OS_UNIX
TEST(DatabaseTest, OnlyTheUserMayReadTheDatabase)
{
    ensureApplication();
    const QString path = Utils::sharePath() + u"/fooyin.db"_s;
    QFile::remove(path);

    constexpr auto OwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser
                             | QFileDevice::WriteUser;
    {
        const Database database;
        EXPECT_EQ(Database::Status::Ok, database.status());
        EXPECT_EQ(OwnerOnly, QFile::permissions(path));
    }

    // A database from an earlier build that others could read
    QFile::setPermissions(path, OwnerOnly | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    {
        const Database database;
        EXPECT_EQ(OwnerOnly, QFile::permissions(path));
    }
    QFile::remove(path);
}
#endif
} // namespace Fooyin::Testing
