#include <gtest/gtest.h>

import std;
import mcpp.build.plan;

using namespace mcpp::build;

TEST(ModuleExtensions, CppmGetsDotMPrefix) {
    EXPECT_EQ(object_filename_for("src/foo.cppm", ".o"), "foo.m.o");
}

TEST(ModuleExtensions, CcmGetsDotMPrefix) {
    EXPECT_EQ(object_filename_for("src/foo.ccm", ".o"), "foo.m.o");
}

TEST(ModuleExtensions, CxxmGetsDotMPrefix) {
    EXPECT_EQ(object_filename_for("src/foo.cxxm", ".o"), "foo.m.o");
}

TEST(ModuleExtensions, IxxGetsDotMPrefix) {
    EXPECT_EQ(object_filename_for("src/foo.ixx", ".o"), "foo.m.o");
}

TEST(ModuleExtensions, CppHasNoPrefix) {
    EXPECT_EQ(object_filename_for("src/foo.cpp", ".o"), "foo.o");
}

TEST(ModuleExtensions, CcHasNoPrefix) {
    EXPECT_EQ(object_filename_for("src/foo.cc", ".o"), "foo.o");
}

TEST(ModuleExtensions, AsmKeepsFullExt) {
    EXPECT_EQ(object_filename_for("src/foo.S", ".o"), "foo.S.o");
}
