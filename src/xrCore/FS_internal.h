#ifndef FS_internalH
#define FS_internalH
#pragma once

#include "lzhuf.h"
#include <io.h>
#include <fcntl.h>
#include <sys\stat.h>
#include <share.h>

void* FileDownload(LPCSTR fn, u32* pdwSize = NULL);
void FileCompress(const char* fn, const char* sign, void* data, u32 size);
void* FileDecompress(const char* fn, const char* sign, u32* size = NULL);

class CFileWriter : public IWriter
{
private:
	FILE* hf;
	bool m_error;

	void release_read_only_attribute()
	{
		DWORD dwAttr = GetFileAttributes(fName.c_str());
		if ((dwAttr != u32(-1)) && (dwAttr & FILE_ATTRIBUTE_READONLY))
		{
			dwAttr &= ~FILE_ATTRIBUTE_READONLY;
			SetFileAttributes(fName.c_str(), dwAttr);
		}
	}

public:
	CFileWriter(const char* name, bool exclusive) : hf(nullptr), m_error(false)
	{
		R_ASSERT(name && name[0]);
		fName = name;
		VerifyPath(fName.c_str());
		if (exclusive)
		{
			const int handle = _sopen(fName.c_str(), _O_WRONLY | _O_TRUNC | _O_CREAT | _O_BINARY, SH_DENYWR);
			if (handle == -1)
			{
				m_error = true;
				Msg("! Can't create file: '%s'. Error: '%s'.", fName.c_str(), _sys_errlist[errno]);
				return;
			}

			hf = _fdopen(handle, "wb");
			if (!hf)
			{
				m_error = true;
				_close(handle);
				Msg("! Can't open file stream: '%s'. Error: '%s'.", fName.c_str(), _sys_errlist[errno]);
			}
		}
		else
		{
			hf = fopen(fName.c_str(), "wb");
			if (!hf)
			{
				m_error = true;
				Msg("! Can't write file: '%s'. Error: '%s'.", fName.c_str(), _sys_errlist[errno]);
			}
		}
	}

	virtual ~CFileWriter()
	{
		if (hf)
		{
			if (fclose(hf) != 0)
				m_error = true;
			hf = nullptr;
			release_read_only_attribute();
		}
	}

	virtual void w(const void* _ptr, u32 count)
	{
		if (!count)
			return;

		if (!hf || m_error)
		{
			m_error = true;
			return;
		}

		constexpr u32 block_size_limit = 0x1000000;
		const u8* ptr = static_cast<const u8*>(_ptr);
		u32 remaining = count;
		while (remaining)
		{
			const u32 block_size = remaining < block_size_limit ? remaining : block_size_limit;
			const size_t written = fwrite(ptr, 1, block_size, hf);
			if (written != block_size)
			{
				m_error = true;
				Msg("! Can't write mem block to file '%s'. Disk may be full. Error: '%s'.",
					fName.c_str(), _sys_errlist[errno]);
				return;
			}

			ptr += block_size;
			remaining -= block_size;
		}
	}

	virtual void seek(u32 pos)
	{
		if (!hf || fseek(hf, pos, SEEK_SET) != 0)
			m_error = true;
	}

	virtual u32 tell()
	{
		if (!hf)
		{
			m_error = true;
			return 0;
		}

		const long position = ftell(hf);
		if (position < 0)
		{
			m_error = true;
			return 0;
		}

		return u32(position);
	}

	virtual bool valid() { return hf != nullptr && !m_error; }

	virtual void flush()
	{
		if (!hf || fflush(hf) != 0)
			m_error = true;
	}

	virtual bool finalize()
	{
		if (!hf)
			return false;

		if (fflush(hf) != 0)
		{
			m_error = true;
			Msg("! Can't flush file '%s'. Error: '%s'.", fName.c_str(), _sys_errlist[errno]);
		}
		if (fclose(hf) != 0)
		{
			m_error = true;
			Msg("! Can't close file '%s'. Error: '%s'.", fName.c_str(), _sys_errlist[errno]);
		}
		hf = nullptr;
		release_read_only_attribute();
		return !m_error;
	}
};

// It automatically frees memory after destruction
class CTempReader : public IReader
{
public:
	CTempReader(void* _data, int _size, int _iterpos) : IReader(_data, _size, _iterpos)
	{
	}

	virtual ~CTempReader();
};

class CPackReader : public IReader
{
	void* base_address;
public:
	CPackReader(void* _base, void* _data, int _size) : IReader(_data, _size) { base_address = _base; }
	virtual ~CPackReader();
};

class XRCORE_API CFileReader : public IReader
{
public:
	CFileReader(const char* name);
	virtual ~CFileReader();
};

class CCompressedReader : public IReader
{
public:
	CCompressedReader(const char* name, const char* sign);
	virtual ~CCompressedReader();
};

class CVirtualFileReader : public IReader
{
private:
	void *hSrcFile, *hSrcMap;
public:
	CVirtualFileReader(const char* cFileName);
	virtual ~CVirtualFileReader();
};

#endif
