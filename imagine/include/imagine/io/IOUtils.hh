#pragma once

/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#include <imagine/io/ioDefs.hh>
#include <imagine/util/memory/Buffer.hh>
#include <imagine/util/concepts.hh>
#include <memory>
#include <utility>

namespace IG
{

class IOBuffer : public ByteBuffer
{
public:
	using ByteBuffer::ByteBuffer;
	using Flags = uint8_t;

	static constexpr Flags MAPPED_FILE_BIT = bit(0);

	constexpr IOBuffer(std::span<uint8_t> span, Flags flags, DeleterFunc deleter = [](const uint8_t*, size_t){}):
		ByteBuffer{span, deleter}, flags{flags} {}

	constexpr bool isMappedFile() const { return flags & MAPPED_FILE_BIT; }

protected:
	Flags flags{};
};

template <ssize_t itemSize>
struct IOReadWriteResult
{
	ssize_t bytes{};
	ssize_t items{};

	constexpr IOReadWriteResult() = default;
	constexpr IOReadWriteResult(ssize_t bytes):
		bytes{bytes}, items{bytes / itemSize} {}
};

template <class IO>
class IOUtils
{
public:
	using AccessHint = IOAccessHint;
	using Advice = IOAdvice;
	using BufferMode = IOBufferMode;
	using SeekMode = IOSeekMode;

	off_t seek(off_t offset);
	bool rewind();
	off_t tell();

	ssize_t send(auto &output, off_t *srcOffset, size_t bytes)
	{
		if(srcOffset)
		{
			seek(*srcOffset);
		}
		ssize_t bytesToWrite = bytes;
		ssize_t totalBytesWritten = 0;
		while(bytesToWrite)
		{
			std::array<char, 4096> buff;
			ssize_t bytes = std::min((ssize_t)sizeof(buff), bytesToWrite);
			ssize_t bytesRead = read(buff.data(), bytes);
			if(bytesRead == 0)
				break;
			if(bytesRead == -1)
			{
				return -1;
			}
			ssize_t bytesWritten = output.write(buff.data(), bytes);
			if(bytesWritten == -1)
			{
				return -1;
			}
			totalBytesWritten += bytesWritten;
			bytesToWrite -= bytes;
		}
		return totalBytesWritten;
	}

	IOBuffer buffer(BufferMode mode = BufferMode::Direct);

	template <class T, bool useOffset = false>
	T getImpl(IOReadWriteResult<sizeof(T)> *resultOut = {}, off_t offset = 0)
	{
		if constexpr(std::is_same_v<T, bool>)
		{
			// special case to convert value to a valid bool
			return getImpl<uint8_t, useOffset>(resultOut, offset);
		}
		else
		{
			T obj;
			ssize_t size;
			if constexpr(useOffset)
				size = static_cast<IO*>(this)->readAtPos(static_cast<void*>(&obj), sizeof(T), offset);
			else
				size = static_cast<IO*>(this)->read(static_cast<void*>(&obj), sizeof(T));
			if(resultOut)
				*resultOut = size;
			if(size < (ssize_t)sizeof(T)) [[unlikely]]
				return {};
			return obj;
		}
	}

	template <class T>
	T get()
	{
		return getImpl<T>();
	}

	template <class T>
	T get(off_t offset)
	{
		return getImpl<T, true>({}, offset);
	}

	ssize_t readAtPosGeneric(void *buff, size_t bytes, off_t offset);

	ssize_t readSized(ResizableContainer auto &c, size_t maxSize)
	{
		if(c.max_size() < maxSize)
			return -1;
		using ReadResult = IOReadWriteResult<sizeof(*c.data())>;
		if constexpr(requires {c.resize_and_overwrite(maxSize, [](char*, std::size_t){return 0;});})
		{
			bool error{};
			c.resize_and_overwrite(maxSize, [&](char *str, std::size_t allocSize) -> ssize_t
			{
				ReadResult result = read(str, std::min(maxSize, allocSize));
				if(result.bytes == -1) [[unlikely]]
				{
					error = true;
					return {};
				}
				return result.items;
			});
			if(error) [[unlikely]]
				return -1;
			return c.size();
		}
		else
		{
			c.resize(maxSize);
			ReadResult result = read(c.data(), maxSize);
			if(result.bytes == -1) [[unlikely]]
				return -1;
			c.resize(result.items);
			return result.items;
		}
	}

	// read/write objects
	template <NotPointerDecayable T>
	IOReadWriteResult<sizeof(T)> read(T &obj)
	{
		if constexpr(std::is_same_v<T, bool>)
		{
			// special case to convert byte value to a valid bool
			IOReadWriteResult<sizeof(bool)> result;
			obj = getImpl<uint8_t>(&result);
			return result;
		}
		else
		{
			return static_cast<IO*>(this)->read(static_cast<void*>(&obj), sizeof(T));
		}
	}

	template <NotPointerDecayable T>
	ssize_t write(T &&obj)
	{
		return static_cast<IO*>(this)->write(static_cast<const void*>(&obj), sizeof(T));
	}

	// read/write whole spans
	template <class T>
	IOReadWriteResult<sizeof(T)> read(std::span<T> span)
	{
		return static_cast<IO*>(this)->read(static_cast<void*>(span.data()), span.size_bytes());
	}

	template <class T>
	IOReadWriteResult<sizeof(T)> write(std::span<T> span)
	{
		return static_cast<IO*>(this)->write(static_cast<const void*>(span.data()), span.size_bytes());
	}

	// read/write pointer data by element
	auto read(Pointer auto ptr, size_t size) -> IOReadWriteResult<sizeof(*ptr)>
	{
		return static_cast<IO*>(this)->read(static_cast<void*>(ptr), size * sizeof(*ptr));
	}

	auto write(Pointer auto ptr, size_t size) -> IOReadWriteResult<sizeof(*ptr)>
	{
		return static_cast<IO*>(this)->write(static_cast<const void*>(ptr), size * sizeof(*ptr));
	}

	// only return basic size for byte-sized pointers
	ssize_t read(PointerOfSize<1> auto ptr, size_t size)
	{
		return static_cast<IO*>(this)->read(static_cast<void*>(ptr), size);
	}

	ssize_t write(PointerOfSize<1> auto ptr, size_t size)
	{
		return static_cast<IO*>(this)->write(static_cast<const void*>(ptr), size);
	}

	FILE *toFileStream(const char *opentype);
};

}
