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

#include "gui/settings/shortcuts/shortcutsmodel.h"

#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/mirage.h>
#include <utils/settings/settingsmanager.h>

#include <QAction>
#include <QApplication>
#include <QMainWindow>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
QStringList listedNames(const QAbstractItemModel& model, const QModelIndex& parent = {})
{
    QStringList names;
    for(int row{0}; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        names.append(index.data().toString());
        names.append(listedNames(model, index));
    }
    return names;
}
} // namespace

TEST(ShortcutsModelTest, LeavesOutCommandsHiddenInTheMenus)
{
    QTemporaryDir dir;
    SettingsManager settings{dir.filePath(u"settings.ini"_s)};
    QMainWindow window;
    ActionManager actionManager{&settings};
    actionManager.setMainWindow(&window);

    QAction plain{u"Plain"_s};
    actionManager.registerAction(&plain, "Test.Plain")->setDescription(u"Plain"_s);
    QAction addFiles{u"Add files"_s};
    actionManager.registerAction(&addFiles, "File.AddFiles")->setDescription(u"Add files"_s);
    QAction upload{u"Upload"_s};
    upload.setProperty(Mirage::ShortcutFollowsVisibility, true);
    upload.setVisible(false);
    actionManager.registerAction(&upload, "Subsonic.Upload")->setDescription(u"Upload"_s);

    ShortcutsModel model;
    QObject::connect(&model, &ShortcutsModel::visibilityChanged, &model,
                     [&model, &actionManager]() { model.populate(&actionManager); });
    model.populate(&actionManager);
    EXPECT_TRUE(listedNames(model).contains(u"Plain"_s));
    EXPECT_FALSE(listedNames(model).contains(u"Add files"_s));
    EXPECT_FALSE(listedNames(model).contains(u"Upload"_s));

    // A server offering uploads appears
    upload.setVisible(true);
    QApplication::processEvents();
    EXPECT_TRUE(listedNames(model).contains(u"Upload"_s));

    upload.setVisible(false);
    QApplication::processEvents();
    EXPECT_FALSE(listedNames(model).contains(u"Upload"_s));
}
} // namespace Fooyin::Testing

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_WIN
        qputenv("QT_QPA_PLATFORM", "windows");
#else
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }

    const QApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
