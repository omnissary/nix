#pragma once

#include "nix/fetchers/filtering-source-accessor.hh"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nix::fetchers {

inline constexpr std::string_view nixIgnoreFileName = ".nixignore";

/**
 * The `.nixignore` matcher.
 *
 * Patterns are interpreted relative to the root of an input source.  The
 * syntax follows gitignore pattern syntax, but matching is always
 * case-sensitive and only a single, root-level file is consulted.
 */
class NixIgnoreRules
{
    struct Rule
    {
        bool negated;
        bool directoryOnly;
        bool anchored;
        size_t line;
        std::string text;
    };

    std::vector<Rule> rules;

public:
    struct Match
    {
        bool ignored;
        std::optional<size_t> line;
        std::string_view pattern;
    };

    static NixIgnoreRules parse(std::string_view contents);

    Match match(const CanonPath & path, SourceAccessor::Type type) const;
};

struct NixIgnoreFilter
{
    NixIgnoreRules rules;

    NixIgnoreRules::Match match(const CanonPath & path, SourceAccessor::Type type) const;
};

/** Compile the root `.nixignore`, if the source has one. */
std::optional<NixIgnoreFilter> readNixIgnore(ref<SourceAccessor> source);

/** Wrap `source` so ignored paths are inaccessible and omitted from directory listings. */
ref<SourceAccessor> makeNixIgnoreSourceAccessor(ref<SourceAccessor> source, NixIgnoreFilter filter);

} // namespace nix::fetchers
