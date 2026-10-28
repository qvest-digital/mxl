// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
// SPDX-License-Identifier: Apache-2.0

// Releasing the last writer of a flow deletes the flow, and nothing after that may walk a path to the flow's name.
//
// A domain on a filesystem that waits for a missing flow name to appear blocks such a walk for the whole wait, and the writer's
// release holds whatever lock its caller holds for as long. tmpfs answers the same walk at once, so the only way to see it here is to
// watch the calls: the stat family and open are interposed in this executable, and every one of them that finds no such file is
// recorded with its path.

#include <cerrno>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <catch2/catch_test_macros.hpp>
#include "mxl/flow.h"
#include "mxl/mxl.h"
#include "Utils.hpp"

namespace
{
    std::mutex walksMutex;
    bool recording = false;
    std::vector<std::string> missingWalks;

    template<typename F>
    F next(char const* name)
    {
        return reinterpret_cast<F>(::dlsym(RTLD_NEXT, name));
    }

    // Records a call that found no such file, and leaves errno as the call left it.
    int noteMissing(int result, char const* path)
    {
        auto const error = errno;
        if ((result == -1) && (error == ENOENT) && (path != nullptr))
        {
            auto const lock = std::scoped_lock{walksMutex};
            if (recording)
            {
                missingWalks.emplace_back(path);
            }
        }
        errno = error;
        return result;
    }

    std::vector<std::string> missingWalksTo(std::string const& name)
    {
        auto const lock = std::scoped_lock{walksMutex};
        auto found = std::vector<std::string>{};
        for (auto const& walk : missingWalks)
        {
            if (walk.find(name) != std::string::npos)
            {
                found.push_back(walk);
            }
        }
        return found;
    }

    void record(bool on)
    {
        auto const lock = std::scoped_lock{walksMutex};
        recording = on;
        if (on)
        {
            missingWalks.clear();
        }
    }
}

extern "C"
{
    __attribute__((visibility("default"))) int stat(char const* path, struct stat* st)
    {
        return noteMissing(next<int (*)(char const*, struct stat*)>("stat")(path, st), path);
    }

    __attribute__((visibility("default"))) int lstat(char const* path, struct stat* st)
    {
        return noteMissing(next<int (*)(char const*, struct stat*)>("lstat")(path, st), path);
    }

    __attribute__((visibility("default"))) int fstatat(int dirfd, char const* path, struct stat* st, int flags)
    {
        return noteMissing(next<int (*)(int, char const*, struct stat*, int)>("fstatat")(dirfd, path, st, flags), path);
    }

    __attribute__((visibility("default"))) int statx(int dirfd, char const* path, int flags, unsigned int mask, struct statx* st)
    {
        return noteMissing(next<int (*)(int, char const*, int, unsigned int, struct statx*)>("statx")(dirfd, path, flags, mask, st), path);
    }

    __attribute__((visibility("default"))) int access(char const* path, int mode)
    {
        return noteMissing(next<int (*)(char const*, int)>("access")(path, mode), path);
    }

    __attribute__((visibility("default"))) int open(char const* path, int flags, ...)
    {
        auto args = va_list{};
        va_start(args, flags);
        auto const mode = ((flags & O_CREAT) != 0) ? static_cast<mode_t>(va_arg(args, int)) : mode_t{0};
        va_end(args);
        return noteMissing(next<int (*)(char const*, int, ...)>("open")(path, flags, mode), path);
    }

    __attribute__((visibility("default"))) int openat(int dirfd, char const* path, int flags, ...)
    {
        auto args = va_list{};
        va_start(args, flags);
        auto const mode = ((flags & O_CREAT) != 0) ? static_cast<mode_t>(va_arg(args, int)) : mode_t{0};
        va_end(args);
        return noteMissing(next<int (*)(int, char const*, int, ...)>("openat")(dirfd, path, flags, mode), path);
    }
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Releasing the last writer walks to no missing flow name", "[writer][release]")
{
    auto const flowId = std::string{"5fbec3b1-1b0f-417d-9059-8b94a47197ed"};
    auto const flowName = flowId + ".mxl-flow";
    auto const flowDef = mxl::tests::readFile("data/v210_flow.json");

    auto instance = mxlCreateInstance(domain.c_str(), "");
    REQUIRE(instance != nullptr);

    auto writer = mxlFlowWriter{};
    auto created = false;
    REQUIRE(mxlCreateFlowWriter(instance, flowDef.c_str(), nullptr, &writer, nullptr, &created) == MXL_STATUS_OK);
    REQUIRE(created);

    record(true);
    REQUIRE(mxlReleaseFlowWriter(instance, writer) == MXL_STATUS_OK);
    record(false);

    // The release deleted the flow, which is what makes any later walk to its name a walk to a missing name.
    REQUIRE_FALSE(std::filesystem::exists(domain / flowName));
    auto const walks = missingWalksTo(flowName);
    for (auto const& walk : walks)
    {
        UNSCOPED_INFO("walked to missing " << walk);
    }
    CHECK(walks.empty());

    REQUIRE(mxlDestroyInstance(instance) == MXL_STATUS_OK);
}
