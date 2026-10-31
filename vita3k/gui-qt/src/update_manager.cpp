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

#include <gui-qt/update_manager.h>

#include <config/version.h>
#include <gui-qt/qt_utils.h>
#include <updater/functions.h>
#include <util/net_utils.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace {

QString tr_update(const char *source_text) {
    return QCoreApplication::translate("UpdateManager", source_text);
}

QDateTime parse_published_at(const std::string &published_at) {
    return QDateTime::fromString(QString::fromStdString(published_at), Qt::ISODate);
}

QString format_published_at(const std::string &published_at) {
    const QDateTime parsed = parse_published_at(published_at);
    if (!parsed.isValid())
        return published_at.empty() ? tr_update("Unknown") : QString::fromStdString(published_at);

    return QLocale().toString(parsed.toLocalTime(), QLocale::ShortFormat);
}

QString format_relative_age(const std::string &published_at) {
    const QDateTime parsed = parse_published_at(published_at);
    if (!parsed.isValid())
        return {};

    const qint64 seconds = parsed.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 60)
        return tr_update("moments ago");
    if (seconds < 3600)
        return tr_update("%1 minute(s) ago").arg(seconds / 60);
    if (seconds < 86400)
        return tr_update("%1 hour(s) ago").arg(seconds / 3600);
    if (seconds < 86400 * 30)
        return tr_update("%1 day(s) ago").arg(seconds / 86400);

    return tr_update("%1 month(s) ago").arg(std::max<qint64>(1, seconds / (86400 * 30)));
}

// ASMM fork: updates are the published releases of Project-RePSASBR/Vita3K-ASMM (drafts and pre-releases are
// ignored). Each release carries one asset per platform named Vita3K-ASMM-<commit hash>-<variant>; the hash in
// this platform's asset tells which release a build is.
QString asmm_asset_suffix() {
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    return QStringLiteral("-windows-arm64.zip");
#elif defined(_WIN32)
    return QStringLiteral("-windows-x64.zip");
#elif defined(__APPLE__) && defined(__aarch64__)
    return QStringLiteral("-macos-arm64.dmg");
#elif defined(__APPLE__)
    return QStringLiteral("-macos-x64.dmg");
#elif defined(__linux__) && defined(__aarch64__)
    return QStringLiteral("-linux-arm64.AppImage");
#elif defined(__linux__)
    return QStringLiteral("-linux-x64.AppImage");
#else
    return {};
#endif
}

struct AsmmRelease {
    updater::UpdateInfo info;
    // Release tag, for the changelog
    QString tag;
    // Commit hash from this platform's asset name
    QString hash;
};

// Short hashes of different lengths name the same commit when one starts with the other
bool same_commit(const QString &a, const QString &b) {
    const qsizetype length = std::min(a.size(), b.size());
    return length >= 7 && a.left(length).compare(b.left(length), Qt::CaseInsensitive) == 0;
}

QString normalized_notes(const QString &body) {
    QString notes = body;
    notes.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList cleaned_lines;
    const QStringList lines = notes.split('\n');
    static const QRegularExpression metadata_line(QStringLiteral("^\\s*(Corresponding commit:|Vita3K Build:)"), QRegularExpression::CaseInsensitiveOption);

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (metadata_line.match(trimmed).hasMatch())
            continue;

        cleaned_lines.append(line);
    }

    while (!cleaned_lines.isEmpty() && cleaned_lines.first().trimmed().isEmpty())
        cleaned_lines.removeFirst();
    while (!cleaned_lines.isEmpty() && cleaned_lines.last().trimmed().isEmpty())
        cleaned_lines.removeLast();

    return cleaned_lines.join('\n').trimmed();
}

QString to_html_with_breaks(const QString &text) {
    QString escaped = text.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return escaped;
}

// The published releases that ship a build for this platform, newest first
bool parse_release_list(const std::string &json, std::vector<AsmmRelease> &releases, QString &error_message) {
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isArray()) {
        error_message = tr_update("The GitHub release response could not be parsed.");
        return false;
    }

    const QString suffix = asmm_asset_suffix();
    if (suffix.isEmpty()) {
        error_message = tr_update("Vita3K ASMM releases have no build for this platform.");
        return false;
    }

    static const QRegularExpression asset_regex(QStringLiteral("^Vita3K-ASMM-([0-9a-fA-F]{7,40})-"));

    for (const QJsonValue &release_value : document.array()) {
        const QJsonObject root = release_value.toObject();
        if (root.value(QStringLiteral("draft")).toBool() || root.value(QStringLiteral("prerelease")).toBool())
            continue;

        for (const QJsonValue &asset_value : root.value(QStringLiteral("assets")).toArray()) {
            const QString asset_name = asset_value.toObject().value(QStringLiteral("name")).toString();
            const auto match = asset_regex.match(asset_name);
            if (!asset_name.endsWith(suffix) || !match.hasMatch())
                continue;

            const QString release_name = root.value(QStringLiteral("name")).toString().trimmed();
            const QString tag_name = root.value(QStringLiteral("tag_name")).toString().trimmed();
            const QString release_url = root.value(QStringLiteral("html_url")).toString().trimmed();

            AsmmRelease release;
            release.info.version = (release_name.isEmpty() ? tag_name : release_name).toStdString();
            release.info.release_url = (release_url.isEmpty() ? QString::fromStdString(updater::release_page_url()) : release_url).toStdString();
            release.info.published_at = root.value(QStringLiteral("published_at")).toString().toStdString();
            release.info.notes = normalized_notes(root.value(QStringLiteral("body")).toString()).toStdString();
            release.tag = tag_name;
            release.hash = match.captured(1);
            releases.push_back(std::move(release));
            break;
        }
    }

    if (releases.empty()) {
        error_message = tr_update("No published Vita3K ASMM release has a build for this platform yet.");
        return false;
    }

    // ISO 8601 UTC timestamps sort as text
    std::stable_sort(releases.begin(), releases.end(), [](const AsmmRelease &a, const AsmmRelease &b) {
        return a.info.published_at > b.info.published_at;
    });
    return true;
}

QString build_details_text(const updater::UpdateInfo &info) {
    QStringList sections;

    if (!info.changelog.empty()) {
        QStringList changelog_lines;
        for (const auto &entry : info.changelog) {
            const QString version = entry.version.empty() ? tr_update("N/A") : QString::fromStdString(entry.version);
            const QString title = entry.title.empty() ? tr_update("N/A") : QString::fromStdString(entry.title);
            changelog_lines.append(tr_update("- %1: %2").arg(version, title));
        }
        sections.append(tr_update("Recent changes:\n%1").arg(changelog_lines.join('\n')));
    }

    if (!info.notes.empty())
        sections.append(QString::fromStdString(info.notes));

    return sections.join(QStringLiteral("\n\n"));
}

updater::UpdateCheckResult build_check_result() {
    updater::UpdateCheckResult result;
    result.current_display_version = updater::current_display_version();

    const std::string response = net_utils::get_web_response(updater::release_api_url());
    if (response.empty()) {
        result.status = updater::UpdateCheckStatus::Failed;
        result.message = tr_update("Could not retrieve the Vita3K ASMM release information. Please try again later.").toStdString();
        return result;
    }

    std::vector<AsmmRelease> releases;
    QString parse_message;
    if (!parse_release_list(response, releases, parse_message)) {
        result.status = updater::UpdateCheckStatus::Failed;
        result.message = parse_message.toStdString();
        return result;
    }

    const QString current_hash = QString::fromUtf8(app_hash);
    const auto current = std::find_if(releases.cbegin(), releases.cend(), [&current_hash](const AsmmRelease &release) {
        return same_commit(release.hash, current_hash);
    });
    result.info = releases.front().info;

    // Not a published release: a development or test build
    if (current == releases.cend()) {
        result.status = updater::UpdateCheckStatus::CustomBuildCanUpdate;
        return result;
    }

    result.current_display_version = QStringLiteral("%1 (%2)").arg(QString::fromStdString(current->info.version), current_hash).toStdString();

    if (current == releases.cbegin()) {
        result.status = updater::UpdateCheckStatus::UpToDate;
        result.message = tr_update("You already have the latest Vita3K ASMM release (%1).").arg(QString::fromStdString(current->info.version)).toStdString();
        return result;
    }

    // The changelog lists every release newer than the running one
    for (auto it = releases.cbegin(); it != current; ++it)
        result.info.changelog.push_back({ it->tag.toStdString(), it->info.version });

    result.status = updater::UpdateCheckStatus::UpdateAvailable;
    return result;
}

} // namespace

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent) {
}

UpdateManager::~UpdateManager() {
    if (m_worker_thread) {
        m_worker_thread->quit();
        m_worker_thread->wait();
    }
}

void UpdateManager::set_update_available(const bool available) {
    if (m_update_available == available)
        return;

    m_update_available = available;
    Q_EMIT update_available_changed(available);
}

void UpdateManager::set_pending_update(std::optional<updater::UpdateCheckResult> result) {
    m_pending_update = std::move(result);
    set_update_available(m_pending_update.has_value());
}

void UpdateManager::start_worker(const std::function<void()> &task) {
    if (m_worker_thread)
        return;

    m_worker_thread = QThread::create(task);
    connect(m_worker_thread, &QThread::finished, this, [this]() {
        m_worker_thread->deleteLater();
        m_worker_thread = nullptr;
    });
    m_worker_thread->start();
}

void UpdateManager::close_progress_dialog() {
    if (!m_progress_dialog)
        return;

    m_progress_dialog->close();
    m_progress_dialog->deleteLater();
    m_progress_dialog = nullptr;
}

void UpdateManager::check_for_updates(const updater::UpdateCheckMode mode, QWidget *parent) {
    if (m_worker_thread) {
        if (mode == updater::UpdateCheckMode::ManualInteractive)
            QMessageBox::information(parent, tr_update("Check for Updates"), tr_update("An update check is already running."));
        return;
    }

    m_parent_widget = parent;

    if (mode == updater::UpdateCheckMode::ManualInteractive) {
        auto *dialog = new QProgressDialog(tr_update("Checking for updates..."), QString(), 0, 0, parent);
        dialog->setWindowTitle(tr_update("Check for Updates"));
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setMinimumDuration(0);
        dialog->setCancelButton(nullptr);
        dialog->show();
        m_progress_dialog = dialog;
    }

    start_worker([this, mode]() {
        const updater::UpdateCheckResult result = build_check_result();
        QMetaObject::invokeMethod(
            this, [this, mode, result]() { handle_check_result(mode, result); }, Qt::QueuedConnection);
    });
}

void UpdateManager::review_pending_update(QWidget *parent) {
    m_parent_widget = parent;

    if (m_pending_update) {
        show_update_message(*m_pending_update);
        return;
    }

    check_for_updates(updater::UpdateCheckMode::ManualInteractive, parent);
}

void UpdateManager::handle_check_result(const updater::UpdateCheckMode mode, const updater::UpdateCheckResult &result) {
    close_progress_dialog();

    switch (result.status) {
    case updater::UpdateCheckStatus::Failed:
        if (mode == updater::UpdateCheckMode::ManualInteractive)
            QMessageBox::warning(m_parent_widget, tr_update("Check for Updates"), QString::fromStdString(result.message));
        return;
    case updater::UpdateCheckStatus::UpToDate:
    case updater::UpdateCheckStatus::CurrentBuildNewerThanLatest:
        set_pending_update(std::nullopt);
        if (mode == updater::UpdateCheckMode::ManualInteractive)
            QMessageBox::information(m_parent_widget, tr_update("Check for Updates"), QString::fromStdString(result.message));
        return;
    case updater::UpdateCheckStatus::CustomBuildCanUpdate:
        set_pending_update(std::nullopt);
        if (mode == updater::UpdateCheckMode::ManualInteractive)
            show_update_message(result);
        return;
    case updater::UpdateCheckStatus::UpdateAvailable:
        set_pending_update(result);
        if (mode != updater::UpdateCheckMode::StartupBackground)
            show_update_message(result);
        return;
    }
}

void UpdateManager::show_update_message(const updater::UpdateCheckResult &result) {
    const QString published_at = format_published_at(result.info.published_at);
    const QString published_age = format_relative_age(result.info.published_at);
    const QString details_text = build_details_text(result.info);
    const QString latest_version = result.info.version.empty() ? tr_update("Latest build") : QString::fromStdString(result.info.version);
    const QString download_url = result.info.release_url.empty() ? QString::fromStdString(updater::release_page_url()) : QString::fromStdString(result.info.release_url);

    QString title;
    QString summary;
    switch (result.status) {
    case updater::UpdateCheckStatus::CustomBuildCanUpdate:
        title = tr_update("Vita3K ASMM Release Available");
        summary = tr_update(
            "You are currently running a build that is not a published Vita3K ASMM release, such as a development or test build.\n\n"
            "Latest Vita3K ASMM release: %1\n"
            "Published: %2%3")
                      .arg(latest_version)
                      .arg(published_at)
                      .arg(published_age.isEmpty() ? QString() : tr_update(" (%1)").arg(published_age));
        break;
    case updater::UpdateCheckStatus::UpdateAvailable:
    default:
        title = tr_update("Update Available");
        summary = tr_update(
            "A newer Vita3K ASMM release is available.\n\n"
            "Current version: %1\n"
            "Latest version: %2\n"
            "Published: %3%4")
                      .arg(QString::fromStdString(result.current_display_version))
                      .arg(latest_version)
                      .arg(published_at)
                      .arg(published_age.isEmpty() ? QString() : tr_update(" (%1)").arg(published_age));
        break;
    }

    const auto dialog_result = gui::utils::show_message_box(
        m_parent_widget,
        QMessageBox::Information,
        title,
        tr_update("%1<br><br>Download page: <a href=\"%2\">Vita3K ASMM releases</a>")
            .arg(to_html_with_breaks(summary), download_url.toHtmlEscaped()),
        {
            { QStringLiteral("open"), tr_update("Open Download Page"), QMessageBox::ActionRole, true },
            { QStringLiteral("ok"), tr_update("OK"), QMessageBox::AcceptRole, false },
        },
        {},
        {},
        false,
        Qt::RichText,
        Qt::TextBrowserInteraction,
        details_text);

    if (dialog_result.clicked_id == QStringLiteral("open"))
        QDesktopServices::openUrl(QUrl(download_url));
}
