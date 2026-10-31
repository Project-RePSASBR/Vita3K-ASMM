package org.vita3k.emulator.data

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.ui.viewmodel.AppAction
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

internal object AppRepository {
    private const val TAG = "Vita3K"
    private const val COMPAT_VERSION_URL =
        "https://api.github.com/repos/Vita3K/compatibility/releases/latest"
    private const val COMPAT_DB_URL =
        "https://github.com/Vita3K/compatibility/releases/download/compat_db/app_compat_db.xml.zip"
    // ASMM fork: updates are the published releases of Project-RePSASBR/Vita3K-ASMM (drafts and pre-releases are
    // ignored). Each release carries Vita3K-ASMM-<commit hash>-android.apk; that hash tells which release a build is.
    private const val UPDATE_RELEASE_URL =
        "https://api.github.com/repos/Project-RePSASBR/Vita3K-ASMM/releases?per_page=30"
    const val UPDATE_PAGE_URL =
        "https://github.com/Project-RePSASBR/Vita3K-ASMM/releases"
    private val compatVersionRegex =
        Regex("""Last updated: (\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}Z)""")
    private val updateAssetRegex = Regex("""^Vita3K-ASMM-([0-9a-fA-F]{7,40})-android\.apk$""")
    private val updateMetadataLineRegex =
        Regex("""^\s*(Corresponding commit:|Vita3K Build:)""", RegexOption.IGNORE_CASE)

    suspend fun initialize(storagePath: String): Boolean = withContext(Dispatchers.IO) {
        NativeLib.init(storagePath)
    }

    suspend fun getAppVersion(): String = withContext(Dispatchers.IO) {
        NativeLib.getAppVersion()
    }

    suspend fun getFirmwareInstallState(): FirmwareInstallState = withContext(Dispatchers.IO) {
        FirmwareInstallState.fromMask(NativeLib.getFirmwareInstallStateMask())
    }

    suspend fun syncCompatibilityDatabase(): Boolean = withContext(Dispatchers.IO) {
        try {
            val currentVersion = NativeLib.getCompatibilityDatabaseVersion()
            val versionResponse = httpGetString(COMPAT_VERSION_URL) ?: return@withContext false
            val latestVersion = compatVersionRegex.find(versionResponse)?.groupValues?.getOrNull(1)
                ?: return@withContext false

            if (latestVersion == currentVersion) {
                return@withContext false
            }

            val zipData = httpGetBytes(COMPAT_DB_URL) ?: return@withContext false
            NativeLib.installCompatibilityDatabase(zipData, latestVersion)
        } catch (error: Exception) {
            Log.w(TAG, "Failed to sync compatibility database", error)
            false
        }
    }

    suspend fun getAppList(): List<AppInfo> = withContext(Dispatchers.IO) {
        NativeLib.getAppListDetailed().map(::toAppInfo)
    }

    suspend fun refreshAppsList() = withContext(Dispatchers.IO) {
        NativeLib.refreshAppsList()
    }

    suspend fun runAppAction(titleId: String, action: AppAction): Boolean =
        withContext(Dispatchers.IO) {
            NativeLib.performAppAction(titleId, action.maskBit)
        }

    suspend fun getAvailableAppActions(titleId: String): Set<AppAction> =
        withContext(Dispatchers.IO) {
            AppAction.fromMask(NativeLib.getAppActionAvailabilityMask(titleId))
        }

    suspend fun getAppInstallSize(titleId: String): Long = withContext(Dispatchers.IO) {
        NativeLib.getAppInstallSize(titleId)
    }

    suspend fun checkForUpdates(appVersion: String): UpdateCheckResult = withContext(Dispatchers.IO) {
        val currentDisplayVersion = currentDisplayVersion(appVersion)
        val response = httpGetString(UPDATE_RELEASE_URL)
            ?: return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.Failed,
                message = "Could not retrieve the Vita3K ASMM release information. Please try again later.",
                currentDisplayVersion = currentDisplayVersion
            )

        val releases = try {
            parseReleases(JSONArray(response))
        } catch (_: Exception) {
            return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.Failed,
                message = "The GitHub release response could not be parsed.",
                currentDisplayVersion = currentDisplayVersion
            )
        }

        val latest = releases.firstOrNull()
            ?: return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.Failed,
                message = "No published Vita3K ASMM release has an Android build yet.",
                currentDisplayVersion = currentDisplayVersion
            )

        // app version = <version>-<build number>-<commit hash>
        val currentHash = appVersion.split("-", limit = 3).getOrNull(2).orEmpty()
        val currentIndex = releases.indexOfFirst { sameCommit(it.hash, currentHash) }

        when {
            // Not a published release: a development or test build
            currentIndex < 0 -> UpdateCheckResult(
                status = UpdateCheckStatus.CustomBuildCanUpdate,
                message = "",
                info = latest.info,
                currentDisplayVersion = currentDisplayVersion
            )
            currentIndex == 0 -> UpdateCheckResult(
                status = UpdateCheckStatus.UpToDate,
                message = "You already have the latest Vita3K ASMM release (${latest.info.version}).",
                info = latest.info,
                currentDisplayVersion = "${latest.info.version} ($currentHash)"
            )
            else -> {
                // The notes start with every release newer than the running one
                val newer = releases.subList(0, currentIndex).joinToString("\n") { "- ${it.info.version}" }
                UpdateCheckResult(
                    status = UpdateCheckStatus.UpdateAvailable,
                    message = "",
                    info = latest.info.copy(notes = "Newer releases:\n$newer\n\n${latest.info.notes}".trim()),
                    currentDisplayVersion = "${releases[currentIndex].info.version} ($currentHash)"
                )
            }
        }
    }

    private class AsmmRelease(val info: UpdateInfo, val hash: String)

    // The published releases that ship an Android build, newest first
    private fun parseReleases(array: JSONArray): List<AsmmRelease> {
        val releases = mutableListOf<AsmmRelease>()
        for (index in 0 until array.length()) {
            val release = array.getJSONObject(index)
            if (release.optBoolean("draft") || release.optBoolean("prerelease")) {
                continue
            }

            val assets = release.optJSONArray("assets") ?: continue
            val hash = (0 until assets.length())
                .mapNotNull { assets.optJSONObject(it)?.optString("name") }
                .firstNotNullOfOrNull { updateAssetRegex.matchEntire(it)?.groupValues?.get(1) }
                ?: continue

            val releaseName = release.optString("name").trim()
            val tagName = release.optString("tag_name").trim()
            releases += AsmmRelease(
                info = UpdateInfo(
                    version = releaseName.ifBlank { tagName },
                    releaseUrl = release.optString("html_url").trim().ifBlank { UPDATE_PAGE_URL },
                    publishedAt = release.optString("published_at").trim(),
                    notes = normalizedUpdateNotes(release.optString("body").orEmpty())
                ),
                hash = hash
            )
        }

        // ISO 8601 UTC timestamps sort as text
        return releases.sortedByDescending { it.info.publishedAt }
    }

    // Short hashes of different lengths name the same commit when one starts with the other
    private fun sameCommit(a: String, b: String): Boolean {
        val length = minOf(a.length, b.length)
        return length >= 7 && a.regionMatches(0, b, 0, length, ignoreCase = true)
    }

    private fun toAppInfo(native: NativeAppInfo): AppInfo = AppInfo(
        titleId = native.titleId,
        title = native.title,
        category = native.category,
        appVer = native.appVer,
        iconPath = native.iconPath.ifEmpty { null },
        hasCustomConfig = native.hasCustomConfig,
        compatibility = CompatibilityState.fromValue(native.compatibility),
        lastPlayed = native.lastPlayed,
        playtime = native.playtime
    )

    private fun currentDisplayVersion(appVersion: String): String {
        if (appVersion.isBlank()) {
            return ""
        }

        val parts = appVersion.split("-", limit = 3)
        val version = parts.getOrNull(0).orEmpty().ifBlank { appVersion }
        val build = parts.getOrNull(1).orEmpty()
        return if (build.isBlank()) version else "$version ($build)"
    }

    private fun normalizedUpdateNotes(body: String): String {
        val cleaned = body
            .replace("\r\n", "\n")
            .lineSequence()
            .filterNot { line -> updateMetadataLineRegex.containsMatchIn(line.trim()) }
            .dropWhile { it.isBlank() }
            .toList()
            .dropLastWhile { it.isBlank() }

        return cleaned.joinToString("\n").trim()
    }

    private fun httpGetString(url: String): String? =
        httpGetBytes(url)?.toString(Charsets.UTF_8)

    private fun httpGetBytes(url: String): ByteArray? {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            instanceFollowRedirects = true
            connectTimeout = 10000
            readTimeout = 15000
            setRequestProperty("User-Agent", "Vita3K-Android")
            setRequestProperty("Accept", "application/vnd.github+json")
        }

        return try {
            connection.connect()
            if (connection.responseCode !in 200..299) {
                null
            } else {
                connection.inputStream.use { it.readBytes() }
            }
        } finally {
            connection.disconnect()
        }
    }
}
