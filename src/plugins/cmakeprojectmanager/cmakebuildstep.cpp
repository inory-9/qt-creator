/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "cmakebuildstep.h"

#include "cmakebuildconfiguration.h"
#include "cmakebuildsystem.h"
#include "cmakekitinformation.h"
#include "cmakeparser.h"
#include "cmakeprojectconstants.h"
#include "cmaketool.h"

#include <coreplugin/find/itemviewfind.h>
#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/gnumakeparser.h>
#include <projectexplorer/processparameters.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/runconfiguration.h>
#include <projectexplorer/target.h>

#include <utils/algorithm.h>

#include <QBoxLayout>
#include <QListWidget>
#include <QRegularExpression>

using namespace ProjectExplorer;
using namespace Utils;

namespace CMakeProjectManager {
namespace Internal {

const char BUILD_TARGETS_KEY[] = "CMakeProjectManager.MakeStep.BuildTargets";
const char CMAKE_ARGUMENTS_KEY[] = "CMakeProjectManager.MakeStep.CMakeArguments";
const char TOOL_ARGUMENTS_KEY[] = "CMakeProjectManager.MakeStep.AdditionalArguments";
const char ADD_RUNCONFIGURATION_ARGUMENT_KEY[] = "CMakeProjectManager.MakeStep.AddRunConfigurationArgument";
const char ADD_RUNCONFIGURATION_TEXT[] = "Current executable";

class CmakeProgressParser : public Utils::OutputLineParser
{
    Q_OBJECT

signals:
    void progress(int percentage);

private:
    Result handleLine(const QString &line, Utils::OutputFormat format) override
    {
        if (format != Utils::StdOutFormat)
            return Status::NotHandled;

        static const QRegularExpression percentProgress("^\\[\\s*(\\d*)%\\]");
        static const QRegularExpression ninjaProgress("^\\[\\s*(\\d*)/\\s*(\\d*)");

        QRegularExpressionMatch match = percentProgress.match(line);
        if (match.hasMatch()) {
            bool ok = false;
            const int percent = match.captured(1).toInt(&ok);
            if (ok)
                emit progress(percent);
            return Status::Done;
        }
        match = ninjaProgress.match(line);
        if (match.hasMatch()) {
            m_useNinja = true;
            bool ok = false;
            const int done = match.captured(1).toInt(&ok);
            if (ok) {
                const int all = match.captured(2).toInt(&ok);
                if (ok && all != 0) {
                    const int percent = static_cast<int>(100.0 * done / all);
                    emit progress(percent);
                }
            }
            return Status::Done;
        }
        return Status::NotHandled;
    }
    bool hasDetectedRedirection() const override { return m_useNinja; }

    // TODO: Shouldn't we know the backend in advance? Then we could merge this class
    //       with CmakeParser.
    bool m_useNinja = false;
};

class CMakeBuildStepConfigWidget : public BuildStepConfigWidget
{
    Q_DECLARE_TR_FUNCTIONS(CMakeProjectManager::Internal::CMakeBuildStepConfigWidget)

public:
    explicit CMakeBuildStepConfigWidget(CMakeBuildStep *buildStep);

private:
    void itemsChanged();
    void updateDetails();
    void buildTargetsChanged();
    void updateBuildTargets();

    CMakeBuildStep *m_buildStep;
    QListWidget *m_buildTargetsList;
};

static bool isCurrentExecutableTarget(const QString &target)
{
    return target == ADD_RUNCONFIGURATION_TEXT;
}

CMakeBuildStep::CMakeBuildStep(BuildStepList *bsl, Utils::Id id) :
    AbstractProcessStep(bsl, id)
{
    //: Default display name for the cmake make step.
    setDefaultDisplayName(tr("CMake Build"));

    m_cmakeArguments = addAspect<StringAspect>();
    m_cmakeArguments->setSettingsKey(CMAKE_ARGUMENTS_KEY);
    m_cmakeArguments->setLabelText(tr("CMake arguments:"));
    m_cmakeArguments->setDisplayStyle(StringAspect::LineEditDisplay);

    m_toolArguments = addAspect<StringAspect>();
    m_toolArguments->setSettingsKey(TOOL_ARGUMENTS_KEY);
    m_toolArguments->setLabelText(tr("Tool arguments:"));
    m_toolArguments->setDisplayStyle(StringAspect::LineEditDisplay);

    // Set a good default build target:
    if (m_buildTargets.isEmpty())
        setBuildTargets({defaultBuildTarget()});

    setLowPriority();

    setEnvironmentModifier([](Environment &env) {
        const QString ninjaProgressString = "[%f/%t "; // ninja: [33/100
        Environment::setupEnglishOutput(&env);
        if (!env.expandedValueForKey("NINJA_STATUS").startsWith(ninjaProgressString))
            env.set("NINJA_STATUS", ninjaProgressString + "%o/sec] ");
    });

    connect(target(), &Target::parsingFinished,
            this, &CMakeBuildStep::handleBuildTargetsChanges);
}

CMakeBuildConfiguration *CMakeBuildStep::cmakeBuildConfiguration() const
{
    return static_cast<CMakeBuildConfiguration *>(buildConfiguration());
}

void CMakeBuildStep::handleBuildTargetsChanges(bool success)
{
    if (!success)
        return; // Do not change when parsing failed.
    const QStringList results = Utils::filtered(m_buildTargets, [this](const QString &s) {
        return knownBuildTargets().contains(s);
    });
    if (results.isEmpty())
        setBuildTargets({defaultBuildTarget()});
    else {
        setBuildTargets(results);
    }
    emit buildTargetsChanged();
}

QVariantMap CMakeBuildStep::toMap() const
{
    QVariantMap map(AbstractProcessStep::toMap());
    // Use QStringList for compatibility with old files
    map.insert(BUILD_TARGETS_KEY, QStringList(m_buildTargets));
    return map;
}

bool CMakeBuildStep::fromMap(const QVariantMap &map)
{
    m_buildTargets = map.value(BUILD_TARGETS_KEY).toStringList();
    if (map.value(ADD_RUNCONFIGURATION_ARGUMENT_KEY, false).toBool())
        m_buildTargets = QStringList(ADD_RUNCONFIGURATION_TEXT);

    return BuildStep::fromMap(map);
}

bool CMakeBuildStep::init()
{
    bool canInit = true;
    CMakeBuildConfiguration *bc = cmakeBuildConfiguration();
    QTC_ASSERT(bc, return false);
    if (!bc->isEnabled()) {
        emit addTask(BuildSystemTask(Task::Error,
                                     tr("The build configuration is currently disabled.")));
        canInit = false;
    }

    CMakeTool *tool = CMakeKitAspect::cmakeTool(kit());
    if (!tool || !tool->isValid()) {
        emit addTask(BuildSystemTask(Task::Error,
                          tr("A CMake tool must be set up for building. "
                             "Configure a CMake tool in the kit options.")));
        canInit = false;
    }

    RunConfiguration *rc =  target()->activeRunConfiguration();
    const bool buildCurrent = Utils::contains(m_buildTargets, [](const QString &s) { return isCurrentExecutableTarget(s); });
    if (buildCurrent && (!rc || rc->buildKey().isEmpty())) {
        emit addTask(BuildSystemTask(Task::Error,
                          QCoreApplication::translate("ProjectExplorer::Task",
                                    "You asked to build the current Run Configuration's build target only, "
                                    "but it is not associated with a build target. "
                                    "Update the Make Step in your build settings.")));
        canInit = false;
    }

    if (!canInit) {
        emitFaultyConfigurationMessage();
        return false;
    }

    // Warn if doing out-of-source builds with a CMakeCache.txt is the source directory
    const Utils::FilePath projectDirectory = bc->target()->project()->projectDirectory();
    if (bc->buildDirectory() != projectDirectory) {
        if (projectDirectory.pathAppended("CMakeCache.txt").exists()) {
            emit addTask(BuildSystemTask(Task::Warning,
                              tr("There is a CMakeCache.txt file in \"%1\", which suggest an "
                                 "in-source build was done before. You are now building in \"%2\", "
                                 "and the CMakeCache.txt file might confuse CMake.")
                              .arg(projectDirectory.toUserOutput(), bc->buildDirectory().toUserOutput())));
        }
    }

    setIgnoreReturnValue(m_buildTargets == QStringList(CMakeBuildStep::cleanTarget()));

    ProcessParameters *pp = processParameters();
    setupProcessParameters(pp);
    pp->setCommandLine(cmakeCommand(rc));

    return AbstractProcessStep::init();
}

void CMakeBuildStep::setupOutputFormatter(Utils::OutputFormatter *formatter)
{
    CMakeParser *cmakeParser = new CMakeParser;
    CmakeProgressParser * const progressParser = new CmakeProgressParser;
    connect(progressParser, &CmakeProgressParser::progress, this, [this](int percent) {
        emit progress(percent, {});
    });
    formatter->addLineParser(progressParser);
    cmakeParser->setSourceDirectory(project()->projectDirectory().toString());
    formatter->addLineParsers({cmakeParser, new GnuMakeParser});
    const QList<Utils::OutputLineParser *> additionalParsers = kit()->createOutputParsers();
    for (Utils::OutputLineParser * const p : additionalParsers)
        p->setRedirectionDetector(progressParser);
    formatter->addLineParsers(additionalParsers);
    formatter->addSearchDir(processParameters()->effectiveWorkingDirectory());
    AbstractProcessStep::setupOutputFormatter(formatter);
}

void CMakeBuildStep::doRun()
{
    // Make sure CMake state was written to disk before trying to build:
    m_waiting = false;
    auto bs = static_cast<CMakeBuildSystem *>(buildSystem());
    if (bs->persistCMakeState()) {
        emit addOutput(tr("Persisting CMake state..."), BuildStep::OutputFormat::NormalMessage);
        m_waiting = true;
    } else if (buildSystem()->isWaitingForParse()) {
        emit addOutput(tr("Running CMake in preparation to build..."), BuildStep::OutputFormat::NormalMessage);
        m_waiting = true;
    }

    if (m_waiting) {
        m_runTrigger = connect(target(), &Target::parsingFinished,
                               this, [this](bool success) { handleProjectWasParsed(success); });
    } else {
        runImpl();
    }
}

void CMakeBuildStep::runImpl()
{
    // Do the actual build:
    AbstractProcessStep::doRun();
}

void CMakeBuildStep::handleProjectWasParsed(bool success)
{
    m_waiting = false;
    disconnect(m_runTrigger);
    if (isCanceled()) {
        emit finished(false);
    } else if (success) {
        runImpl();
    } else {
        AbstractProcessStep::stdError(tr("Project did not parse successfully, cannot build."));
        emit finished(false);
    }
}

BuildStepConfigWidget *CMakeBuildStep::createConfigWidget()
{
    return new CMakeBuildStepConfigWidget(this);
}

QString CMakeBuildStep::defaultBuildTarget() const
{
    const BuildStepList *const bsl = stepList();
    QTC_ASSERT(bsl, return {});
    const Utils::Id parentId = bsl->id();
    if (parentId == ProjectExplorer::Constants::BUILDSTEPS_CLEAN)
        return cleanTarget();
    if (parentId == ProjectExplorer::Constants::BUILDSTEPS_DEPLOY)
        return installTarget();
    return allTarget();
}

QStringList CMakeBuildStep::buildTargets() const
{
    return m_buildTargets;
}

bool CMakeBuildStep::buildsBuildTarget(const QString &target) const
{
    return m_buildTargets.contains(target);
}

void CMakeBuildStep::setBuildTargets(const QStringList &buildTargets)
{
    if (m_buildTargets == buildTargets)
        return;
    m_buildTargets = buildTargets;
    emit targetsToBuildChanged();
}

Utils::CommandLine CMakeBuildStep::cmakeCommand(RunConfiguration *rc) const
{
    CMakeTool *tool = CMakeKitAspect::cmakeTool(kit());

    Utils::CommandLine cmd(tool ? tool->cmakeExecutable() : Utils::FilePath(), {});
    cmd.addArgs({"--build", "."});

    cmd.addArg("--target");
    cmd.addArgs(Utils::transform(m_buildTargets, [rc](const QString &s) {
            QString target = s;
        if (isCurrentExecutableTarget(s)) {
            if (rc) {
                target = rc->buildKey();
                const int pos = target.indexOf("///::///");
                if (pos >= 0) {
                    target = target.mid(pos + 8);
                }
            } else {
                target = "<i>&lt;" + tr(ADD_RUNCONFIGURATION_TEXT) + "&gt;</i>";
            }
        }
        return target;
    }));

    if (!m_cmakeArguments->value().isEmpty())
        cmd.addArgs(m_cmakeArguments->value(), CommandLine::Raw);

    if (!m_toolArguments->value().isEmpty()) {
        cmd.addArg("--");
        cmd.addArgs(m_toolArguments->value(), CommandLine::Raw);
    }

    return cmd;
}

QStringList CMakeBuildStep::knownBuildTargets()
{
    auto bs = qobject_cast<CMakeBuildSystem *>(buildSystem());
    return bs ? bs->buildTargetTitles() : QStringList();
}

QString CMakeBuildStep::cleanTarget()
{
    return QString("clean");
}

QString CMakeBuildStep::allTarget()
{
    return QString("all");
}

QString CMakeBuildStep::installTarget()
{
    return QString("install");
}

QString CMakeBuildStep::testTarget()
{
    return QString("test");
}

QStringList CMakeBuildStep::specialTargets()
{
    return { allTarget(), cleanTarget(), installTarget(), testTarget() };
}

//
// CMakeBuildStepConfigWidget
//

CMakeBuildStepConfigWidget::CMakeBuildStepConfigWidget(CMakeBuildStep *buildStep)
    : BuildStepConfigWidget(buildStep)
    , m_buildStep(buildStep)
    , m_buildTargetsList(new QListWidget)
{
    setDisplayName(tr("Build", "CMakeProjectManager::CMakeBuildStepConfigWidget display name."));

    LayoutBuilder builder(this);
    builder.addRow(buildStep->m_cmakeArguments);
    builder.addRow(buildStep->m_toolArguments);

    m_buildTargetsList->setFrameStyle(QFrame::NoFrame);
    m_buildTargetsList->setMinimumHeight(200);

    auto frame = new QFrame(this);
    frame->setFrameStyle(QFrame::StyledPanel);
    auto frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->addWidget(Core::ItemViewFind::createSearchableWrapper(m_buildTargetsList,
                                                                       Core::ItemViewFind::LightColored));

    builder.startNewRow().addItems(tr("Targets:"), frame);

    buildTargetsChanged();
    updateDetails();

    connect(buildStep->m_cmakeArguments, &StringAspect::changed,
            this, &CMakeBuildStepConfigWidget::updateDetails);
    connect(buildStep->m_toolArguments, &StringAspect::changed,
            this, &CMakeBuildStepConfigWidget::updateDetails);

    connect(m_buildTargetsList, &QListWidget::itemChanged,
            this, &CMakeBuildStepConfigWidget::itemsChanged);
    connect(ProjectExplorerPlugin::instance(), &ProjectExplorerPlugin::settingsChanged,
            this, &CMakeBuildStepConfigWidget::updateDetails);

    connect(m_buildStep,
            &CMakeBuildStep::buildTargetsChanged,
            this,
            &CMakeBuildStepConfigWidget::buildTargetsChanged);

    connect(m_buildStep,
            &CMakeBuildStep::targetsToBuildChanged,
            this,
            &CMakeBuildStepConfigWidget::updateBuildTargets);

    connect(m_buildStep->buildConfiguration(),
            &BuildConfiguration::environmentChanged,
            this,
            &CMakeBuildStepConfigWidget::updateDetails);
}

void CMakeBuildStepConfigWidget::itemsChanged()
{
    const QList<QListWidgetItem *> items = [this]() {
        QList<QListWidgetItem *> items;
        for (int row = 0; row < m_buildTargetsList->count(); ++row)
            items.append(m_buildTargetsList->item(row));
        return items;
    }();
    const QStringList targetsToBuild = Utils::transform(Utils::filtered(items, Utils::equal(&QListWidgetItem::checkState, Qt::Checked)),
                                                        [](const QListWidgetItem *i) { return i->data(Qt::UserRole).toString(); });
    m_buildStep->setBuildTargets(targetsToBuild);
    updateDetails();
}

void CMakeBuildStepConfigWidget::buildTargetsChanged()
{
    {
        QFont italics;
        italics.setItalic(true);

        auto addItem = [italics, this](const QString &buildTarget, const QString &displayName, bool special = false) {
            auto item = new QListWidgetItem(displayName, m_buildTargetsList);
            item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            item->setData(Qt::UserRole, buildTarget);
            if (special)
                item->setFont(italics);
        };

        QSignalBlocker blocker(m_buildTargetsList);
        m_buildTargetsList->clear();

        QStringList targetList = m_buildStep->knownBuildTargets();
        targetList.sort();

        addItem(ADD_RUNCONFIGURATION_TEXT, tr(ADD_RUNCONFIGURATION_TEXT), true);

        foreach (const QString &buildTarget, targetList)
            addItem(buildTarget, buildTarget, CMakeBuildStep::specialTargets().contains(buildTarget));

        updateBuildTargets();
    }
    updateDetails();
}

void CMakeBuildStepConfigWidget::updateBuildTargets()
{
    const QStringList buildTargets = m_buildStep->buildTargets();
    {
        QSignalBlocker blocker(m_buildTargetsList);
        for (int row = 0; row < m_buildTargetsList->count(); ++row) {
            QListWidgetItem *item = m_buildTargetsList->item(row);
            const QString title = item->data(Qt::UserRole).toString();

            item->setCheckState(m_buildStep->buildsBuildTarget(title) ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateDetails();
}

void CMakeBuildStepConfigWidget::updateDetails()
{
    ProcessParameters param;
    m_buildStep->setupProcessParameters(&param);
    param.setCommandLine(m_buildStep->cmakeCommand(nullptr));

    setSummaryText(param.summary(displayName()));
}

//
// CMakeBuildStepFactory
//

CMakeBuildStepFactory::CMakeBuildStepFactory()
{
    registerStep<CMakeBuildStep>(Constants::CMAKE_BUILD_STEP_ID);
    setDisplayName(CMakeBuildStep::tr("Build", "Display name for CMakeProjectManager::CMakeBuildStep id."));
    setSupportedProjectType(Constants::CMAKE_PROJECT_ID);
}

void CMakeBuildStep::processFinished(int exitCode, QProcess::ExitStatus status)
{
    AbstractProcessStep::processFinished(exitCode, status);
    emit progress(100, QString());
}

} // Internal
} // CMakeProjectManager

#include <cmakebuildstep.moc>
