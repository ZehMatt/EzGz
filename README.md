# EzGz
A single header library for easily and quickly decompressing Gz archives, written in modern C++. It's designed to be both easy to use and highly performant.

## Installation
Just add `ezgz.hpp` into your project, it contains all the functionality and depends only on the C++20 standard library. You can use git subtree to get updates cleanly.

## Usage
### Decompression
The easiest way of using this is to use the `IGzStream`. It inherits from `std::istream`, so it's usable as any other C++ input stream:
```C++
Ezgz::IGzStream input("data.gz");
std::string line;
while (std::getline(input, line)) { // You can read it by lines for example
	std::cout << line << std::endl;
}
```

It can also be constructed from a `std::istream` to read data from, `std::span<const uint8_t>` holding raw data or a `std::function<int(std::span<uint8_t> batch)>` that fills the span in its argument with data and returns how many bytes it wrote. All constructors accept an optional argument that determines the number of bytes reachable through `unget()` (10 by default).

If you don't want to use a standard stream, you can use `IGzFile`, which gives a slightly lower level approach:
```C++
Ezgz::IGzFile<> input(data); // Expecting the file's contents is already a contiguous container
while (std::optional<std::span<const char>> chunk = input.readSome()) {
	output.put(*chunk);
}
```

It supports some other ways of reading the data (the separator is newline by default, other separators can be set as second argument):
```C++
Ezgz::IGzFile<> input([&file] (std::span<uint8_t> batch) -> int {
	// Fast reading from stream
	file.read(reinterpret_cast<char*>(batch.data()), batch.size());
	return input.gcount();
});
data.readByLines([&] (std::span<const char> chunk) {
	std::cout << chunk << std::endl;
});
```

Or simply:
```C++
std::vector<char> decompressed = Ezgz::IGzFile<>("data.gz").readAll();
```

If the data is only deflate-compressed and not in an archive, you should use `IDeflateFile` instead of `IGzFile`. But in that case, it will most likely be already in some buffer, in which case, it's more convenient to do this:
```C++
std::vector<char> decompressed = Ezgz::readDeflateIntoVector(data);
```
The function has an overload that accepts a functor that fill buffers with input data and returns the amount of data filled.

#### Configuration
Most classes and free functions accept a template argument whose values allow tuning some properties:
* `maxOutputBufferSize` - maximum number of bytes in the output buffer, if filled, decompression will stop to empty it
* `minOutputBufferSize` - must be at least 32768 for correct decompression, decompression may fail if smaller but can save some memory
* `inutBufferSize` - the input buffer's size, decides how often is the function to fill more data called
* `verifyChecksum` - boolean whether to verify the checksum after parsing the file
* `Checksum` - a class that computers the CRC32 checksum, 3 are available:
  * `NoChecksum` - does nothing, can save some time if checksum isn't checked or isn't known
  * `LightCrc32` - uses a 1 kiB table (precomputed at compile time), slow on modern CPUs
  * `FastCrc32` - uses a 16 kiB table (precomputed at compile time), works well with out of order execution

You can either declare your own struct or inherit from a default one and adjust only what you want:
```C++
struct Settings : Ezgz::DefaultDecompressionSettings {
	constexpr static int inputBufferSize = 30000;
	using Checksum = EzGz::NoChecksum;
	constexpr static bool verifyChecksum = false;
}; // This will skip checksum
std::vector<char> decompressed = Ezgz::IGzFile<Settings>("data.gz").readAll();
```

## Performance
Decompression speeds over 250 MiB/s are possible on modern CPUs, making it about 10% faster than `zlib`. It was tested on the standard Silesia Corpus file, compressed for minimum size.

Using it through `std::ostream` has no noticeable impact on performance but any type of parsing will impact it significantly.

## Code remarks
The type used to represent bytes of compressed data is `uint8_t`. The type to represent bytes of uncompressed data is `char`. Some casting is necessary, but it usually makes it clear which data are compressed which aren't.

`I` is the prefix for _input,_ `O` is the prefix for _output._

Errors are handled with exceptions. Unless there is a bug, an error happens only if the input file is incorrect. All exceptions inherit from `std::exception`, parsing errors are `std::runtime_error`, internal errors with `std::logic_error` (these should not appear unless there is a bug). In absence of RTTI, catching an exception almost certainly means the file is corrupted (if compiling with exceptions disabled, exceptions have to be enabled for the file that includes this header, performance would be worse without them). Exceptions thrown during decompression mean the entire output may be invalid (checksum failures are detected only at the end of file). If an exception is thrown inside a function that fills an input buffer, it will be propagated.

The decompression algorithm itself does not use dynamic allocation. All buffers and indexes are on stack. Exceptions, string values obtained from the files (like names) and callbacks done using `std::function` may dynamically allocate.