#include "nix/fetchers/fetchers.hh"
#include "nix/store/store-api.hh"
#include "nix/util/archive.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/nixignore.hh"

namespace nix::fetchers {

struct PathInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != "path")
            return {};

        if (url.authority && url.authority->host.size())
            throw Error("path URL '%s' should not have an authority ('%s')", url, *url.authority);

        Input input{};
        input.attrs.insert_or_assign("type", "path");
        input.attrs.insert_or_assign("path", urlPathToPath(url.path).string());

        for (auto & [name, value] : url.query)
            if (name == "rev" || name == "narHash")
                input.attrs.insert_or_assign(name, value);
            else if (name == "revCount" || name == "lastModified" || name == "nixIgnore") {
                if (auto n = string2Int<uint64_t>(value))
                    input.attrs.insert_or_assign(name, *n);
                else
                    throw Error("path URL '%s' has invalid parameter '%s'", url, name);
            } else
                throw Error("path URL '%s' has unsupported parameter '%s'", url, name);

        getNixIgnoreMode(input);
        return input;
    }

    std::string_view schemeName() const override
    {
        return "path";
    }

    std::string schemeDescription() const override
    {
        // TODO
        return "";
    }

    const std::map<std::string, AttributeInfo> & allowedAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {
                "path",
                {},
            },
            /* Allow the user to pass in "fake" tree info
               attributes. This is useful for making a pinned tree work
               the same as the repository from which is exported (e.g.
               path:/nix/store/...-source?lastModified=1585388205&rev=b0c285...).
             */
            {
                "rev",
                {},
            },
            {
                "revCount",
                {},
            },
            {
                "lastModified",
                {},
            },
            {
                "narHash",
                {},
            },
            {
                "nixIgnore",
                {
                    .type = "Integer",
                    .required = false,
                    .doc = "Version of root ignore-file source selection. Only version 1 is supported.",
                },
            },
        };
        return attrs;
    }

    std::optional<Input> inputFromAttrs(const Attrs & attrs) const override
    {
        getStrAttr(attrs, "path");

        Input input{};
        input.attrs = attrs;
        getNixIgnoreMode(input);
        return input;
    }

    ParsedURL toURL(const Input & input) const override
    {
        auto query = attrsToQuery(input.attrs);
        query.erase("path");
        query.erase("type");
        query.erase("__final");
        query.erase(std::string(nixIgnoreDiscoverAttr));
        return ParsedURL{
            .scheme = "path",
            .path = pathToUrlPath(std::filesystem::path{getStrAttr(input.attrs, "path")}),
            .query = query,
        };
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        return getAbsPath(input);
    }

    void putFile(
        const Input & input,
        const CanonPath & path,
        std::string_view contents,
        std::optional<std::string> commitMsg) const override
    {
        auto root = getAbsPath(input);
        auto dotGit = root / ".git";
        if (commitMsg && (pathExists(dotGit / "HEAD") || std::filesystem::is_regular_file(dotGit))) {
            auto gitInput = Input::fromAttrs({
                {"type", "git"},
                {"url", root.string()},
            });
            gitInput.putFile(path, contents, std::move(commitMsg));
            return;
        }

        writeFile(root / path.rel(), contents);
    }

    std::optional<std::filesystem::path> isRelative(const Input & input) const override
    {
        std::filesystem::path path = getStrAttr(input.attrs, "path");
        if (path.is_absolute())
            return std::nullopt;
        else
            return path;
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        return (bool) input.getNarHash();
    }

    std::filesystem::path getAbsPath(const Input & input) const
    {
        std::filesystem::path path = getStrAttr(input.attrs, "path");

        if (path.is_absolute())
            return canonPath(path);

        throw Error("cannot fetch input '%s' because it uses a relative path", input.to_string());
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessor(const Settings & settings, Store & store, const Input & _input) const override
    {
        Input input(_input);
        auto path = getStrAttr(input.attrs, "path");

        auto absPath = getAbsPath(input);

        // FIXME: check whether access to 'path' is allowed.
        auto storePath = store.maybeParseStorePath(absPath.string());

        if (storePath)
            store.addTempRoot(*storePath);

        time_t mtime = 0;
        auto mode = getNixIgnoreMode(input);
        auto source = makeFSSourceAccessor(absPath, /*trackLastModified=*/false);
        if (mode != NixIgnoreMode::Disabled && source->lstat(CanonPath::root).type == SourceAccessor::tDirectory) {
            auto filter = readNixIgnore(source);
            Activity act(*logger, lvlTalkative, actUnknown, fmt("copying %s to the store", PathFmt(absPath)));
            PathFilter pathFilter = [&](const std::string & path) {
                CanonPath canonPath(path);
                auto st = std::filesystem::symlink_status(absPath / canonPath.rel());
                auto type = std::filesystem::is_directory(st)      ? SourceAccessor::tDirectory
                            : std::filesystem::is_regular_file(st) ? SourceAccessor::tRegular
                            : std::filesystem::is_symlink(st)      ? SourceAccessor::tSymlink
                                                                   : SourceAccessor::tUnknown;
                return !filter.match(canonPath, type).ignored;
            };
            auto src = sinkToSource([&](Sink & sink) { mtime = dumpPathAndGetMtime(absPath, sink, pathFilter); });
            storePath = store.addToStoreFromDump(*src, "source");
            markNixIgnore(input);
        } else {
            if (mode == NixIgnoreMode::Required)
                throw Error("source declares nixIgnore version %d but is not a directory", nixIgnoreVersion);
            input.attrs.erase(std::string(nixIgnoreDiscoverAttr));
            if (!storePath || storePath->name() != "source" || !store.isValidPath(*storePath)) {
                Activity act(*logger, lvlTalkative, actUnknown, fmt("copying %s to the store", PathFmt(absPath)));
                // FIXME: try to substitute storePath.
                auto src = sinkToSource(
                    [&](Sink & sink) { mtime = dumpPathAndGetMtime(absPath.string(), sink, defaultPathFilter); });
                storePath = store.addToStoreFromDump(*src, "source");
            }
        }

        auto accessor = store.requireStoreObjectAccessor(*storePath);

        // To prevent `fetchToStore()` copying the path again to Nix
        // store, pre-create an entry in the fetcher cache.
        auto narHash = store.queryPathInfo(*storePath)->narHash.to_string(HashFormat::SRI, true);
        accessor->fingerprint = fmt("path:%s", narHash);
        settings.getCache()->upsert(
            makeSourcePathToHashCacheKey(
                *accessor->fingerprint, ContentAddressMethod::Raw::NixArchive, CanonPath::root),
            {{"hash", narHash}});

        /* Trust the lastModified value supplied by the user, if
           any. It's not a "secure" attribute so we don't care. */
        if (!input.getLastModified())
            input.attrs.insert_or_assign("lastModified", uint64_t(mtime));

        return {accessor, std::move(input)};
    }

    std::optional<ExperimentalFeature> experimentalFeature() const override
    {
        return Xp::Flakes;
    }
};

static auto rPathInputScheme = OnStartup([] { registerInputScheme(std::make_unique<PathInputScheme>()); });

} // namespace nix::fetchers
