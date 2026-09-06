#include "nix/fetchers/input-cache.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/nixignore.hh"
#include "nix/fetchers/registry.hh"
#include "nix/util/sync.hh"

namespace nix::fetchers {

namespace {

void warnIfLegacySourceSelection(
    SourceSelectionPolicy sourceSelectionPolicy, const Input & input, ref<SourceAccessor> accessor)
{
    if (sourceSelectionPolicy != SourceSelectionPolicy::NixIgnoreV1 || !input.isFinal()
        || input.attrs.contains("nixIgnore"))
        return;

    auto type = maybeGetStrAttr(input.attrs, "type");
    if (!type || (*type != "git" && *type != "path"))
        return;

    if (accessor->lstat(CanonPath::root).type != SourceAccessor::tDirectory)
        return;

    for (auto name : nixIgnoreFileNames) {
        auto stat = accessor->maybeLstat(CanonPath(name));
        if (!stat || stat->type != SourceAccessor::tRegular)
            continue;

        static bool warned = false;
        warnOnce(
            warned,
            "using legacy source selection for an older lock entry; update the lock file to enable ignore-file filtering");
        return;
    }
}

} // namespace

InputCache::CachedResult InputCache::getAccessor(
    const Settings & settings,
    Store & store,
    const Input & originalInput,
    UseRegistries useRegistries,
    SourceSelectionPolicy sourceSelectionPolicy)
{
    auto prepare = [&](Input input) {
        if (sourceSelectionPolicy == SourceSelectionPolicy::NixIgnoreV1) {
            discoverNixIgnore(input);
            if (!input.isDirect())
                input.attrs.insert_or_assign(std::string(nixIgnoreDiscoverAttr), Explicit<bool>(true));
        }
        return input;
    };

    auto cacheInput = prepare(originalInput);
    auto fetched = lookup(cacheInput);
    Input resolvedInput = originalInput;

    if (!fetched) {
        if (originalInput.isDirect()) {
            auto [accessor, lockedInput] = cacheInput.getAccessor(settings, store);
            fetched.emplace(CachedInput{.lockedInput = lockedInput, .accessor = accessor});
        } else {
            if (useRegistries != UseRegistries::No) {
                auto [res, extraAttrs] = lookupInRegistries(settings, store, originalInput, useRegistries);
                resolvedInput = std::move(res);
                auto resolvedCacheInput = prepare(resolvedInput);
                fetched = lookup(resolvedCacheInput);
                if (!fetched) {
                    auto [accessor, lockedInput] = resolvedCacheInput.getAccessor(settings, store);
                    fetched.emplace(
                        CachedInput{.lockedInput = lockedInput, .accessor = accessor, .extraAttrs = extraAttrs});
                }
                upsert(std::move(resolvedCacheInput), *fetched);
            } else {
                throw Error(
                    "'%s' is an indirect flake reference, but registry lookups are not allowed",
                    originalInput.to_string());
            }
        }
        upsert(std::move(cacheInput), *fetched);
    }

    warnIfLegacySourceSelection(sourceSelectionPolicy, originalInput, fetched->accessor);

    debug("got tree '%s' from '%s'", fetched->accessor, fetched->lockedInput.to_string());

    return {fetched->accessor, resolvedInput, fetched->lockedInput, fetched->extraAttrs};
}

struct InputCacheImpl : InputCache
{
    Sync<std::map<Input, CachedInput>> cache_;

    std::optional<CachedInput> lookup(const Input & originalInput) const override
    {
        auto cache(cache_.readLock());
        auto i = cache->find(originalInput);
        if (i == cache->end())
            return std::nullopt;
        debug(
            "mapping '%s' to previously seen input '%s' -> '%s",
            originalInput.to_string(),
            i->first.to_string(),
            i->second.lockedInput.to_string());
        return i->second;
    }

    void upsert(Input key, CachedInput cachedInput) override
    {
        cache_.lock()->insert_or_assign(std::move(key), std::move(cachedInput));
    }

    void clear() override
    {
        cache_.lock()->clear();
        /* The workdir info cache has the same "per evaluation" lifetime
           as the input cache, so flush it here as well so that e.g.
           `:reload` in `nix repl` picks up changes in git work trees. */
        GitRepo::invalidateWorkdirInfoCache();
    }
};

ref<InputCache> InputCache::create()
{
    return make_ref<InputCacheImpl>();
}

} // namespace nix::fetchers
