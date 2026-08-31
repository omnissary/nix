#include "nix/fetchers/nixignore.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

namespace nix::fetchers {

namespace {

bool ignored(const NixIgnoreRules & rules, std::string_view path, SourceAccessor::Type type = SourceAccessor::tRegular)
{
    return rules.match(CanonPath(path), type).ignored;
}

} // namespace

TEST(NixIgnoreRules, commentsEscapesAndTrailingSpaces)
{
    auto rules = NixIgnoreRules::parse("\n# comment\n\\#hash\n\\!bang\ntrimmed   \nliteral\\ \n");

    EXPECT_TRUE(ignored(rules, "#hash"));
    EXPECT_TRUE(ignored(rules, "!bang"));
    EXPECT_TRUE(ignored(rules, "trimmed"));
    EXPECT_TRUE(ignored(rules, "literal "));
    EXPECT_FALSE(ignored(rules, "comment"));
}

TEST(NixIgnoreRules, rootedAndUnrootedPatterns)
{
    auto rules = NixIgnoreRules::parse("/root-only\nanywhere\ndir/file\n");

    EXPECT_TRUE(ignored(rules, "root-only"));
    EXPECT_FALSE(ignored(rules, "nested/root-only"));
    EXPECT_TRUE(ignored(rules, "nested/anywhere"));
    EXPECT_TRUE(ignored(rules, "dir/file"));
    EXPECT_FALSE(ignored(rules, "nested/dir/file"));
}

TEST(NixIgnoreRules, wildcardsAndCharacterClasses)
{
    auto rules = NixIgnoreRules::parse("*.tmp\nfile?.txt\nitem[0-2]\ndash[a\\-z]\na/**/generated\ncache/**\n");

    EXPECT_TRUE(ignored(rules, "deep/file.tmp"));
    EXPECT_TRUE(ignored(rules, "file1.txt"));
    EXPECT_FALSE(ignored(rules, "file10.txt"));
    EXPECT_TRUE(ignored(rules, "item2"));
    EXPECT_FALSE(ignored(rules, "item3"));
    EXPECT_TRUE(ignored(rules, "dash-"));
    EXPECT_FALSE(ignored(rules, "dashm"));
    EXPECT_TRUE(ignored(rules, "a/generated"));
    EXPECT_TRUE(ignored(rules, "a/b/c/generated"));
    EXPECT_TRUE(ignored(rules, "cache/deep/file"));
    EXPECT_FALSE(ignored(rules, "cache", SourceAccessor::tDirectory));
}

TEST(NixIgnoreRules, directoryPatternsAndNegation)
{
    auto rules = NixIgnoreRules::parse("build/\n!build\n/build/*\n!/build/keep\n");

    EXPECT_FALSE(ignored(rules, "build", SourceAccessor::tDirectory));
    EXPECT_TRUE(ignored(rules, "build/drop"));
    EXPECT_FALSE(ignored(rules, "build/keep"));
    EXPECT_FALSE(ignored(rules, "build", SourceAccessor::tRegular));
}

TEST(NixIgnoreRules, cannotReincludeBelowIgnoredParent)
{
    auto rules = NixIgnoreRules::parse("private/\n!private/public.txt\n");
    EXPECT_TRUE(ignored(rules, "private/public.txt"));
}

TEST(NixIgnoreRules, matchingIsCaseSensitive)
{
    auto rules = NixIgnoreRules::parse("Secret\n");
    EXPECT_TRUE(ignored(rules, "Secret"));
    EXPECT_FALSE(ignored(rules, "secret"));
}

TEST(NixIgnoreMode, preservesLegacyFinalInputsAndVersionsNewOnes)
{
    EnableExperimentalFeature enableFlakes("flakes");
    Input input;
    EXPECT_EQ(getNixIgnoreMode(input), NixIgnoreMode::Discover);

    input.attrs.insert_or_assign("__final", Explicit<bool>{true});
    EXPECT_EQ(getNixIgnoreMode(input), NixIgnoreMode::Disabled);

    input.attrs.insert_or_assign("nixIgnore", nixIgnoreVersion);
    EXPECT_EQ(getNixIgnoreMode(input), NixIgnoreMode::Required);

    input.attrs.insert_or_assign("nixIgnore", uint64_t(2));
    EXPECT_THROW(getNixIgnoreMode(input), Error);
}

TEST(NixIgnoreSourceAccessor, filtersDirectoryAndDirectAccess)
{
    auto source = make_ref<MemorySourceAccessor>();
    source->addFile(CanonPath(".nixignore"), std::string("secret/\n"));
    source->addFile(CanonPath("visible"), std::string("ok"));
    source->addFile(CanonPath("secret/value"), std::string("do not read"));

    auto rules = readNixIgnore(source);
    ASSERT_TRUE(rules);
    auto filtered = makeNixIgnoreSourceAccessor(source, std::move(*rules));

    auto entries = filtered->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains(".nixignore"));
    EXPECT_TRUE(entries.contains("visible"));
    EXPECT_FALSE(entries.contains("secret"));
    EXPECT_FALSE(filtered->pathExists(CanonPath("secret/value")));
    EXPECT_THROW(filtered->readFile(CanonPath("secret/value")), RestrictedPathError);
}

TEST(NixIgnoreSourceAccessor, rejectsSelfExclusion)
{
    auto source = make_ref<MemorySourceAccessor>();
    source->addFile(CanonPath(".nixignore"), std::string(".nixignore\n"));
    EXPECT_THROW(readNixIgnore(source), Error);
}

} // namespace nix::fetchers
