/*
 * Copyright 2013 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *      Martin Preisler <mpreisle@redhat.com>
 */

#include "ScanningSession.h"
#include "ResultViewer.h"
#include "DiagnosticsDialog.h"

extern "C" {
#include <xccdf_policy.h>
#include <xccdf_session.h>
#include <scap_ds.h>
#include <oscap_error.h>
}

#include <ctime>

ScanningSession::ScanningSession(DiagnosticsDialog* diagnosticsDialog, QObject* parent):
    QObject(parent),

    mSession(0),
    mTailoring(0),

    mSessionDirty(false),
    mTailoringUserChanges(false),

    mDiagnosticsDialog(diagnosticsDialog)
{
    mTailoringFile.setAutoRemove(true);
}

ScanningSession::~ScanningSession()
{
    closeFile();
}

struct xccdf_session* ScanningSession::getXCCDFSession() const
{
    reloadSession();
    return mSession;
}

bool ScanningSession::fileOpened() const
{
    return mSession != 0;
}

bool ScanningSession::profileSelected() const
{
    if (!fileOpened())
        return false;

    reloadSession();
    return xccdf_session_get_profile_id(mSession) != 0;
}

bool ScanningSession::isSelectedProfileTailoring() const
{
    if (!fileOpened())
        return false;

    reloadSession();

    struct xccdf_policy_model* policyModel = xccdf_session_get_policy_model(mSession);
    if (!policyModel)
        return false;

    struct xccdf_policy* policy= xccdf_session_get_xccdf_policy(mSession);
    if (!policy)
        return false;

    struct xccdf_profile* profile = xccdf_policy_get_profile(policy);
    if (!profile)
        return false;

    return xccdf_profile_get_tailoring(profile);
}

void ScanningSession::openFile(const QString& path)
{
    if (mSession)
        closeFile();

    mSession = xccdf_session_new(path.toUtf8().constData());
    if (!mSession)
    {
        mDiagnosticsDialog->errorMessage(
            QString("Failed to create session for '%1'. OpenSCAP error message:\n%2").arg(path).arg(oscap_err_desc()));
        return;
    }

    mSessionDirty = true;
    mTailoringUserChanges = false;

    // set default profile after opening, this ensures that xccdf_policy can be returned
    setProfileID(QString());

    mDiagnosticsDialog->infoMessage(QString("Opened file '%1'.").arg(path));
}

void ScanningSession::closeFile()
{
    const QString oldOpenedFile = mSession ? xccdf_session_get_filename(mSession) : "";

    if (mSession)
    {
        // session "owns" mTailoring and will free it as part of xccdf_policy_model
        xccdf_session_free(mSession);

        mSession = 0;
        mTailoring = 0;

        mSessionDirty = false;
        mTailoringUserChanges = false;
    }

    if (!oldOpenedFile.isEmpty())
        mDiagnosticsDialog->infoMessage(QString("Closed file '%1'.").arg(oldOpenedFile));
}

bool ScanningSession::isSDS() const
{
    if (!fileOpened())
        return false;

    reloadSession();
    return xccdf_session_is_sds(mSession);
}

void ScanningSession::setDatastreamID(const QString& datastreamID)
{
    if (datastreamID.isEmpty())
        xccdf_session_set_datastream_id(mSession, 0);
    else
        xccdf_session_set_datastream_id(mSession, datastreamID.toUtf8().constData());

    mSessionDirty = true;
}

void ScanningSession::setComponentID(const QString& componentID)
{
    if (componentID.isEmpty())
        xccdf_session_set_component_id(mSession, 0);
    else
        xccdf_session_set_component_id(mSession, componentID.toUtf8().constData());

    mSessionDirty = true;
}

void ScanningSession::ensureTailoringExists()
{
    reloadSession();

    if (!mTailoring)
    {
        mTailoring = xccdf_tailoring_new();
        xccdf_tailoring_set_version(mTailoring, "1");

        {
            time_t rawtime;
            struct tm* timeinfo;
            char buffer[80];

            time(&rawtime);
            timeinfo = localtime(&rawtime);

            strftime(buffer, 80, "%Y-%m-%dT%H:%M:%S", timeinfo);

            xccdf_tailoring_set_version_time(mTailoring, buffer);
        }

        mTailoringUserChanges = true;
        reloadSession(true);
    }
}

void ScanningSession::resetTailoring()
{
    if (!fileOpened())
        return;

    xccdf_session_set_user_tailoring_cid(mSession, 0);
    xccdf_session_set_user_tailoring_file(mSession, 0);

    mTailoring = 0;

    mSessionDirty = true;
    mTailoringUserChanges = false;
}

void ScanningSession::setTailoringFile(const QString& tailoringFile)
{
    if (!fileOpened())
        return;

    xccdf_session_set_user_tailoring_cid(mSession, 0);
    xccdf_session_set_user_tailoring_file(mSession, tailoringFile.toUtf8().constData());

    mTailoring = 0;

    mSessionDirty = true;
    mTailoringUserChanges = false;
}

void ScanningSession::setTailoringComponentID(const QString& componentID)
{
    if (!fileOpened())
        return;

    xccdf_session_set_user_tailoring_file(mSession, 0);
    xccdf_session_set_user_tailoring_cid(mSession, componentID.toUtf8().constData());

    mTailoring = 0;

    mSessionDirty = true;
    mTailoringUserChanges = false;
}

bool ScanningSession::setProfileID(const QString& profileID)
{
    if (!fileOpened())
        return false;

    reloadSession();
    return xccdf_session_set_profile_id(mSession, profileID.isEmpty() ? NULL : profileID.toUtf8().constData());
}

void ScanningSession::reloadSession(bool forceReload) const
{
    if (!mSession)
        return;

    if (mSessionDirty || forceReload)
    {
        if (xccdf_session_load(mSession) != 0)
        {
            mDiagnosticsDialog->errorMessage(
                QString("Failed to reload session. OpenSCAP error message:\n%1").arg(oscap_err_desc()));
        }
        else
        {
            struct xccdf_policy_model* policyModel = xccdf_session_get_policy_model(mSession);

            // In case we didn't have any tailoring previously, lets use the one from the session.
            // Otherwise we will reuse our own tailoring instead of the session's because we may
            // already have user-made changes in it!

            if (!mTailoringUserChanges)
                mTailoring = xccdf_policy_model_get_tailoring(policyModel);
            else
                xccdf_policy_model_set_tailoring(policyModel, mTailoring);

            mSessionDirty = false;
        }
    }
}

QString ScanningSession::getInputFile() const
{
    if (!mSession)
        return "";

    return xccdf_session_get_filename(mSession);
}

struct xccdf_profile* ScanningSession::tailorCurrentProfile(bool shadowed)
{
    reloadSession();

    if (!mSession)
        return 0;

    // create a new profile, inheriting the currently selected profile
    // or no profile if currently selected profile is the '(default profile)'
    struct xccdf_policy_model* policyModel = xccdf_session_get_policy_model(mSession);
    if (!policyModel)
        return 0;

    struct xccdf_profile* newProfile = xccdf_profile_new();

    struct xccdf_policy* policy= xccdf_session_get_xccdf_policy(mSession);
    struct xccdf_profile* oldProfile = policy ? xccdf_policy_get_profile(policy) : 0;

    // TODO: new profile's ID may clash with existing profile!
    if (oldProfile)
    {
        xccdf_profile_set_extends(newProfile, xccdf_profile_get_id(oldProfile));

        if (shadowed)
        {
            xccdf_profile_set_id(newProfile, xccdf_profile_get_id(oldProfile));
        }
        else
        {
            QString newId = QString(xccdf_profile_get_id(oldProfile)) + QString("_tailored");
            xccdf_profile_set_id(newProfile, newId.toUtf8().constData());
        }

        struct oscap_text_iterator* titles = xccdf_profile_get_title(oldProfile);
        while (oscap_text_iterator_has_more(titles))
        {
            struct oscap_text* oldTitle = oscap_text_iterator_next(titles);
            struct oscap_text* newTitle = oscap_text_clone(oldTitle);

            oscap_text_set_text(newTitle, (QString(oscap_text_get_text(oldTitle)) + QString(" tailored")).toUtf8().constData());
            xccdf_profile_add_title(newProfile, newTitle);
        }
    }
    else
    {
        xccdf_profile_set_id(newProfile, "xccdf_profile_default_tailored");

        struct oscap_text* newTitle = oscap_text_new();
        oscap_text_set_lang(newTitle, "en_US");
        oscap_text_set_text(newTitle, "(default profile) tailored");
        xccdf_profile_add_title(newProfile, newTitle);
    }

    ensureTailoringExists();

    if (!xccdf_tailoring_add_profile(mTailoring, newProfile))
    {
        mDiagnosticsDialog->errorMessage("Failed to add a newly created profile for tailoring!");
        xccdf_profile_free(xccdf_profile_to_item((newProfile)));

        return 0;
    }

    mTailoringUserChanges = true;
    return newProfile;
}

QString ScanningSession::getTailoringFile()
{
    ensureTailoringExists();

    if (mTailoringFile.isOpen())
        mTailoringFile.close();

    mTailoringFile.open();
    mTailoringFile.close();

    struct xccdf_benchmark* benchmark = getXCCDFInputBenchmark();
    xccdf_tailoring_export(mTailoring, mTailoringFile.fileName().toUtf8().constData(), xccdf_benchmark_get_schema_version(benchmark));

    return mTailoringFile.fileName();
}

bool ScanningSession::hasTailoring() const
{
    return mTailoring != NULL;
}

struct xccdf_benchmark* ScanningSession::getXCCDFInputBenchmark()
{
    reloadSession();

    if (!mSession)
        return NULL;

    struct xccdf_policy_model* policyModel = xccdf_session_get_policy_model(mSession);
    return xccdf_policy_model_get_benchmark(policyModel);
}