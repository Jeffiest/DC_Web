#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "moonmod/mods_directory.hpp"

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path root;
    TempTree() {
        root = fs::temp_directory_path() / ("moonmod-mods-dir-" + std::to_string(std::random_device{}()));
        fs::create_directories(root);
    }
    ~TempTree() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path Dir(const fs::path &relative) const {
        fs::create_directories(root / relative);
        return root / relative;
    }
    void File(const fs::path &relative) const {
        fs::create_directories((root / relative).parent_path());
        std::ofstream(root / relative) << "";
    }
};

} // namespace

TEST(MoonModModsDirectory, CheckoutModsFolderWins) {
    TempTree tree;
    tree.File("Chronicle/kazarov.conf");
    const fs::path mods = tree.Dir("Chronicle/mods");
    const fs::path exe = tree.Dir("Chronicle/port/build/pc-release");
    tree.Dir("Chronicle/port/build/pc-release/mods");
    EXPECT_EQ(moonmod::ModsDirectory(exe, tree.root / "Chronicle/port/run/save"), mods);
}

TEST(MoonModModsDirectory, CheckoutWithoutModsFallsBackToSave) {
    TempTree tree;
    tree.File("Chronicle/kazarov.conf");
    const fs::path exe = tree.Dir("Chronicle/port/build/pc-release");
    const fs::path save = tree.root / "Chronicle/port/run/save";
    EXPECT_EQ(moonmod::ModsDirectory(exe, save), save / "mods");
}

TEST(MoonModModsDirectory, CopiedBuildUsesModsBesideExecutable) {
    TempTree tree;
    const fs::path exe = tree.Dir("Games/DarkCloud");
    const fs::path mods = tree.Dir("Games/DarkCloud/mods");
    EXPECT_EQ(moonmod::ModsDirectory(exe, tree.root / "save"), mods);
}

TEST(MoonModModsDirectory, OtherwiseSaveMods) {
    TempTree tree;
    const fs::path exe = tree.Dir("Games/DarkCloud");
    EXPECT_EQ(moonmod::ModsDirectory(exe, tree.root / "save"), tree.root / "save" / "mods");
    EXPECT_EQ(moonmod::ModsDirectory({}, tree.root / "save"), tree.root / "save" / "mods");
}
