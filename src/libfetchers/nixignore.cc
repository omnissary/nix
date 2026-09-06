#include "nix/fetchers/nixignore.hh"

#include "nix/fetchers/fetchers.hh"
#include "nix/util/configuration.hh"
#include "nix/util/strings.hh"

#include <unordered_map>

namespace nix::fetchers {

NixIgnoreMode getNixIgnoreMode(const Input & input)
{
    if (auto version = maybeGetIntAttr(input.attrs, "nixIgnore")) {
        if (*version != nixIgnoreVersion)
            throw Error("unsupported nixIgnore version %d (only version %d is supported)", *version, nixIgnoreVersion);
        return NixIgnoreMode::Required;
    }

    return input.attrs.contains(std::string(nixIgnoreDiscoverAttr)) ? NixIgnoreMode::Discover : NixIgnoreMode::Disabled;
}

void discoverNixIgnore(Input & input)
{
    auto type = maybeGetStrAttr(input.attrs, "type");
    if (type && (*type == "git" || *type == "path") && !input.isFinal() && !input.attrs.contains("nixIgnore"))
        input.attrs.insert_or_assign(std::string(nixIgnoreDiscoverAttr), Explicit<bool>(true));
}

void markNixIgnore(Input & input)
{
    input.attrs.erase(std::string(nixIgnoreDiscoverAttr));
    input.attrs.insert_or_assign("nixIgnore", nixIgnoreVersion);
}

namespace {

using MatchChar = unsigned char;

bool isAscii(MatchChar c)
{
    return c <= 0x7f;
}

std::optional<bool> matchesCharacterClass(std::string_view name, MatchChar c)
{
    if (!isAscii(c)) {
        if (name == "alnum" || name == "alpha" || name == "blank" || name == "cntrl" || name == "digit"
            || name == "graph" || name == "lower" || name == "print" || name == "punct" || name == "space"
            || name == "upper" || name == "xdigit")
            return false;
        return std::nullopt;
    }

    if (name == "alnum")
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (name == "alpha")
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (name == "blank")
        return c == ' ' || c == '\t';
    if (name == "cntrl")
        return c <= 0x1f || c == 0x7f;
    if (name == "digit")
        return c >= '0' && c <= '9';
    if (name == "graph")
        return c >= 0x21 && c <= 0x7e;
    if (name == "lower")
        return c >= 'a' && c <= 'z';
    if (name == "print")
        return c >= 0x20 && c <= 0x7e;
    if (name == "punct")
        return c >= 0x21 && c <= 0x7e && !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
    if (name == "space")
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    if (name == "upper")
        return c >= 'A' && c <= 'Z';
    if (name == "xdigit")
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    return std::nullopt;
}

struct CharacterClassResult
{
    size_t nextPattern;
    bool matched;
};

struct CharacterClassAtom
{
    size_t nextPattern;
    std::optional<MatchChar> literal;
    bool matched;
};

std::optional<CharacterClassAtom>
parseCharacterClassAtom(std::string_view pattern, size_t patternPos, MatchChar textChar)
{
    if (patternPos >= pattern.size())
        return std::nullopt;

    if (pattern[patternPos] == '\\') {
        if (++patternPos >= pattern.size())
            return std::nullopt;
        auto literal = static_cast<MatchChar>(pattern[patternPos]);
        return CharacterClassAtom{
            .nextPattern = patternPos + 1,
            .literal = literal,
            .matched = textChar == literal,
        };
    }

    if (pattern[patternPos] == '[' && patternPos + 1 < pattern.size() && pattern[patternPos + 1] == ':') {
        auto end = pattern.find(":]", patternPos + 2);
        if (end == std::string_view::npos)
            return std::nullopt;
        auto matched = matchesCharacterClass(pattern.substr(patternPos + 2, end - patternPos - 2), textChar);
        if (!matched)
            return std::nullopt;
        return CharacterClassAtom{
            .nextPattern = end + 2,
            .literal = std::nullopt,
            .matched = *matched,
        };
    }

    auto literal = static_cast<MatchChar>(pattern[patternPos]);
    return CharacterClassAtom{
        .nextPattern = patternPos + 1,
        .literal = literal,
        .matched = textChar == literal,
    };
}

std::optional<CharacterClassResult> matchCharacterClass(std::string_view pattern, size_t patternPos, MatchChar textChar)
{
    auto current = patternPos + 1;
    bool negated = false;
    if (current < pattern.size() && (pattern[current] == '!' || pattern[current] == '^')) {
        negated = true;
        ++current;
    }

    bool first = true;
    bool matched = false;
    while (current < pattern.size()) {
        if (pattern[current] == ']' && !first)
            return CharacterClassResult{
                .nextPattern = current + 1,
                .matched = matched != negated && textChar != '/',
            };

        auto start = parseCharacterClassAtom(pattern, current, textChar);
        if (!start)
            return std::nullopt;
        first = false;

        if (start->literal && start->nextPattern + 1 < pattern.size() && pattern[start->nextPattern] == '-'
            && pattern[start->nextPattern + 1] != ']') {
            auto end = parseCharacterClassAtom(pattern, start->nextPattern + 1, textChar);
            if (!end || !end->literal)
                return std::nullopt;
            matched = matched || (textChar >= *start->literal && textChar <= *end->literal);
            current = end->nextPattern;
        } else {
            matched = matched || start->matched;
            current = start->nextPattern;
        }
    }

    return std::nullopt;
}

bool wildMatch(std::string_view pattern, std::string_view text)
{
    std::unordered_map<size_t, bool> memo;
    auto matchFrom = [&](auto & self, size_t patternPos, size_t textPos) -> bool {
        auto key = patternPos * (text.size() + 1) + textPos;
        if (auto i = memo.find(key); i != memo.end())
            return i->second;

        auto finish = [&](bool result) {
            memo.emplace(key, result);
            return result;
        };

        if (patternPos == pattern.size())
            return finish(textPos == text.size());

        auto patternChar = pattern[patternPos];
        if (patternChar == '*') {
            auto nextPattern = patternPos;
            while (nextPattern < pattern.size() && pattern[nextPattern] == '*')
                ++nextPattern;

            bool doubleStar = nextPattern - patternPos >= 2 && (patternPos == 0 || pattern[patternPos - 1] == '/')
                              && (nextPattern == pattern.size() || pattern[nextPattern] == '/'
                                  || (pattern[nextPattern] == '\\' && nextPattern + 1 < pattern.size()
                                      && pattern[nextPattern + 1] == '/'));

            if (doubleStar && nextPattern < pattern.size()) {
                auto afterSlash = nextPattern + (pattern[nextPattern] == '/' ? 1 : 2);
                if (self(self, afterSlash, textPos))
                    return finish(true);
            }

            for (auto nextText = textPos;; ++nextText) {
                if (self(self, nextPattern, nextText))
                    return finish(true);
                if (nextText == text.size() || (!doubleStar && text[nextText] == '/'))
                    break;
            }
            return finish(false);
        }

        if (textPos == text.size())
            return finish(false);

        if (patternChar == '?')
            return finish(text[textPos] != '/' && self(self, patternPos + 1, textPos + 1));

        if (patternChar == '[') {
            auto characterClass = matchCharacterClass(pattern, patternPos, static_cast<MatchChar>(text[textPos]));
            return finish(
                characterClass && characterClass->matched && self(self, characterClass->nextPattern, textPos + 1));
        }

        if (patternChar == '\\') {
            if (++patternPos == pattern.size())
                return finish(false);
            patternChar = pattern[patternPos];
        }

        return finish(patternChar == text[textPos] && self(self, patternPos + 1, textPos + 1));
    };

    return matchFrom(matchFrom, 0, 0);
}

bool isEscaped(std::string_view s, size_t pos)
{
    size_t backslashes = 0;
    while (pos > 0 && s[--pos] == '\\')
        ++backslashes;
    return backslashes % 2 != 0;
}

std::string_view baseName(std::string_view path)
{
    auto pos = path.rfind('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

} // namespace

NixIgnoreRules NixIgnoreRules::parse(std::string_view contents)
{
    NixIgnoreRules result;
    size_t lineNumber = 0;

    for (auto line : splitString<std::vector<std::string>>(contents, "\n")) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        while (!line.empty() && line.back() == ' ' && !isEscaped(line, line.size() - 1))
            line.pop_back();

        if (line.empty() || line.front() == '#')
            continue;

        bool negated = false;
        if (line.front() == '!') {
            negated = true;
            line.erase(0, 1);
        }

        bool directoryOnly = !line.empty() && line.back() == '/' && !isEscaped(line, line.size() - 1);
        if (directoryOnly)
            line.pop_back();

        bool anchored = !line.empty() && line.front() == '/' && !isEscaped(line, 0);
        if (anchored)
            line.erase(0, 1);

        if (line.empty())
            continue;

        for (size_t i = 0; i < line.size(); ++i)
            if (line[i] == '/' && !isEscaped(line, i)) {
                anchored = true;
                break;
            }

        result.rules.push_back(
            Rule{
                .negated = negated,
                .directoryOnly = directoryOnly,
                .anchored = anchored,
                .line = lineNumber,
                .text = std::move(line),
            });
    }

    return result;
}

NixIgnoreRules::Match NixIgnoreRules::match(const CanonPath & path, SourceAccessor::Type type) const
{
    if (path.isRoot())
        return {.ignored = false};

    auto fullPath = std::string(path.rel());
    size_t end = 0;

    while (end < fullPath.size()) {
        auto slash = fullPath.find('/', end);
        auto prefixEnd = slash == std::string::npos ? fullPath.size() : slash;
        auto prefix = std::string_view(fullPath).substr(0, prefixEnd);
        bool isDirectory = slash != std::string::npos || type == SourceAccessor::tDirectory;

        std::optional<bool> ignored;
        std::optional<size_t> matchingLine;
        std::string_view matchingPattern;

        for (auto & rule : rules) {
            if (rule.directoryOnly && !isDirectory)
                continue;
            auto candidate = rule.anchored ? prefix : baseName(prefix);
            if (wildMatch(rule.text, candidate)) {
                ignored = !rule.negated;
                matchingLine = rule.line;
                matchingPattern = rule.text;
            }
        }

        if (ignored.value_or(false))
            return {.ignored = true, .line = matchingLine, .pattern = matchingPattern};

        if (slash == std::string::npos)
            break;
        end = slash + 1;
    }

    return {.ignored = false};
}

namespace {

bool isVcsMetadata(const CanonPath & path)
{
    for (auto component : path)
        if (component == ".git" || component == ".hg" || component == ".jj")
            return true;
    return false;
}

} // namespace

NixIgnoreRules::Match NixIgnoreFilter::match(const CanonPath & path, SourceAccessor::Type type) const
{
    if (isVcsMetadata(path))
        return {.ignored = true};
    return rules.match(path, type);
}

NixIgnoreFilter readNixIgnore(ref<SourceAccessor> source)
{
    NixIgnoreFilter result;

    for (auto name : nixIgnoreFileNames) {
        CanonPath path(name);
        auto stat = source->maybeLstat(path);
        if (!stat)
            continue;
        if (stat->type != SourceAccessor::tRegular)
            throw Error("'%s' must be a regular file", name);

        result.sourceFile = path;
        result.rules = NixIgnoreRules::parse(source->readFile(path));
        if (auto match = result.match(path, SourceAccessor::tRegular); match.ignored)
            throw Error("'%s' excludes itself at line %d", name, match.line.value_or(0));
        break;
    }

    return result;
}

namespace {

struct NixIgnoreSourceAccessor final : CachingFilteringSourceAccessor
{
private:
    void anchor() override {}

public:
    std::shared_ptr<const NixIgnoreFilter> filter;

    NixIgnoreSourceAccessor(ref<SourceAccessor> next, std::shared_ptr<const NixIgnoreFilter> filter)
        : CachingFilteringSourceAccessor(
              SourcePath(next),
              [filter](const CanonPath & path) {
                  auto stat = filter->match(path, SourceAccessor::tUnknown);
                  if (stat.line)
                      return RestrictedPathError("path '%s' is excluded by source filter line %d", path, *stat.line);
                  return RestrictedPathError("path '%s' is excluded by the flake source policy", path);
              })
        , filter(std::move(filter))
    {
    }

    bool isAllowedUncached(const CanonPath & path) override
    {
        if (path.isRoot())
            return true;
        auto stat = next->maybeLstat(prefix / path);
        if (!stat)
            return true;
        return !filter->match(path, stat->type).ignored;
    }
};

} // namespace

ref<SourceAccessor> makeNixIgnoreSourceAccessor(ref<SourceAccessor> source, NixIgnoreFilter filter)
{
    auto sharedFilter = std::make_shared<const NixIgnoreFilter>(std::move(filter));
    return make_ref<NixIgnoreSourceAccessor>(std::move(source), std::move(sharedFilter));
}

} // namespace nix::fetchers
