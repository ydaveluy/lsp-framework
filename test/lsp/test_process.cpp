#include <lsp/process.h>

#if defined(LSP_PROCESS_POSIX) && defined(LSP_TEST_PROCESS_IMPLEMENTATION)
#include <cstdint>
#include <string>
#include <sys/stat.h>

namespace{

auto pipeIdentity(int fd) -> std::string
{
	struct stat status{};
	if(fstat(fd, &status) != 0 || !S_ISFIFO(status.st_mode))
		return {};
	return std::to_string(static_cast<std::uintmax_t>(status.st_dev)) + ':' +
	       std::to_string(static_cast<std::uintmax_t>(status.st_ino));
}

}
#endif

#ifdef LSP_TEST_PROCESS_IMPLEMENTATION

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#include <lsp/io/stream.h>
#include <lsp/io/standard_io.h>
#ifdef LSP_PROCESS_POSIX
#include <unistd.h>
#include <signal.h>
#endif

using namespace lsp;

int main(int argc, char** argv)
{
	if(argc < 1) // Should never happen
		return -1;

	if(argc == 1)
		return 0;

	if(std::strcmp(argv[1], "SingleArg") == 0)
	{
		return 1;
	}
	else if(std::strcmp(argv[1], "MultiArg") == 0)
	{
		if(std::strcmp(argv[2], "--first") == 0 &&
		   std::strcmp(argv[3], "second") == 0 &&
		   std::strcmp(argv[4], "foo") == 0 &&
		   std::strcmp(argv[5], "--bar") == 0)
		{
			return 2;
		}
	}
	else if(std::strcmp(argv[1], "EscapedArgs") == 0)
	{
		if(std::strcmp(argv[2], "hello world") == 0 &&
		   std::strcmp(argv[3], "C:\\Program Files\\") == 0 &&
		   std::strcmp(argv[4], "quote\"inside") == 0 &&
		   std::strcmp(argv[5], "") == 0 &&
		   std::strcmp(argv[6], "back\\slash") == 0 &&
		   std::strcmp(argv[7], "\"") == 0 &&
		   std::strcmp(argv[8], "it's") == 0)
		{
			return 3;
		}
	}
	else if(std::strcmp(argv[1], "StdIn") == 0)
	{
		const auto expected   = std::string_view("Hello World!");
		char       buffer[16] = {};

		if(std::fread(buffer, expected.size(), 1, stdin) == 1 && buffer == expected)
			return 4;
	}
	else if(std::strcmp(argv[1], "StdOut") == 0)
	{
		std::fprintf(stdout, "Hello World!");
		std::fflush(stdout);
		return 5;
	}
	else if(std::strcmp(argv[1], "StdErr") == 0)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::fprintf(stderr, "Hello Error!");
		std::fflush(stderr);
		return 8;
	}
	else if(std::strcmp(argv[1], "StdOutAndStdErr") == 0)
	{
		std::fprintf(stdout, "out");
		std::fflush(stdout);
		std::fprintf(stderr, "err");
		std::fflush(stderr);
		return 9;
	}
	else if(std::strcmp(argv[1], "StdIO") == 0)
	{
		std::fprintf(stdout, "Hello World!");
		std::fflush(stdout);

		const auto expected   = std::string_view("ping");
		char       buffer[16] = {};

		if(std::fread(buffer, expected.size(), 1, stdin) == 1 && buffer == expected)
		{
			std::fprintf(stdout, "pong");
			std::fflush(stdout);
			return 6;
		}
	}
	else if(std::strcmp(argv[1], "ProcessId") == 0)
	{
		int id = -1;

		if(std::scanf("%d", &id) == 1 && Process::currentProcessId() == id)
			return 7;
	}
	else if(std::strcmp(argv[1], "Terminate") == 0)
	{
		for(;;)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	else if(std::strcmp(argv[1], "StandardIOReadEOF") == 0)
	{
		char buffer[2];
		try{ io::standardIO().read(buffer, sizeof(buffer)); }
		catch(const io::Error&){ return 10; }
	}
	else if(std::strcmp(argv[1], "StandardIOZeroSize") == 0)
	{
		io::standardIO().read(nullptr, 0);
		io::standardIO().write(nullptr, 0);
		return 11;
	}
#ifdef __linux__
	else if(std::strcmp(argv[1], "StandardIOFlushFailure") == 0)
	{
		if(!std::freopen("/dev/full", "wb", stdout))
			return -1;
		try{ io::standardIO().write("x", 1); }
		catch(const io::Error&){ return 13; }
	}
#endif
	else if(std::strcmp(argv[1], "ClosedStdin") == 0)
	{
		std::fclose(stdin);
		std::fprintf(stdout, "ready");
		std::fflush(stdout);
		for(;;)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	else if(std::strcmp(argv[1], "WaitForInput") == 0)
	{
		char byte;
		return std::fread(&byte, 1, 1, stdin) == 1 ? 0 : -1;
	}
#ifdef LSP_PROCESS_WIN32
	else if(std::strcmp(argv[1], "ExitCodeStillActive") == 0)
	{
		char byte;
		(void)std::fread(&byte, 1, 1, stdin);
		return 259;
	}
#endif
#ifdef LSP_PROCESS_POSIX
	else if(std::strcmp(argv[1], "StandardIOClosedStdout") == 0)
	{
		int fds[2];
		if(pipe(fds) != 0)
			return -1;
		close(fds[0]);
		if(dup2(fds[1], STDOUT_FILENO) == -1)
			return -1;
		close(fds[1]);
		signal(SIGPIPE, SIG_DFL);
		try{ io::standardIO().write("x", 1); }
		catch(const io::Error&)
		{
			close(STDOUT_FILENO);
			return 14;
		}
		close(STDOUT_FILENO);
		return -1;
	}
	else if(std::strcmp(argv[1], "InheritedDescriptors") == 0)
	{
		for(int fd = STDERR_FILENO + 1; fd < 4096; ++fd)
		{
			const auto identity = pipeIdentity(fd);
			if(identity.empty())
				continue;
			for(int arg = 2; arg < argc; ++arg)
				if(identity == argv[arg])
					return 12;
		}
		return 0;
	}
	else if(std::strcmp(argv[1], "PipeIdentities") == 0)
	{
		for(int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd)
		{
			const auto identity = pipeIdentity(fd);
			if(identity.empty())
				return -1;
			std::fprintf(stdout, "%s\n", identity.c_str());
		}
		std::fflush(stdout);
		for(;;)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
#endif

	return -1; // Test failure
}

#else

#include <chrono>
#include <string>
#include <thread>
#include <filesystem>
#include <test/test.h>
#include <lsp/io/stream.h>
#ifdef LSP_PROCESS_POSIX
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

using namespace lsp;

#ifndef LSP_TEST_PROCESS_EXE
	#define LSP_TEST_PROCESS_EXE "TestProcess"
#endif

int main(int argc, char** argv)
{
	auto app = test::TestApp();

	app.addTest("DefaultConstructed", [](){
		auto proc = Process();

		test::check(!proc.isRunning(), "!isRunning");
		test::compare(proc.id(), -1);
		test::compare(proc.wait(), -1);
		test::expectException<ProcessError>([&proc](){ (void)proc.stdIO(); }, "Process is not running - Cannot get stdio");
		proc.terminate(); // Should be a noop and not throw or crash...
	});

	app.addTest("InvalidExecutable", [](){
		test::expectException<ProcessError>([]()
		{
			(void)Process::start("DoesNotExist");
		});
	});

	app.addTest("EmbeddedNullArgumentsAreRejected", [](){
		auto executable = std::string(LSP_TEST_PROCESS_EXE);
		executable += '\0';
		executable += "suffix";
		test::expectException<ProcessError>([&executable](){ (void)Process::start(executable); });
		test::expectException<ProcessError>([](){ (void)Process::start(LSP_TEST_PROCESS_EXE, {std::string("x\0y", 3)}); });
	});

	app.addTest("NoArgs", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE);
		test::compare(proc.wait(), 0);
	});

	app.addTest("NaturalExit", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE);
		std::this_thread::sleep_for(std::chrono::seconds(1)); // Not 100% reliable but after a second the process should have finished...
		test::check(!proc.isRunning(), "!isRunning");
		proc.terminate();
		test::compare(proc.wait(), -1); // Terminate reset the internal state even if called after process finish, so the exit code is not valid
	});

	app.addTest("SingleArg", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"SingleArg"});
		test::compare(proc.wait(), 1);
	});

	app.addTest("MultiArg", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"MultiArg", "--first", "second", "foo", "--bar"});
		test::compare(proc.wait(), 2);
	});

	app.addTest("EscapedArgs", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {
			"EscapedArgs",
			"hello world",
			"C:\\Program Files\\",
			"quote\"inside",
			"",
			"back\\slash",
			"\"",
			"it's"
		});
		test::compare(proc.wait(), 3);
	});

	app.addTest("StdIn", [](){
		auto       proc  = Process::start(LSP_TEST_PROCESS_EXE, {"StdIn"});
		auto&      stdIO = proc.stdIO();
		const auto msg   = std::string_view("Hello World!");

		stdIO.write(msg.data(), msg.size());
		test::compare(proc.wait(), 4);
	});

	app.addTest("StdOut", [](){
		auto       proc       = Process::start(LSP_TEST_PROCESS_EXE, {"StdOut"});
		auto&      stdIO      = proc.stdIO();
		const auto expected   = std::string_view("Hello World!");
		char       buffer[16] = {};

		stdIO.read(buffer, expected.size());
		test::compare(buffer, expected);
		test::compare(proc.wait(), 5);
	});

	app.addTest("StdIO", [](){
		auto  proc       = Process::start(LSP_TEST_PROCESS_EXE, {"StdIO"});
		auto& stdIO      = proc.stdIO();
		auto  expected   = std::string_view("Hello World!");
		char  buffer[16] = {};

		stdIO.read(buffer, expected.size());
		test::compare(buffer, expected);
		stdIO.write("ping", 4);
		expected = "pong";
		buffer[expected.size()] = '\0';
		stdIO.read(buffer, expected.size());
		test::compare(buffer, expected);
		test::compare(proc.wait(), 6);
	});

	app.addTest("StdInReadPastEOF", [](){
		auto  proc       = Process::start(LSP_TEST_PROCESS_EXE);
		auto& stdIO      = proc.stdIO();

		test::expectException<io::Error>([&stdIO](){
			char buf[1];
			stdIO.read(buf, sizeof(buf));
		}, "Reached EOF");
	});

	app.addTest("StdErr", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StdErr"});
		test::compare(proc.readAvailableStdErr(), std::string());
		std::this_thread::sleep_for(std::chrono::seconds(2)); // Give process time to write to stderr
		test::compare(proc.readAvailableStdErr(), std::string_view("Hello Error!"));
		test::compare(proc.readAvailableStdErr(), std::string());
		test::compare(proc.wait(), 8);
	});

	app.addTest("StdOutAndStdErrAreSeparate", [](){
		auto       proc       = Process::start(LSP_TEST_PROCESS_EXE, {"StdOutAndStdErr"});
		auto&      stdIO      = proc.stdIO();
		const auto expected   = std::string_view("out");
		char       buffer[16] = {};

		stdIO.read(buffer, expected.size());
		test::compare(std::string_view(buffer, expected.size()), expected);
		auto stderrText = std::string();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			stderrText += proc.readAvailableStdErr();
			if(stderrText.size() >= 3)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		while(std::chrono::steady_clock::now() < deadline);
		test::compare(stderrText, std::string_view("err"));
		test::compare(proc.wait(), 9);
	});

	app.addTest("IsRunningDoesNotDiscardBufferedStdErr", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StdOutAndStdErr"});

		std::this_thread::sleep_for(std::chrono::seconds(1)); // let the process write its output and exit

		test::check(!proc.isRunning(), "!isRunning");
		test::check(!proc.isRunning(), "!isRunningIdempotent");

		test::compare(proc.readAvailableStdErr(), std::string_view("err"));
		test::compare(proc.wait(), 9);
	});

	app.addTest("ProcessId", [](){
		auto       proc  = Process::start(LSP_TEST_PROCESS_EXE, {"ProcessId"});
		auto&      stdIO = proc.stdIO();
		const auto idStr = std::to_string(proc.id());

		stdIO.write(idStr.data(), idStr.size());
		test::compare(proc.wait(), 7);
	});

	app.addTest("Terminate", [](){
		auto       proc = Process::start(LSP_TEST_PROCESS_EXE, {"Terminate"});
		const auto id   = proc.id();

		test::check(proc.isRunning(), "isRunning");
		test::check(id != -1, "validId");
		test::check(Process::exists(id), "processExists");
		proc.terminate();
		test::check(!proc.isRunning(), "!isRunning");
		test::compare(proc.id(), -1);
		test::compare(proc.wait(), -1);
		test::check(!Process::exists(id), "!processExists");
	});

	app.addTest("CurrentProcessIdExists", [](){
		const auto id = Process::currentProcessId();
		test::check(Process::exists(id));
	});

	app.addTest("StandardIOReadReportsTruncatedInput", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StandardIOReadEOF"});
		proc.stdIO().write("x", 1);
		test::compare(proc.wait(), 10);
	});

	app.addTest("StandardIOAcceptsZeroSizeTransfers", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StandardIOZeroSize"});
		test::compare(proc.wait(), 11);
	});

	app.addTest("ClosedStdinWriteThrows", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"ClosedStdin"});
		auto& stdIO = proc.stdIO();
		char ready[5];
		bool failed = false;
		try
		{
			stdIO.read(ready, sizeof(ready));
			try{ stdIO.write("x", 1); }
			catch(const io::Error&){ failed = true; }
		}
		catch(...)
		{
			proc.terminate();
			throw;
		}
		proc.terminate();
		test::compare(std::string_view(ready, sizeof(ready)), "ready");
		test::check(failed);
	});

#ifdef LSP_PROCESS_POSIX
	app.addTest("ClosedStdoutWriteThrowsWithoutSignal", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StandardIOClosedStdout"});
		test::compare(proc.wait(), 14);
	});

	app.addTest("SignaledExitSurvivesIsRunning", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"Terminate"});
		test::compare(kill(proc.id(), SIGTERM), 0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while(proc.isRunning() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if(proc.isRunning())
		{
			proc.terminate();
			test::fail("Process did not exit before the deadline");
		}
		test::compare(proc.wait(), 128 + SIGTERM);
	});

	app.addTest("OtherProcessesDoNotInheritPipes", [](){
		auto running = Process::start(LSP_TEST_PROCESS_EXE, {"PipeIdentities"});
		auto args = Process::ArgList{"InheritedDescriptors"};
		int exitCode = -1;
		try
		{
			auto& stream = running.stdIO();
			for(int line = 0; line < 3; ++line)
			{
				auto identity = std::string();
				for(;;)
				{
					char byte;
					stream.read(&byte, 1);
					if(byte == '\n')
						break;
					identity += byte;
				}
				args.push_back(std::move(identity));
			}
			auto probe = Process::start(LSP_TEST_PROCESS_EXE, args);
			exitCode = probe.wait();
		}
		catch(...)
		{
			running.terminate();
			throw;
		}
		running.terminate();
		test::compare(exitCode, 0);
	});

	app.addTest("ClosedStandardDescriptorsDoNotAliasPipes", [](){
		const auto pid = fork();
		test::check(pid != -1);
		if(pid == 0)
		{
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			try
			{
				auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StdIn"});
				proc.stdIO().write("Hello World!", 12);
				_exit(proc.wait() == 4 ? 0 : 1);
			}
			catch(...){ _exit(1); }
		}
		int status = 0;
		test::compare(waitpid(pid, &status, 0), pid);
		test::check(WIFEXITED(status));
		test::compare(WEXITSTATUS(status), 0);
	});
#endif

#ifdef __linux__
	app.addTest("StandardIOReportsFlushFailure", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"StandardIOFlushFailure"});
		test::compare(proc.wait(), 13);
	});

	app.addTest("NaturalExitClosesPipeDescriptors", [](){
		const auto descriptorCount = []()
		{
			return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator()));
		};
		const auto before = descriptorCount();
		for(int i = 0; i < 12; ++i)
		{
			auto proc = Process::start(LSP_TEST_PROCESS_EXE);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while(proc.isRunning() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			test::check(!proc.isRunning());
			test::compare(proc.wait(), 0);
		}
		test::compare(descriptorCount(), before);
	});
#endif

	app.addTest("MoveConstructed", [](){
		auto proc     = Process::start(LSP_TEST_PROCESS_EXE, {"WaitForInput"});
		const auto id = proc.id();
		auto proc2    = Process(std::move(proc));

		test::check(id != -1, "validId");
		test::compare(proc.id(), -1);
		test::compare(proc2.id(), id);
		test::compare(proc.wait(), -1);
		proc2.stdIO().write("x", 1);
		test::compare(proc2.wait(), 0);
	});

	app.addTest("MoveAssigned", [](){
		auto       proc  = Process::start(LSP_TEST_PROCESS_EXE, {"WaitForInput"});
		const auto id    = proc.id();
		auto       proc2 = Process();

		proc2 = std::move(proc);
		test::check(id != -1, "validId");
		test::compare(proc.id(), -1);
		test::compare(proc2.id(), id);
		test::compare(proc.wait(), -1);
		proc2.stdIO().write("x", 1);
		test::compare(proc2.wait(), 0);
	});

#ifdef LSP_PROCESS_WIN32
	app.addTest("StillActiveExitCodeDoesNotMeanRunning", [](){
		auto proc = Process::start(LSP_TEST_PROCESS_EXE, {"ExitCodeStillActive"});
		proc.stdIO().write("x", 1);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while(proc.isRunning() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if(proc.isRunning())
		{
			proc.terminate();
			test::fail("Exited process still reported as running");
		}
		test::compare(proc.wait(), 259);
	});
#endif

	return app.main(argc, argv);
}

#endif
