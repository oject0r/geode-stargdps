#include "updater.hpp"
#include <asp/fs/fs.hpp>
#include <Geode/utils/web.hpp>
#include <resources.hpp>
#include <hash.hpp>
#include <utility>
#include "LoaderImpl.hpp"
#include "ModMetadataImpl.hpp"
#include <Geode/utils/string.hpp>
#include <Geode/utils/StringMap.hpp>

#include "../server/Server.hpp"

using namespace geode::prelude;

static StringMap<async::TaskHolder<web::WebResponse>> RUNNING_REQUESTS {};

bool s_isNewUpdateDownloaded = false;

void updater::downloadLatestLoaderResources() {
    log::debug("Downloading latest resources");

    async::spawn(
        server::getLatestLoaderVersion(),
        [](Result<server::ServerLoaderVersion, server::ServerError> res) {
            if (res.ok()) {
                auto& release = res.unwrap();

                updater::tryDownloadLoaderResources(release.resources.url, release.resources.hash, false);
            } else {
                ResourceDownloadEvent().send(
                    UpdateFailed("Unable to download resources: " + res.unwrapErr().details)
                );
            }
        }
    );
}

Result<> updater::extractLoaderResources(ByteSpan data, std::string_view expectedHash) {
    if (expectedHash.empty()) {
        log::debug("Skipping hash validation for resources");
    } else {
        auto actualHash = geode::sha256(data).toString();
        if (actualHash != expectedHash) {
            log::error("Hash mismatch in downloaded resources: expected {} but got {}", expectedHash, actualHash);
            return Err("Hash mismatch in downloaded resources");
        }
    }

    auto tempDir = dirs::getGeodeResourcesDir() / fmt::format("{}_tmp", Mod::get()->getID());
    auto resourcesDir = dirs::getGeodeResourcesDir() / Mod::get()->getID();

    GEODE_UNWRAP(asp::fs::removeAll(tempDir).mapErr([](auto ec) {
        return "Unable to remove old temporary directory: " + ec.message();
    }));

    GEODE_UNWRAP(asp::fs::createDir(tempDir).mapErr([](auto ec) {
        return "Unable to create temporary directory: " + ec.message();
    }));

    // unzip resources zip
    auto unzip = GEODE_UNWRAP(file::Unzip::create(data).mapErr([](auto const& e) {
        return "Unable to load new resources archive: " + e;
    }));

    GEODE_UNWRAP(unzip.extractAllTo(tempDir).mapErr([](auto const& e) {
        return "Unable to unzip new resources: " + e;
    }));

    GEODE_UNWRAP(asp::fs::removeAll(resourcesDir).mapErr([](auto ec) {
        return "Unable to remove old resources directory: " + ec.message();
    }));

    // this might fail due to fuse on certain devices? might have to do with sd card usage too
    // society if we could just access /data/media/0/ directly...
    if(!asp::fs::rename(tempDir, resourcesDir)) {
        // removing once again because it's possible the rename results in a partial failure allegedly?
        GEODE_UNWRAP(asp::fs::removeAll(resourcesDir).mapErr([](auto ec) {
            return "Unable to clear stale resources directory before copy: " + ec.message();
        }));

        GEODE_UNWRAP(asp::fs::copy(
            tempDir, resourcesDir,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing
        ).mapErr([](auto ec) {
            return "Unable to copy new resources directory: " + ec.message();
        }));

        GEODE_UNWRAP(asp::fs::removeAll(tempDir).mapErr([](auto ec) {
            return "Unable to remove final temporary directory: " + ec.message();
        }));
    }

    updater::updateSpecialFiles();
    return Ok();
}

void updater::tryDownloadLoaderResources(std::string url, std::string hash, bool tryLatestOnError) {
    if (RUNNING_REQUESTS.contains(url)) return;

    auto progress = [](const web::WebProgress& prog) {
        ResourceDownloadEvent().send(
            UpdateProgress(
                static_cast<uint8_t>(prog.downloadProgress().value_or(0)),
                "Downloading resources"
            )
        );
    };

    auto& holder = RUNNING_REQUESTS[url];
    holder.spawn(
        "Geode resources download",
        web::WebRequest{}.onProgress(std::move(progress)).get(url),
        [url, hash = std::move(hash)](auto response) {
            if (response.ok()) {
                auto data = std::move(response).data();
                if (GEODE_UNWRAP_IF_ERR(e, updater::extractLoaderResources(data, hash))) {
                    ResourceDownloadEvent().send(UpdateFailed(e));
                } else {
                    ResourceDownloadEvent().send(UpdateFinished());
                }
            }
            else {
                auto reason = response.string().unwrapOr("Unknown");
                // if the url was not found, try downloading latest release instead
                // (for development versions)
                if (response.code() == 404) {
                    log::warn("Unable to download resources: {}", reason);
                }
                ResourceDownloadEvent().send(
                    UpdateFailed("Unable to download resources: " + reason)
                );
            }
            RUNNING_REQUESTS.erase(url);
        }
    );
}

void updater::updateSpecialFiles() {
    auto resourcesDir = dirs::getGeodeResourcesDir() / Mod::get()->getID();
    auto res = ModMetadataImpl::getImpl(ModImpl::get()->m_metadata).addSpecialFiles(resourcesDir);
    if (res.isErr()) {
        log::warn("Unable to add special files: {}", res.unwrapErr());
    }
}

void updater::downloadLoaderResources(bool useLatestRelease) {
    static bool DOWNLOADING_LOADER_RESOURCES = false;

    if (DOWNLOADING_LOADER_RESOURCES) return;
    DOWNLOADING_LOADER_RESOURCES = true;

    async::spawn(
        server::getLoaderVersion(Loader::get()->getVersion().toNonVString()),
        [useLatestRelease](Result<server::ServerLoaderVersion, server::ServerError> res) {
            if (res.ok()) {
                auto& release = res.unwrap();

                updater::tryDownloadLoaderResources(release.resources.url, release.resources.hash, false);

                DOWNLOADING_LOADER_RESOURCES = false;
                return;
            }
            if (useLatestRelease) {
                log::info("Loader version {} does not exist, trying to download latest resources", Loader::get()->getVersion().toVString());
                downloadLatestLoaderResources();
            }
            else {
                log::warn("Loader version {} does not exist on GitHub, not downloading the resources", Loader::get()->getVersion().toVString());
                ResourceDownloadEvent().send(UpdateFinished());
            }

            DOWNLOADING_LOADER_RESOURCES = false;
        }
    );

}

bool updater::verifyLoaderResources() {
    static std::optional<bool> CACHED = std::nullopt;
    if (CACHED.has_value()) {
        return CACHED.value();
    }

    // geode/resources/geode.loader
    auto resourcesDir = dirs::getGeodeResourcesDir() / Mod::get()->getID();

    // if the resources dir doesn't exist, then it's probably incorrect
    if (!(
        std::filesystem::exists(resourcesDir) &&
            std::filesystem::is_directory(resourcesDir)
    )) {
        log::debug("Resources directory does not exist");
        updater::downloadLoaderResources(true);
        return false;
    }

    // TODO: actually have a proper way to disable checking resources
    // for development builds
    if (std::filesystem::exists(resourcesDir / "dont-update.txt")) {
        // this is kind of a hack, but it's the easiest way to prevent
        // auto update while developing
        log::debug("Not updating resources since dont-update.txt exists");
        return true;
    }

    // make sure every file was covered
    size_t coverage = 0;

    // verify hashes
    for (auto& file : std::filesystem::directory_iterator(resourcesDir)) {
        auto name = utils::string::pathToString(file.path().filename());
        // skip unknown files
        if (!LOADER_RESOURCE_HASHES.count(name)) {
            continue;
        }
        // verify hash
        // if we hash anything other than text, change this
        auto hash = sha256Text(file.path()).toString();
        const auto& expected = LOADER_RESOURCE_HASHES.at(name);
        if (hash != expected) {
            log::debug("Resource hash mismatch: {} ({}, {})", name, hash.substr(0, 7), expected.substr(0, 7));
            updater::downloadLoaderResources();
            return false;
        }
        coverage += 1;
    }

    // make sure every file was found
    if (coverage != LOADER_RESOURCE_HASHES.size()) {
        log::debug("Resource coverage mismatch");
        updater::downloadLoaderResources();
        return false;
    }

    return true;
}

void updater::downloadLoaderUpdate(std::string url, std::string hash) {
    // Auto updating is disabled in this fork
}

Result<> updater::installLoaderUpdate(utils::web::WebResponse response, std::string_view expectedHash) {
    // Auto updating is disabled in this fork
    return Ok();
}

void updater::checkForLoaderUpdates() {
    // Auto updating is disabled in this fork
}

bool updater::isNewUpdateDownloaded() {
    return false;
}
