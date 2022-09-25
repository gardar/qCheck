#include <qCheck.hpp>

#include <atomic>
#include <charconv>
#include <fstream>
#include <thread>
#include <utility>

#include <CRC/CRC32.hpp>

#include <fcntl.h>
#include <sys/mman.h>

const char* Usage
	= "qCheck - Wunkolo <wunkolo@gmail.com>\n"
	  "Usage: qCheck [Options]... [Files]...\n"
	  "  -t, --threads            Number of checker threads in parallel\n"
	  "  -c, --check              Verify all input as .sfv files\n"
	  "  -h, --help               Show this help message\n";

std::optional<std::uint32_t> ChecksumFile(const std::filesystem::path& Path)
{
	std::uint32_t     CRC32 = 0;
	std::error_code   CurError;
	const std::size_t FileSize = std::filesystem::file_size(Path, CurError);

	if( CurError )
	{
		return std::nullopt;
	}

	const int FileHandle = open(Path.c_str(), O_RDONLY, 0);
	if( FileHandle == -1 )
	{
		return std::nullopt;
	}

	// Try to map the file, upon failure, use regular file-descriptor reads
	void* FileMap = mmap(
		nullptr, FileSize, PROT_READ, MAP_SHARED | MAP_POPULATE, FileHandle, 0);

	if( std::uintptr_t(FileMap) != -1ULL )
	{
		const auto FileData = std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(FileMap), FileSize);

		madvise(FileMap, FileSize, MADV_SEQUENTIAL | MADV_WILLNEED);

		CRC32 = CRC::Checksum(FileData);

		munmap((void*)FileMap, FileSize);
	}
	else
	{
		std::array<std::byte, 4096> Buffer;

		ssize_t ReadCount = read(FileHandle, Buffer.data(), Buffer.size());
		while( ReadCount > 0 )
		{
			CRC32
				= CRC::Checksum(std::span(Buffer).subspan(0, ReadCount), CRC32);
			ReadCount = read(FileHandle, Buffer.data(), Buffer.size());
		}
	}

	close(FileHandle);

	return CRC32;
}

int Check(const Settings& CurSettings)
{
	struct CheckEntry
	{
		std::filesystem::path FilePath;
		std::uint32_t         Checksum;
	};
	std::atomic<std::size_t> QueueLock{0};
	std::vector<CheckEntry>  Checkqueue;

	// Queue up all files to be checked

	std::string CurLine;
	for( const auto& CurSfvPath : CurSettings.InputFiles )
	{
		std::ifstream CheckFile(CurSfvPath);
		if( !CheckFile )
		{
			std::fprintf(
				stdout, "Failed to open \"%s\" for reading\n",
				CurSfvPath.string().c_str());
			return EXIT_FAILURE;
		}

		while( std::getline(CheckFile, CurLine) )
		{
			if( CurLine[0] == ';' )
				continue;
			const std::size_t      BreakPos = CurLine.find_last_of(' ');
			const std::string_view PathString
				= std::string_view(CurLine).substr(0, BreakPos);
			const std::string_view CheckString
				= std::string_view(CurLine).substr(BreakPos + 1);
			std::uint32_t                CheckValue = ~0u;
			const std::from_chars_result ParseResult
				= std::from_chars<std::uint32_t>(
					CheckString.begin(), CheckString.end(), CheckValue, 16);
			if( ParseResult.ec != std::errc() )
			{
				// Error parsing checksum value
				continue;
			}
			std::filesystem::path FilePath;
			if( CurSfvPath.has_parent_path() )
			{
				FilePath = CurSfvPath.parent_path();
			}
			else
			{
				FilePath = ".";
			}
			FilePath /= PathString;
			Checkqueue.push_back({FilePath, CheckValue});
		}
	}

	std::vector<std::thread> Workers;

	for( std::size_t i = 0; i < CurSettings.Threads; ++i )
	{
		Workers.push_back(std::thread(
			[&QueueLock,
			 &Checkqueue = std::as_const(Checkqueue)](std::size_t WorkerIndex) {
#ifdef _POSIX_VERSION
				char ThreadName[16] = {0};
				std::sprintf(ThreadName, "qCheckWkr: %4zu", WorkerIndex);
				pthread_setname_np(pthread_self(), ThreadName);
#endif
				while( true )
				{
					const std::size_t EntryIndex = QueueLock.fetch_add(1);
					if( EntryIndex >= Checkqueue.size() )
						return;
					const CheckEntry& CurEntry = Checkqueue[EntryIndex];

					const std::optional<std::uint32_t> CurSum
						= ChecksumFile(CurEntry.FilePath);

					if( CurSum.has_value() )
					{
						const bool Valid = CurEntry.Checksum == CurSum;
						std::printf(
							"\e[36m%s\t\e[33m%08X\e[37m...%s%08X\t%s\e[0m\n",
							CurEntry.FilePath.c_str(), CurEntry.Checksum,
							Valid ? "\e[32m" : "\e[31m", CurSum.value(),
							Valid ? "\e[32mOK" : "\e[31mFAIL");
					}
					else
					{
						std::printf(
							"\e[36m%s\t\e[33m%08X\t\t\e[31mError opening "
							"file\n",
							CurEntry.FilePath.c_str(), CurEntry.Checksum);
					}
				}
			},
			i));
	}

	for( std::size_t i = 0; i < CurSettings.Threads; ++i )
		Workers[i].join();

	return EXIT_SUCCESS;
}

void ChecksumThread(
	std::atomic<std::size_t>&              FileIndex,
	std::span<const std::filesystem::path> FileList, std::size_t WorkerIndex)
{
#ifdef _POSIX_VERSION
	char ThreadName[16] = {0};
	std::sprintf(ThreadName, "qCheckWkr: %4zu", WorkerIndex);
	pthread_setname_np(pthread_self(), ThreadName);
#endif
	while( true )
	{
		const std::size_t EntryIndex = FileIndex.fetch_add(1);
		if( EntryIndex >= FileList.size() )
			return;
		const std::filesystem::path&       CurPath = FileList[EntryIndex];
		const std::optional<std::uint32_t> CRC32   = ChecksumFile(CurPath);
		// If writing to a terminal, put some pretty colored output
		if( CRC32.has_value() )
		{
			if( isatty(fileno(stdout)) )
			{
				std::fprintf(
					stdout, "\e[36m%s\t\e[33m%08X\e[0m\n",
					CurPath.filename().c_str(), CRC32.value());
			}
			else
			{
				std::fprintf(
					stdout, "%s %08X\n", CurPath.filename().c_str(),
					CRC32.value());
			}
		}
		else
		{
			if( isatty(fileno(stdout)) )
			{
				std::fprintf(
					stdout, "\e[36m%s\t\e[31mERROR\e[0m\n",
					CurPath.filename().c_str());
			}
			else
			{
				std::fprintf(stdout, "%s ERROR\n", CurPath.filename().c_str());
			}
		}
	}
}