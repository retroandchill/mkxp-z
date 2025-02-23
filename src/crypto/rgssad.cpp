/*
** rgssad.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2014 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "rgssad.h"
#include "boost-hash.h"
#include "util.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <memory>

/* Equivalent Linear Congruential Generator (LCG) constants for iteration 2^n
 * all the way up to 2^32/4 (the largest dword offset possible in
 * RGSS{AD,[23]A}).
 *
 * This table can be easily calculated by taking the original LCG parameter
 * m[0] and a[0] and write them down as LCG_TABLE[0]. Then for the rest of
 * the 29 entries, LGC_TABLE[n] = {m[n], a[n]} where m[n] = pow(m[n-1], 2)
 * and a[n] = a[n-1] * (m[n-1] + 1). */
constexpr static uint32_t LCG_TABLE[30][2] = {
    {0x00000007, 0x00000003}, {0x00000031, 0x00000018},
    {0x00000961, 0x000004b0}, {0x0057f6c1, 0x002bfb60},
    {0xa5057d81, 0xd282bec0}, {0x6e913b01, 0x37489d80},
    {0xc0bb7601, 0x605dbb00}, {0x1bdaec01, 0x8ded7600},
    {0x0145d801, 0x80a2ec00}, {0x28cbb001, 0x1465d800},
    {0xea976001, 0x754bb000}, {0x392ec001, 0x1c976000},
    {0x025d8001, 0x012ec000}, {0x44bb0001, 0x225d8000},
    {0x89760001, 0xc4bb0000}, {0x12ec0001, 0x89760000},
    {0x25d80001, 0x12ec0000}, {0x4bb00001, 0x25d80000},
    {0x97600001, 0x4bb00000}, {0x2ec00001, 0x97600000},
    {0x5d800001, 0x2ec00000}, {0xbb000001, 0x5d800000},
    {0x76000001, 0xbb000000}, {0xec000001, 0x76000000},
    {0xd8000001, 0xec000000}, {0xb0000001, 0xd8000000},
    {0x60000001, 0xb0000000}, {0xc0000001, 0x60000000},
    {0x80000001, 0xc0000000}, {0x00000001, 0x80000000},
};

using PHYSFS_Io_Ptr = std::shared_ptr<PHYSFS_Io>;

PHYSFS_Io_Ptr createPhysfsIoPtr(PHYSFS_Io* io) {
    return PHYSFS_Io_Ptr(io, [](PHYSFS_Io *ptr) {
        ptr->destroy(ptr);
    });
}

struct RGSS_entryData
{
	int64_t offset;
	uint64_t size;
	uint32_t startMagic;
};

struct RGSS_entryHandle
{
	const RGSS_entryData data;
	uint32_t currentMagic;
	uint64_t currentOffset = 0;
    PHYSFS_Io_Ptr io = createPhysfsIoPtr(nullptr);

	RGSS_entryHandle(const RGSS_entryData &data, PHYSFS_Io *archIo)
	    : data(data),
	      currentMagic(data.startMagic),
          io(createPhysfsIoPtr(archIo->duplicate(archIo)))
    {
	}


};

struct RGSS_archiveData
{
	PHYSFS_Io *archiveIo;

	/* Maps: file path
	 * to:   entry data */
	BoostHash<std::string, RGSS_entryData> entryHash;

	/* Maps: directory path,
	 * to:   list of contained entries */
	BoostHash<std::string, BoostSet<std::string> > dirHash;

};

static bool
readUint32(PHYSFS_Io *io, uint32_t &result)
{
	char buff[4];
	PHYSFS_sint64 count = io->read(io, buff, 4);

	result = ((buff[0] << 0x00) & 0x000000FF) |
	         ((buff[1] << 0x08) & 0x0000FF00) |
	         ((buff[2] << 0x10) & 0x00FF0000) |
	         ((buff[3] << 0x18) & 0xFF000000) ;

	return (count == 4);
}

#define RGSS_HEADER "RGSSAD"
#define RGSS_MAGIC 0xDEADCAFE

#define PHYSFS_ALLOC(type) \
	static_cast<type*>(PHYSFS_getAllocator()->Malloc(sizeof(type)))

#define IO_READ(io, dest, size) (io->read(io, dest, size) == size)

static inline uint32_t
advanceMagic(uint32_t &magic)
{
	uint32_t old = magic;

	magic = magic * 7 + 3;

	return old;
}

static inline uint32_t
advanceMagicN(uint32_t &magic, uint32_t n) {
    uint32_t old = magic;
    int table_index = 0;

    while (n != 0) {
        if (n & 1) {
            magic = magic * LCG_TABLE[table_index][0] + LCG_TABLE[table_index][1];
        }
        n >>= 1;
        table_index++;
    }

    return old;
}

static PHYSFS_sint64
RGSS_ioRead(PHYSFS_Io *self, OpaquePtr buffer, PHYSFS_uint64 len)
{
    auto entry = static_cast<RGSS_entryHandle*>(self->opaque);

	PHYSFS_Io *io = entry->io.get();

	uint64_t toRead = std::min<uint64_t>(entry->data.size - entry->currentOffset, len);
	uint64_t offs = entry->currentOffset;

	io->seek(io, entry->data.offset + offs);

	/* We divide up the bytes to be read in 3 categories:
	 *
	 * preAlign: If the current read address is not dword
	 *   aligned, this is the number of bytes to read til
	 *   we reach alignment again (therefore can only be
	 *   3 or less).
	 *
	 * align: The number of aligned dwords we can read
	 *   times 4 (= number of bytes).
	 *
	 * postAlign: The number of bytes to read after the
	 *   last aligned dword. Always 3 or less.
	 *
	 * Treating the pre- and post aligned reads specially,
	 * we can read all aligned dwords in one syscall directly
	 * into the write buffer and then run the xor chain on
	 * it afterwards. */

	uint8_t preAlign = 4 - (offs % 4);

	if (preAlign == 4)
		preAlign = 0;
	else
		preAlign = std::min<uint64_t>(preAlign, len);

	uint8_t postAlign = (len > preAlign) ? (offs + len) % 4 : 0;

	uint64_t align = len - (preAlign + postAlign);

	/* Byte buffer pointer */
	uint8_t *bBufferP = static_cast<uint8_t*>(buffer);

	if (preAlign > 0)
	{
		uint32_t dword;
		io->read(io, &dword, preAlign);

		/* Need to align the bytes with the
		 * magic before xoring */
		dword <<= 8 * (offs % 4);
		dword ^= entry->currentMagic;

		/* Shift them back to normal */
		dword >>= 8 * (offs % 4);
		memcpy(bBufferP, &dword, preAlign);

		bBufferP += preAlign;

		/* Only advance the magic if we actually
		 * reached the next alignment */
		if ((offs+preAlign) % 4 == 0)
			advanceMagic(entry->currentMagic);
	}

	if (align > 0)
	{
		/* Double word buffer pointer */
		uint32_t *dwBufferP = reinterpret_cast<uint32_t*>(bBufferP);

		/* Read aligned dwords in one go */
		io->read(io, bBufferP, align);

		/* Then xor them */
		for (uint64_t i = 0; i < (align / 4); ++i)
			dwBufferP[i] ^= advanceMagic(entry->currentMagic);

		bBufferP += align;
	}

	if (postAlign > 0)
	{
		uint32_t dword;
		io->read(io, &dword, postAlign);

		/* Bytes are already aligned with magic */
		dword ^= entry->currentMagic;
		memcpy(bBufferP, &dword, postAlign);
	}

	entry->currentOffset += toRead;

	return toRead;
}

static int
RGSS_ioSeek(PHYSFS_Io *self, PHYSFS_uint64 offset)
{
	RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	if (offset == entry->currentOffset)
		return 1;

	if (offset > entry->data.size-1)
		return 0;

	/* If rewinding, we need to rewind to begining */
	if (offset < entry->currentOffset)
	{
		entry->currentOffset = 0;
		entry->currentMagic = entry->data.startMagic;
	}

	/* For each overstepped alignment, advance magic */
	uint64_t currentDword = entry->currentOffset / 4;
	uint64_t targetDword  = offset / 4;
	uint64_t dwordsSought = targetDword - currentDword;

	advanceMagicN(entry->currentMagic, (uint32_t) dwordsSought);

	entry->currentOffset = offset;
	entry->io->seek(entry->io.get(), entry->data.offset + entry->currentOffset);

	return 1;
}

static PHYSFS_sint64
RGSS_ioTell(PHYSFS_Io *self)
{
	const RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	return entry->currentOffset;
}

static PHYSFS_sint64
RGSS_ioLength(PHYSFS_Io *self)
{
	const RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	return entry->data.size;
}

static PHYSFS_Io*
RGSS_ioDuplicate(PHYSFS_Io *self)
{
	const RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);
	auto entryDup = std::make_unique<RGSS_entryHandle>(*entry);

	auto dup = PHYSFS_ALLOC(PHYSFS_Io);
	*dup = *self;
	dup->opaque = entryDup.release();

	return dup;
}

static void
RGSS_ioDestroy(PHYSFS_Io *self)
{
    // Place the object into a managed container again after previously releasing it
	auto entry = static_cast<RGSS_entryHandle*>(self->opaque);
    std::unique_ptr<RGSS_entryHandle> disposer(entry);

	PHYSFS_getAllocator()->Free(self);
}

static const PHYSFS_Io RGSS_IoTemplate =
{
    0, /* version */
    nullptr, /* opaque */
    RGSS_ioRead,
    nullptr, /* write */
    RGSS_ioSeek,
    RGSS_ioTell,
    RGSS_ioLength,
    RGSS_ioDuplicate,
    nullptr, /* flush */
    RGSS_ioDestroy
};

static void
processDirectories(RGSS_archiveData *data, BoostSet<std::string> &topLevel,
                   char *nameBuf, uint32_t nameLen)
{
	/* Check for top level entries */
	for (uint32_t i = 0; i < nameLen; ++i)
	{
		bool slash = nameBuf[i] == '/';
		if (!slash && i+1 < nameLen)
			continue;

		if (slash)
			nameBuf[i] = '\0';

		topLevel.insert(nameBuf);

		if (slash)
			nameBuf[i] = '/';

		break;
	}

	/* Check for more entries */
	for (uint32_t i = nameLen; i > 0; i--)
		if (nameBuf[i] == '/')
		{
			nameBuf[i] = '\0';

			const char *dir = nameBuf;
			const char *entry = &nameBuf[i+1];

			BoostSet<std::string> &entryList = data->dirHash[dir];
			entryList.insert(entry);
		}
}

static bool
verifyHeader(PHYSFS_Io *io, char version)
{
	char header[8];

	if (!IO_READ(io, header, sizeof(header)))
		return false;

	if (strcmp(header, RGSS_HEADER))
		return false;

	if (header[7] != version)
		return false;

	return true;
}

static OpaquePtr
RGSS_openArchive(PHYSFS_Io *io, const char *, int forWrite, int *claimed)
{
	if (forWrite)
		return nullptr;

	/* Version 1 */
	if (!verifyHeader(io, 1))
		return nullptr;
	else
		*claimed = 1;

	auto data = std::make_unique<RGSS_archiveData>();
	data->archiveIo = io;

	uint32_t magic = RGSS_MAGIC;

	/* Top level entry list */
	BoostSet<std::string> &topLevel = data->dirHash[""];

	while (true)
	{
		/* Read filename length,
         * if nothing was read, no files remain */
		uint32_t nameLen;

		if (!readUint32(io, nameLen))
			break;

		nameLen ^= advanceMagic(magic);

		static char nameBuf[512];
		for (uint32_t i = 0; i < nameLen; ++i)
		{
			char c;
			io->read(io, &c, 1);
			nameBuf[i] = c ^ (advanceMagic(magic) & 0xFF);
			if (nameBuf[i] == '\\')
				nameBuf[i] = '/';
		}

		nameBuf[nameLen] = '\0';

		uint32_t entrySize;
		readUint32(io, entrySize);
		entrySize ^= advanceMagic(magic);

		RGSS_entryData entry;
		entry.offset = io->tell(io);
		entry.size = entrySize;
		entry.startMagic = magic;

		data->entryHash.insert(nameBuf, entry);
		processDirectories(data.get(), topLevel, nameBuf, nameLen);

		io->seek(io, entry.offset + entry.size);
	}

	return data.release();
}

static PHYSFS_EnumerateCallbackResult
RGSS_enumerateFiles(OpaquePtr opaque, const char *dirname,
                    PHYSFS_EnumerateCallback cb,
                    const char *origdir, OpaquePtr callbackdata)
{
	RGSS_archiveData *data = static_cast<RGSS_archiveData*>(opaque);

	std::string _dirname(dirname);

	if (!data->dirHash.contains(_dirname))
		return PHYSFS_ENUM_STOP;

	const BoostSet<std::string> &entries = data->dirHash[_dirname];

	BoostSet<std::string>::const_iterator iter;
	for (iter = entries.cbegin(); iter != entries.cend(); ++iter)
		cb(callbackdata, origdir, iter->c_str());

	return PHYSFS_ENUM_OK;
}

static PHYSFS_Io*
RGSS_openRead(OpaquePtr opaque, const char *filename)
{
    auto data = static_cast<RGSS_archiveData*>(opaque);

	if (!data->entryHash.contains(filename))
		return nullptr;

	auto entry = std::make_unique<RGSS_entryHandle>(data->entryHash[filename], data->archiveIo);

    auto *io = PHYSFS_ALLOC(PHYSFS_Io);

	*io = RGSS_IoTemplate;
	io->opaque = entry.release();

	return io;
}

static int
RGSS_stat(OpaquePtr opaque, const char *filename, PHYSFS_Stat *stat)
{
    auto data = static_cast<RGSS_archiveData*>(opaque);

	bool hasFile = data->entryHash.contains(filename);
	bool hasDir  = data->dirHash.contains(filename);

	if (!hasFile && !hasDir)
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_NOT_FOUND);
		return 0;
	}

	stat->modtime    =
	stat->createtime =
	stat->accesstime = 0;
	stat->readonly   = 1;

	if (hasFile)
	{
		const RGSS_entryData &entry = data->entryHash[filename];

		stat->filesize = entry.size;
		stat->filetype = PHYSFS_FILETYPE_REGULAR;
	}
	else
	{
		stat->filesize = 0;
		stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
	}

	return 1;
}

static void
RGSS_closeArchive(OpaquePtr opaque)
{
    // Return the unmanaged memory into a smart pointer
    auto data = static_cast<RGSS_archiveData*>(opaque);
    std::unique_ptr<RGSS_archiveData> managed(data);
}

static PHYSFS_Io*
RGSS_noop1(const OpaquePtr, const char*)
{
	return nullptr;
}

static int
RGSS_noop2(const OpaquePtr, const char*)
{
	return 0;
}

const PHYSFS_Archiver RGSS1_Archiver =
{
	0,
	{
		"RGSSAD",
		"RGSS encrypted archive format",
		"", /* Author */
		"", /* Website */
		0 /* symlinks not supported */
	},
	RGSS_openArchive,
	RGSS_enumerateFiles,
	RGSS_openRead,
	RGSS_noop1, /* openWrite */
	RGSS_noop1, /* openAppend */
	RGSS_noop2, /* remove */
	RGSS_noop2, /* mkdir */
	RGSS_stat,
	RGSS_closeArchive
};

const PHYSFS_Archiver RGSS2_Archiver =
{
	0,
	{
		"RGSS2A",
		"RGSS2 encrypted archive format",
		"", /* Author */
		"", /* Website */
		0 /* symlinks not supported */
	},
	RGSS_openArchive,
	RGSS_enumerateFiles,
	RGSS_openRead,
	RGSS_noop1, /* openWrite */
	RGSS_noop1, /* openAppend */
	RGSS_noop2, /* remove */
	RGSS_noop2, /* mkdir */
	RGSS_stat,
	RGSS_closeArchive
};

static bool
readUint32AndXor(PHYSFS_Io *io, uint32_t &result, uint32_t key)
{
	if (!readUint32(io, result))
		return false;

	result ^= key;

	return true;
}

static OpaquePtr
RGSS3_openArchive(PHYSFS_Io *io, const char *, int forWrite, int *claimed)
{
	if (forWrite)
		return nullptr;

	/* Version 3 */
	if (!verifyHeader(io, 3))
		return nullptr;
	else
		*claimed = 1;

	uint32_t baseMagic;

	if (!readUint32(io, baseMagic))
		return nullptr;

	baseMagic = (baseMagic * 9) + 3;

	auto data = std::make_unique<RGSS_archiveData>();
	data->archiveIo = io;

	/* Top level entry list */
	BoostSet<std::string> &topLevel = data->dirHash[""];

	while (true)
	{
		uint32_t offset, size, magic, nameLen;

		if (!readUint32AndXor(io, offset, baseMagic))
			goto error;

		/* Zero offset means entry list has ended */
		if (offset == 0)
			break;

		if (!readUint32AndXor(io, size, baseMagic))
			goto error;

		if (!readUint32AndXor(io, magic, baseMagic))
			goto error;

		if (!readUint32AndXor(io, nameLen, baseMagic))
			goto error;

		char nameBuf[512];

		if (!IO_READ(io, nameBuf, nameLen))
			goto error;

		for (uint32_t i = 0; i < nameLen; ++i)
		{
			nameBuf[i] ^= ((baseMagic >> 8*(i%4)) & 0xFF);

			if (nameBuf[i] == '\\')
				nameBuf[i] = '/';
		}

		nameBuf[nameLen] = '\0';

		RGSS_entryData entry;
		entry.offset = offset;
		entry.size = size;
		entry.startMagic = magic;

		data->entryHash.insert(nameBuf, entry);
		processDirectories(data.get(), topLevel, nameBuf, nameLen);

		continue;

	error:
		return nullptr;
	}

    // Release the ownership and return
	return data.release();
}

const PHYSFS_Archiver RGSS3_Archiver =
{
	0,
	{
		"RGSS3A",
		"RGSS3 encrypted archive format",
		"", /* Author */
		"", /* Website */
		0 /* symlinks not supported */
	},
	RGSS3_openArchive,
	RGSS_enumerateFiles,
	RGSS_openRead,
	RGSS_noop1, /* openWrite */
	RGSS_noop1, /* openAppend */
	RGSS_noop2, /* remove */
	RGSS_noop2, /* mkdir */
	RGSS_stat,
	RGSS_closeArchive
};
