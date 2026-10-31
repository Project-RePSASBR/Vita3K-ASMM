#include <updater/functions.h>

#include <config/version.h>
#include <fmt/format.h>

namespace updater {

// ASMM fork: the published releases of Project-RePSASBR/Vita3K-ASMM, not upstream's continuous build
std::string release_api_url() {
    return "https://api.github.com/repos/Project-RePSASBR/Vita3K-ASMM/releases?per_page=30";
}

std::string release_page_url() {
    return "https://github.com/Project-RePSASBR/Vita3K-ASMM/releases";
}

std::string display_version(const UpdateInfo &info) {
    if (!info.version.empty())
        return fmt::format("{} ({})", info.version, info.build_number);

    return fmt::format("Build {}", info.build_number);
}

std::string current_display_version() {
    return fmt::format("{} ({})", app_version, app_number);
}

bool is_official_build() {
    return ::is_official_build;
}

} // namespace updater
