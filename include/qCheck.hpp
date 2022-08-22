#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include <getopt.h>

struct Settings
{
	std::vector<std::filesystem::path> InputFiles;
	std::size_t                        Threads = 2;
	bool                               Verbose = true;
	bool                               Check   = false;
};

extern const char* Usage;

const static struct option CommandOptions[]
	= {{"threads", required_argument, nullptr, 't'},
	   {"check", no_argument, nullptr, 'c'},
	   {"help", no_argument, nullptr, 'h'},
	   {nullptr, no_argument, nullptr, '\0'}};

std::optional<std::uint32_t> ChecksumFile(const std::filesystem::path& Path);
int                          Check(const Settings& CurSettings);

void ChecksumThread(
	std::atomic<std::size_t>&              FileIndex,
	std::span<const std::filesystem::path> FileList, std::size_t WorkerIndex);